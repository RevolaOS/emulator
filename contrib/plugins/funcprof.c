/*
 * funcprof.c -- per-function cost profiler for iPod-Classic guest code.
 *
 * Measures every call of a set of watched guest functions without halting the
 * CPU (gdb breakpoints desynchronise the click-wheel latch; see pctrace.c):
 *
 *   calls       number of completed calls
 *   incl        guest instructions executed from entry to return, children
 *               included (the primary metric: deterministic for identical input)
 *   self        incl minus the incl of watched callees
 *   max         largest single-call incl
 *   host_ns     host wall time spent emulating those calls
 *   unclosed    entries whose return was never observed (tail-jump hooks,
 *               longjmp, task never resumed); they contribute no cost
 *
 * Instruction counts are TCG instructions retired, not ARM926 cycles: they do
 * not model caches, wait states, or the cost of individual instructions. Use
 * them to compare two versions of the same code under the same input; take
 * absolute timings on the device. Interrupts and other RTXC tasks that run
 * while a call is open are included in that call's cost.
 *
 * HOW CALLS ARE MATCHED. Every TB gets one exec callback. When a TB starts at
 * a watched entry, the callback reads LR and SP (authoritative only at a TB
 * head under the 8.2 register-read backport, see plugins/api.c) and pushes a
 * frame. A later TB whose start PC equals an open frame's return address and
 * whose SP equals that frame's entry SP closes the frame (AAPCS restores SP on
 * return). Matching on SP keeps recursion and RTXC task switches apart; frames
 * above the matched one are discarded as unclosed, and frames below it with
 * the same return address and SP (tail calls, interworking veneers) close too.
 * Re-entering a function with an open frame's LR and SP continues that frame
 * (loops at the entry). Hooks entered by a patched branch carry a stale LR and
 * never close; they are reported as unclosed when dropped.
 *
 * Build:   make -C build/contrib/plugins libfuncprof.so
 * Use:     -plugin build/contrib/plugins/libfuncprof.so,syms=funcs.txt,...
 *
 * Arguments (comma separated):
 *   syms=PATH       watched functions, one "0xADDR NAME" per line ('#' comments)
 *   fn=0xADDR:NAME  add one watched function (repeatable)
 *   ctl=PATH        FIFO for commands: "reset", "start", "stop", "dump PATH".
 *                   Commands run on the vCPU thread at the next TB, so a paused
 *                   VM executes them after "cont".
 *   out=PATH        write a final dump to PATH at exit
 *   record=off      start with recording stopped (default on)
 *   trace=on        keep per-call events for a Chrome/Perfetto trace
 *   tracecap=N      maximum kept events (default 1000000)
 *   timeline=insns  trace timestamps in guest instructions (1 us == 1 insn)
 *                   instead of host time
 *   signpost=on     emit os_signpost intervals (macOS) in the
 *                   "PointsOfInterest" category of subsystem
 *                   "org.qemu.ipod.funcprof", visible in Instruments
 *
 * A dump writes PATH (summary JSON) and, with trace=on, PATH.trace.json. Both
 * appear atomically (written to a temporary name, then renamed).
 *
 * License: GNU GPL, version 2 or later (see COPYING).
 */
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
#include <os/log.h>
#include <os/signpost.h>
#endif

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAX_DEPTH 64
#define REG_SP 13
#define REG_LR 14

typedef struct {
    uint32_t addr;
    char *name;
    guint64 calls;
    guint64 incl;
    guint64 self;
    guint64 max;
    guint64 host_ns;
    guint64 unclosed;
} Func;

typedef struct {
    Func *func;
    uint32_t ret;
    uint32_t sp;
    guint64 insn_start;
    guint64 child;
    guint64 host_start;
    bool valid;                 /* false: opened before the last reset */
#ifdef __APPLE__
    os_signpost_id_t signpost;
#endif
} Frame;

typedef struct {
    Func *func;
    guint64 start;              /* host ns or instruction count */
    guint64 dur;
    guint64 incl;
    unsigned depth;
} Event;

typedef struct {
    Func *func;
    unsigned n_insns;
} EntryTB;

static GHashTable *funcs;       /* addr -> Func* */
static Frame stack[MAX_DEPTH];
static unsigned depth;
static guint64 icount;          /* instructions executed before the current TB */
static guint64 record_icount;   /* instructions executed while recording */
static guint64 record_host_ns;  /* host time spent recording */
static guint64 record_since;    /* host ns when recording last started */
static bool recording = true;
static bool tracing;
static bool timeline_insns;
static guint64 tracecap = 1000000;
static guint64 events_dropped;
static GArray *events;          /* Event */
static GByteArray *reg_buf;
static char *out_path;
static char *ctl_path;
static GAsyncQueue *commands;
static gint pending;

#ifdef __APPLE__
static bool signposts;
static os_log_t signpost_log;
#endif

static guint64 host_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (guint64)ts.tv_sec * 1000000000ull + (guint64)ts.tv_nsec;
}

static uint32_t read_reg32(int reg)
{
    g_byte_array_set_size(reg_buf, 0);
    if (qemu_plugin_read_register(reg_buf, reg) < 4) {
        return 0;
    }
    return (uint32_t)reg_buf->data[0] | ((uint32_t)reg_buf->data[1] << 8) |
           ((uint32_t)reg_buf->data[2] << 16) |
           ((uint32_t)reg_buf->data[3] << 24);
}

static void add_func(uint32_t addr, const char *name)
{
    addr &= ~1u;                /* Thumb symbols carry bit 0 */
    if (g_hash_table_contains(funcs, GUINT_TO_POINTER(addr))) {
        return;
    }
    Func *f = g_new0(Func, 1);
    f->addr = addr;
    f->name = g_strdup(name);
    g_hash_table_insert(funcs, GUINT_TO_POINTER(addr), f);
}

static void load_syms(const char *path)
{
    char *text;
    GError *error = NULL;
    if (!g_file_get_contents(path, &text, NULL, &error)) {
        fprintf(stderr, "funcprof: %s\n", error->message);
        g_error_free(error);
        return;
    }
    char **lines = g_strsplit(text, "\n", -1);
    for (char **line = lines; *line; line++) {
        char *s = g_strstrip(*line);
        if (!*s || *s == '#') {
            continue;
        }
        char *end;
        guint64 addr = g_ascii_strtoull(s, &end, 0);
        if (end == s) {
            continue;
        }
        add_func((uint32_t)addr, g_strstrip(end));
    }
    g_strfreev(lines);
    g_free(text);
}

static void signpost_end(Frame *fr, guint64 incl, bool closed)
{
#ifdef __APPLE__
    if (signposts && fr->signpost != OS_SIGNPOST_ID_NULL) {
        if (closed) {
            os_signpost_interval_end(signpost_log, fr->signpost, "call",
                                     "%{public}s insns=%llu",
                                     fr->func->name, (unsigned long long)incl);
        } else {
            os_signpost_interval_end(signpost_log, fr->signpost, "call",
                                     "%{public}s unclosed", fr->func->name);
        }
    }
#endif
}

/* Drop stack[k] without a return; its watched callees' cost stays with its caller. */
static void discard(unsigned k)
{
    Frame *fr = &stack[k];
    if (k > 0) {
        stack[k - 1].child += fr->child;
    }
    if (fr->valid && recording) {
        fr->func->unclosed++;
    }
    signpost_end(fr, 0, false);
}

static void close_frame(unsigned k, guint64 now)
{
    Frame *fr = &stack[k];
    guint64 incl = icount - fr->insn_start;

    if (k > 0) {
        stack[k - 1].child += incl;
    }
    if (!fr->valid || !recording) {
        signpost_end(fr, incl, true);
        return;
    }
    Func *f = fr->func;
    guint64 host = now - fr->host_start;
    f->calls++;
    f->incl += incl;
    f->self += incl > fr->child ? incl - fr->child : 0;
    f->host_ns += host;
    if (incl > f->max) {
        f->max = incl;
    }
    signpost_end(fr, incl, true);
    if (tracing) {
        if (events->len < tracecap) {
            Event e = {
                f, timeline_insns ? fr->insn_start : fr->host_start,
                timeline_insns ? incl : host, incl, k,
            };
            g_array_append_val(events, e);
        } else {
            events_dropped++;
        }
    }
}

static void check_return(uint32_t pc)
{
    uint32_t sp = 0;
    bool have_sp = false;

    for (unsigned k = depth; k-- > 0;) {
        if (stack[k].ret != pc) {
            continue;
        }
        if (!have_sp) {
            sp = read_reg32(REG_SP);
            have_sp = true;
        }
        if (stack[k].sp != sp) {
            continue;
        }
        while (depth > k + 1) {
            discard(--depth);
        }
        guint64 now = host_now();
        close_frame(k, now);
        depth = k;
        /* Tail calls (veneers, b-to-function) share the caller's LR and SP. */
        while (depth && stack[depth - 1].ret == pc && stack[depth - 1].sp == sp) {
            close_frame(--depth, now);
        }
        return;
    }
}

static void remove_frame(unsigned k)
{
    discard(k);
    memmove(&stack[k], &stack[k + 1], sizeof(Frame) * (depth - k - 1));
    depth--;
}

static void push_frame(Func *f)
{
    uint32_t ret = read_reg32(REG_LR) & ~1u;
    uint32_t sp = read_reg32(REG_SP);

    /*
     * Re-entry with the same return address and SP continues the open
     * activation: a loop whose head is the function entry, or a hook entered
     * by a patched branch (stale LR, never returns to it). Recursion runs at a
     * deeper SP. Frames opened above it since then can no longer return.
     */
    for (unsigned k = depth; k-- > 0;) {
        if (stack[k].func == f && stack[k].ret == ret && stack[k].sp == sp) {
            while (depth > k + 1) {
                discard(--depth);
            }
            return;
        }
    }
    if (depth == MAX_DEPTH) {
        remove_frame(0);
    }
    Frame *fr = &stack[depth++];
    fr->func = f;
    fr->ret = ret;
    fr->sp = sp;
    fr->insn_start = icount;
    fr->child = 0;
    fr->host_start = host_now();
    fr->valid = recording;
#ifdef __APPLE__
    fr->signpost = OS_SIGNPOST_ID_NULL;
    if (signposts && recording && os_signpost_enabled(signpost_log)) {
        fr->signpost = os_signpost_id_generate(signpost_log);
        os_signpost_interval_begin(signpost_log, fr->signpost, "call",
                                   "%{public}s", f->name);
    }
#endif
}

/* ---- dump ------------------------------------------------------------- */

static void json_string(GString *s, const char *text)
{
    g_string_append_c(s, '"');
    for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\') {
            g_string_append_c(s, '\\');
        }
        if ((unsigned char)*p < 0x20) {
            g_string_append_printf(s, "\\u%04x", *p);
        } else {
            g_string_append_c(s, *p);
        }
    }
    g_string_append_c(s, '"');
}

static void write_atomic(const char *path, GString *s)
{
    char *tmp = g_strdup_printf("%s.tmp", path);
    GError *error = NULL;
    if (!g_file_set_contents(tmp, s->str, s->len, &error)) {
        fprintf(stderr, "funcprof: %s\n", error->message);
        g_error_free(error);
    } else if (rename(tmp, path) != 0) {
        fprintf(stderr, "funcprof: rename %s: %s\n", path, strerror(errno));
    }
    g_free(tmp);
}

static gint by_incl(gconstpointer a, gconstpointer b)
{
    const Func *fa = *(Func *const *)a, *fb = *(Func *const *)b;
    return fa->incl < fb->incl ? 1 : fa->incl > fb->incl ? -1 : 0;
}

static void write_trace(const char *path)
{
    /* Chrome trace format: ts/dur are microseconds; keep ns/insn resolution. */
    double scale = timeline_insns ? 1.0 : 0.001;
    guint64 base = events->len ? g_array_index(events, Event, 0).start : 0;
    for (guint i = 0; i < events->len; i++) {
        Event *e = &g_array_index(events, Event, i);
        if (e->start < base) {
            base = e->start;
        }
    }
    GString *t = g_string_new("{\"displayTimeUnit\": \"ns\", \"traceEvents\": [\n");
    g_string_append_printf(t, "{\"ph\": \"M\", \"pid\": 1, \"tid\": 1, "
                           "\"name\": \"thread_name\", \"args\": {\"name\": "
                           "\"guest (%s)\"}}",
                           timeline_insns ? "1 us = 1 instruction"
                                          : "host time");
    for (guint i = 0; i < events->len; i++) {
        Event *e = &g_array_index(events, Event, i);
        g_string_append(t, ",\n{\"ph\": \"X\", \"pid\": 1, \"tid\": 1, \"name\": ");
        json_string(t, e->func->name);
        g_string_append_printf(t, ", \"ts\": %.3f, \"dur\": %.3f, "
                               "\"args\": {\"insns\": %" PRIu64
                               ", \"depth\": %u}}",
                               (e->start - base) * scale, e->dur * scale,
                               e->incl, e->depth);
    }
    g_string_append(t, "\n]}\n");
    char *trace_path = g_strdup_printf("%s.trace.json", path);
    write_atomic(trace_path, t);
    g_free(trace_path);
    g_string_free(t, true);
}

static void dump(const char *path)
{
    guint64 host = record_host_ns + (recording ? host_now() - record_since : 0);
    /* The trace goes first: the summary's appearance signals a complete dump. */
    if (events) {
        write_trace(path);
    }
    GPtrArray *list = g_hash_table_get_values_as_ptr_array(funcs);
    g_ptr_array_sort(list, by_incl);

    GString *s = g_string_new("{\n");
    g_string_append_printf(s, "  \"recording\": %s,\n",
                           recording ? "true" : "false");
    g_string_append_printf(s, "  \"insns\": %" PRIu64 ",\n", record_icount);
    g_string_append_printf(s, "  \"host_ns\": %" PRIu64 ",\n", host);
    g_string_append(s, "  \"open_frames\": [");
    for (unsigned k = 0; k < depth; k++) {
        g_string_append(s, k ? ", {\"name\": " : "{\"name\": ");
        json_string(s, stack[k].func->name);
        g_string_append_printf(s, ", \"ret\": \"0x%08" PRIx32 "\", \"sp\": \"0x%08"
                               PRIx32 "\"}", stack[k].ret, stack[k].sp);
    }
    g_string_append(s, "],\n");
    g_string_append_printf(s, "  \"events\": %u,\n",
                           events ? events->len : 0);
    g_string_append_printf(s, "  \"events_dropped\": %" PRIu64 ",\n",
                           events_dropped);
    g_string_append(s, "  \"functions\": [");
    bool first = true;
    for (guint i = 0; i < list->len; i++) {
        Func *f = g_ptr_array_index(list, i);
        if (!f->calls && !f->unclosed) {
            continue;
        }
        g_string_append(s, first ? "\n    {" : ",\n    {");
        first = false;
        g_string_append(s, "\"name\": ");
        json_string(s, f->name);
        g_string_append_printf(s, ", \"addr\": \"0x%08" PRIx32 "\", "
                               "\"calls\": %" PRIu64 ", \"incl\": %" PRIu64
                               ", \"self\": %" PRIu64 ", \"max\": %" PRIu64
                               ", \"host_ns\": %" PRIu64
                               ", \"unclosed\": %" PRIu64 "}",
                               f->addr, f->calls, f->incl, f->self, f->max,
                               f->host_ns, f->unclosed);
    }
    g_string_append(s, "\n  ]\n}\n");
    write_atomic(path, s);
    g_string_free(s, true);
    g_ptr_array_free(list, true);
}

/* ---- commands --------------------------------------------------------- */

static void set_recording(bool on)
{
    if (on == recording) {
        return;
    }
    guint64 now = host_now();
    if (on) {
        record_since = now;
    } else {
        record_host_ns += now - record_since;
    }
    recording = on;
}

static void reset(void)
{
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, funcs);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        Func *f = value;
        f->calls = f->incl = f->self = f->max = f->host_ns = f->unclosed = 0;
    }
    for (unsigned k = 0; k < depth; k++) {
        stack[k].valid = false;
    }
    record_icount = 0;
    record_host_ns = 0;
    record_since = host_now();
    events_dropped = 0;
    if (events) {
        g_array_set_size(events, 0);
    }
}

static void run_command(char *line)
{
    char *cmd = g_strstrip(line);
    if (g_str_equal(cmd, "reset")) {
        reset();
    } else if (g_str_equal(cmd, "start")) {
        set_recording(true);
    } else if (g_str_equal(cmd, "stop")) {
        set_recording(false);
    } else if (g_str_has_prefix(cmd, "dump ")) {
        dump(g_strstrip(cmd + 5));
    } else if (*cmd) {
        fprintf(stderr, "funcprof: unknown command '%s'\n", cmd);
    }
}

static void drain_commands(void)
{
    char *line;
    while ((line = g_async_queue_try_pop(commands))) {
        g_atomic_int_add(&pending, -1);
        run_command(line);
        g_free(line);
    }
}

/*
 * Reader thread. Opening the FIFO read-write keeps a writer end open, so
 * reads never hit EOF and a writer's non-blocking open always finds a reader.
 */
static gpointer control_thread(gpointer unused)
{
    int fd = open(ctl_path, O_RDWR);
    FILE *f = fd >= 0 ? fdopen(fd, "r") : NULL;
    if (!f) {
        fprintf(stderr, "funcprof: open %s: %s\n", ctl_path, strerror(errno));
        return NULL;
    }
    char buf[4096];
    while (fgets(buf, sizeof(buf), f)) {
        g_async_queue_push(commands, g_strdup(buf));
        g_atomic_int_add(&pending, 1);
    }
    return NULL;
}

/* ---- instrumentation -------------------------------------------------- */

static inline void tb_common(uint32_t pc)
{
    if (g_atomic_int_get(&pending)) {
        drain_commands();
    }
    if (depth) {
        check_return(pc);
    }
}

static void vcpu_tb_exec(unsigned int cpu_index, void *udata)
{
    uint64_t packed = (uint64_t)(uintptr_t)udata;
    uint32_t pc = (uint32_t)(packed >> 32);
    unsigned n = (unsigned)(packed & 0xffffffffu);

    tb_common(pc);
    icount += n;
    if (recording) {
        record_icount += n;
    }
}

static void vcpu_entry_exec(unsigned int cpu_index, void *udata)
{
    EntryTB *e = udata;

    tb_common(e->func->addr);
    push_frame(e->func);
    icount += e->n_insns;
    if (recording) {
        record_icount += e->n_insns;
    }
}

static void vcpu_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    uint32_t pc = (uint32_t)qemu_plugin_tb_vaddr(tb);
    unsigned n = (unsigned)qemu_plugin_tb_n_insns(tb);
    Func *f = g_hash_table_lookup(funcs, GUINT_TO_POINTER(pc));

    if (f) {
        /* One per translation of an entry TB; retranslations are rare. */
        EntryTB *e = g_new(EntryTB, 1);
        e->func = f;
        e->n_insns = n;
        qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_entry_exec,
                                             QEMU_PLUGIN_CB_R_REGS, e);
    } else {
        uint64_t packed = ((uint64_t)pc << 32) | n;
        qemu_plugin_register_vcpu_tb_exec_cb(tb, vcpu_tb_exec,
                                             QEMU_PLUGIN_CB_R_REGS,
                                             (void *)(uintptr_t)packed);
    }
}

static void plugin_exit(qemu_plugin_id_t id, void *p)
{
    if (out_path) {
        dump(out_path);
    }
}

static bool parse_bool(const char *name, const char *value, bool *out)
{
    if (!qemu_plugin_bool_parse(name, value, out)) {
        fprintf(stderr, "funcprof: bad boolean %s=%s\n", name, value);
        return false;
    }
    return true;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    funcs = g_hash_table_new(g_direct_hash, g_direct_equal);
    reg_buf = g_byte_array_new();
    bool signpost_arg = false;

    for (int i = 0; i < argc; i++) {
        char **kv = g_strsplit(argv[i], "=", 2);
        const char *key = kv[0], *value = kv[1] ? kv[1] : "";
        bool ok = true;
        if (g_str_equal(key, "syms")) {
            load_syms(value);
        } else if (g_str_equal(key, "fn")) {
            char **parts = g_strsplit(value, ":", 2);
            add_func((uint32_t)g_ascii_strtoull(parts[0], NULL, 0),
                     parts[1] ? parts[1] : parts[0]);
            g_strfreev(parts);
        } else if (g_str_equal(key, "ctl")) {
            ctl_path = g_strdup(value);
        } else if (g_str_equal(key, "out")) {
            out_path = g_strdup(value);
        } else if (g_str_equal(key, "record")) {
            ok = parse_bool(key, value, &recording);
        } else if (g_str_equal(key, "trace")) {
            ok = parse_bool(key, value, &tracing);
        } else if (g_str_equal(key, "tracecap")) {
            tracecap = g_ascii_strtoull(value, NULL, 0);
        } else if (g_str_equal(key, "timeline")) {
            timeline_insns = g_str_equal(value, "insns");
            ok = timeline_insns || g_str_equal(value, "host");
        } else if (g_str_equal(key, "signpost")) {
            ok = parse_bool(key, value, &signpost_arg);
        } else {
            fprintf(stderr, "funcprof: unknown argument %s\n", argv[i]);
            ok = false;
        }
        g_strfreev(kv);
        if (!ok) {
            return -1;
        }
    }
    if (g_hash_table_size(funcs) == 0) {
        fprintf(stderr, "funcprof: no functions to watch (syms= or fn=)\n");
        return -1;
    }
#ifdef __APPLE__
    if (signpost_arg) {
        signposts = true;
        signpost_log = os_log_create("org.qemu.ipod.funcprof",
                                     OS_LOG_CATEGORY_POINTS_OF_INTEREST);
    }
#else
    if (signpost_arg) {
        fprintf(stderr, "funcprof: signpost=on needs macOS; ignored\n");
    }
#endif
    if (tracing) {
        events = g_array_new(false, false, sizeof(Event));
    }
    record_since = host_now();
    if (ctl_path) {
        commands = g_async_queue_new();
        g_thread_new("funcprof-ctl", control_thread, NULL);
    }
    fprintf(stderr, "funcprof: watching %u functions\n",
            g_hash_table_size(funcs));
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
