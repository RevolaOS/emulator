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
 * HEAP TRACKING. heap= hooks name the guest allocator's entry points. An
 * allocation hook reads its size (and optionally a heap-instance) argument at
 * entry and the returned pointer in r0 at the TB that starts at its return
 * address with the entry SP. Pending allocations are matched on (return
 * address, SP) outside the frame stack, so an RTXC task switch inside the
 * allocator cannot drop them. A free hook acts at entry. The plugin keeps a
 * table of live blocks from the first guest instruction, independent of
 * recording, so the live totals are the requested bytes currently allocated
 * through the hooks; allocator headers and rounding are not included. A
 * pointer freed through a hook but never seen allocated counts as an
 * untracked free; an allocation returning a pointer that is still live
 * replaces it and counts as replaced (both stay zero when the hooks cover
 * every allocator path). A realloc frees its old block unless a hooked free
 * already did.
 *
 * The call site of an allocation is its hook's return address, so hook the
 * innermost functions that are entered with the caller's LR (shims reached by
 * tail calls from the public wrappers). Hooks must not nest: an allocation
 * hook called from another allocation hook is counted twice. A free hook
 * called inside a realloc hook is fine. While recording, each allocation and
 * tracked free is also credited to the innermost open watched function
 * (alloc_bytes, freed_bytes) and, when that frame ends, to it and its watched
 * callers inclusively (alloc_incl, freed_incl); like instruction counts, this
 * includes interrupts and other tasks that run while the frame is open.
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
 *   heap=0xADDR:KIND[:NAME]
 *                   add one heap hook (repeatable). KIND is
 *                     alloc[/SIZE[/HEAP]]       defaults SIZE=r0
 *                     free[/PTR[/HEAP]]         defaults PTR=r0
 *                     realloc[/PTR/SIZE[/HEAP]] defaults PTR=r0 SIZE=r1
 *                   where each field names an argument register r0-r12;
 *                   HEAP (optional) identifies the heap instance
 *   heaps=PATH      heap hooks, one "0xADDR KIND NAME" per line
 *
 * At least one of syms=, fn=, heap= or heaps= must name an address.
 *
 * A dump writes PATH (summary JSON) and, with trace=on, PATH.trace.json. Both
 * appear atomically (written to a temporary name, then renamed). With heap
 * hooks the summary has a "heap" object: window totals plus the current live
 * totals, per-heap-instance totals, and per-call-site statistics including
 * the blocks allocated since the last reset that are still live ("leaks").
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
#define MAX_PENDING 32          /* allocations awaiting their return */
#define LEAK_EXAMPLES 4         /* largest leaked blocks listed per site */
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
    guint64 alloc_calls;
    guint64 alloc_bytes;        /* allocated while this was the innermost frame */
    guint64 alloc_incl;         /* ... or one of its watched callees was */
    guint64 free_calls;
    guint64 freed_bytes;
    guint64 freed_incl;
} Func;

typedef struct {
    Func *func;
    uint32_t ret;
    uint32_t sp;
    guint64 insn_start;
    guint64 child;
    guint64 host_start;
    guint64 allocs, alloc_bytes, alloc_child;   /* heap credit, see credit_heap */
    guint64 frees, freed_bytes, freed_child;
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

typedef enum { HEAP_ALLOC, HEAP_FREE, HEAP_REALLOC } HeapKind;

typedef struct {
    uint32_t addr;
    char *name;
    HeapKind kind;
    int ptr_reg;                /* free, realloc */
    int size_reg;               /* alloc, realloc */
    int heap_reg;               /* -1: no heap-instance argument */
} HeapHook;

typedef struct {                /* an allocation awaiting its return */
    HeapHook *hook;
    uint32_t ret, sp;
    uint32_t heap, size, old;
} HeapCall;

typedef struct {                /* a live block */
    uint32_t size, site, heap;
    guint64 seq;                /* allocation order since boot */
    guint64 epoch;              /* recording epoch it was made in; 0 = not recording */
} Block;

typedef struct {
    guint64 allocs, bytes, frees, freed_bytes;  /* since the last reset */
    guint64 live_count, live_bytes, leak_count, leak_bytes; /* filled by dump */
    uint32_t leak_ptr[LEAK_EXAMPLES];
    Block leak[LEAK_EXAMPLES];
    unsigned n_leak;
} Site;

typedef struct {
    guint64 live_count, live_bytes, peak_bytes;
    guint64 allocs, bytes, frees, freed_bytes;  /* since the last reset */
} HeapStats;

typedef struct {
    Func *func;                 /* NULL: heap hook only */
    HeapHook *hook;             /* NULL: watched function only */
    uint32_t addr;
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

static GHashTable *heap_hooks;  /* addr -> HeapHook* */
static GHashTable *blocks;      /* ptr -> Block* */
static GHashTable *sites;       /* return address -> Site* */
static GHashTable *heaps;       /* heap instance -> HeapStats* */
static HeapCall heap_calls[MAX_PENDING];
static unsigned n_heap_calls;
static HeapStats heap_total;
static guint64 heap_epoch = 1;  /* advanced by reset */
static guint64 heap_seq;
static guint64 heap_failed, heap_untracked, heap_replaced, heap_dropped;

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

/*
 * Heap credit of an ending frame: its own allocations and frees, and its
 * callees' inclusively. Bytes are known without a return, so discarded frames
 * are credited too.
 */
static void credit_heap(unsigned k)
{
    Frame *fr = &stack[k];
    guint64 alloc_incl = fr->alloc_bytes + fr->alloc_child;
    guint64 freed_incl = fr->freed_bytes + fr->freed_child;
    if (k > 0) {
        stack[k - 1].alloc_child += alloc_incl;
        stack[k - 1].freed_child += freed_incl;
    }
    if (fr->valid && recording) {
        Func *f = fr->func;
        f->alloc_calls += fr->allocs;
        f->alloc_bytes += fr->alloc_bytes;
        f->alloc_incl += alloc_incl;
        f->free_calls += fr->frees;
        f->freed_bytes += fr->freed_bytes;
        f->freed_incl += freed_incl;
    }
}

/* Drop stack[k] without a return; its watched callees' cost stays with its caller. */
static void discard(unsigned k)
{
    Frame *fr = &stack[k];
    if (k > 0) {
        stack[k - 1].child += fr->child;
    }
    credit_heap(k);
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
    credit_heap(k);
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
    fr->allocs = fr->alloc_bytes = fr->alloc_child = 0;
    fr->frees = fr->freed_bytes = fr->freed_child = 0;
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

/* ---- heap ------------------------------------------------------------- */

static Site *site_for(uint32_t addr)
{
    Site *s = g_hash_table_lookup(sites, GUINT_TO_POINTER(addr));
    if (!s) {
        s = g_new0(Site, 1);
        g_hash_table_insert(sites, GUINT_TO_POINTER(addr), s);
    }
    return s;
}

static HeapStats *heap_for(uint32_t heap)
{
    HeapStats *h = g_hash_table_lookup(heaps, GUINT_TO_POINTER(heap));
    if (!h) {
        h = g_new0(HeapStats, 1);
        g_hash_table_insert(heaps, GUINT_TO_POINTER(heap), h);
    }
    return h;
}

/* Remove a live block; `counted` records it as a free in the window stats. */
static void release_block(uint32_t ptr, Block *b, bool counted)
{
    HeapStats *h = heap_for(b->heap);
    heap_total.live_count--;
    heap_total.live_bytes -= b->size;
    h->live_count--;
    h->live_bytes -= b->size;
    if (counted && recording) {
        Site *s = site_for(b->site);
        s->frees++;
        s->freed_bytes += b->size;
        heap_total.frees++;
        heap_total.freed_bytes += b->size;
        h->frees++;
        h->freed_bytes += b->size;
        if (depth) {
            stack[depth - 1].frees++;
            stack[depth - 1].freed_bytes += b->size;
        }
    }
    g_hash_table_remove(blocks, GUINT_TO_POINTER(ptr));
}

static void heap_free(uint32_t ptr)
{
    if (!ptr) {
        return;
    }
    Block *b = g_hash_table_lookup(blocks, GUINT_TO_POINTER(ptr));
    if (b) {
        release_block(ptr, b, true);
    } else if (recording) {
        heap_untracked++;
    }
}

static void heap_alloc_done(const HeapCall *c, uint32_t ptr)
{
    if (!ptr) {
        if (recording) {
            heap_failed++;
        }
        return;
    }
    Block *old;
    if (c->hook->kind == HEAP_REALLOC && c->old &&
        (old = g_hash_table_lookup(blocks, GUINT_TO_POINTER(c->old)))) {
        release_block(c->old, old, true);   /* unless a hooked free already ran */
    }
    old = g_hash_table_lookup(blocks, GUINT_TO_POINTER(ptr));
    if (old) {
        if (recording) {
            heap_replaced++;
        }
        release_block(ptr, old, false);
    }
    Block *b = g_new(Block, 1);
    b->size = c->size;
    b->site = c->ret;
    b->heap = c->heap;
    b->seq = ++heap_seq;
    b->epoch = recording ? heap_epoch : 0;
    g_hash_table_insert(blocks, GUINT_TO_POINTER(ptr), b);

    HeapStats *h = heap_for(c->heap);
    heap_total.live_count++;
    heap_total.live_bytes += c->size;
    h->live_count++;
    h->live_bytes += c->size;
    if (!recording) {
        return;
    }
    if (heap_total.live_bytes > heap_total.peak_bytes) {
        heap_total.peak_bytes = heap_total.live_bytes;
    }
    if (h->live_bytes > h->peak_bytes) {
        h->peak_bytes = h->live_bytes;
    }
    Site *s = site_for(c->ret);
    s->allocs++;
    s->bytes += c->size;
    heap_total.allocs++;
    heap_total.bytes += c->size;
    h->allocs++;
    h->bytes += c->size;
    if (depth) {
        stack[depth - 1].allocs++;
        stack[depth - 1].alloc_bytes += c->size;
    }
}

static void heap_entry(HeapHook *hook)
{
    uint32_t heap = hook->heap_reg >= 0 ? read_reg32(hook->heap_reg) : 0;

    if (hook->kind == HEAP_FREE) {
        heap_free(read_reg32(hook->ptr_reg));
        return;
    }
    uint32_t ret = read_reg32(REG_LR) & ~1u;
    uint32_t sp = read_reg32(REG_SP);
    HeapCall *c = NULL;
    for (unsigned i = 0; i < n_heap_calls; i++) {
        if (heap_calls[i].ret == ret && heap_calls[i].sp == sp) {
            c = &heap_calls[i];     /* re-entry: the earlier call never returned */
            break;
        }
    }
    if (!c) {
        if (n_heap_calls == MAX_PENDING) {
            memmove(&heap_calls[0], &heap_calls[1], sizeof(HeapCall) * (MAX_PENDING - 1));
            n_heap_calls--;
            if (recording) {
                heap_dropped++;
            }
        }
        c = &heap_calls[n_heap_calls++];
    }
    c->hook = hook;
    c->ret = ret;
    c->sp = sp;
    c->heap = heap;
    c->size = read_reg32(hook->size_reg);
    c->old = hook->kind == HEAP_REALLOC ? read_reg32(hook->ptr_reg) : 0;
}

static void check_heap_return(uint32_t pc)
{
    uint32_t sp = 0;
    bool have_sp = false;

    for (unsigned i = n_heap_calls; i-- > 0;) {
        if (heap_calls[i].ret != pc) {
            continue;
        }
        if (!have_sp) {
            sp = read_reg32(REG_SP);
            have_sp = true;
        }
        if (heap_calls[i].sp != sp) {
            continue;
        }
        HeapCall c = heap_calls[i];
        memmove(&heap_calls[i], &heap_calls[i + 1],
                sizeof(HeapCall) * (n_heap_calls - i - 1));
        n_heap_calls--;
        heap_alloc_done(&c, read_reg32(0));
        return;
    }
}

static bool parse_reg(const char *text, int *reg)
{
    char *end;
    if (text[0] != 'r') {
        return false;
    }
    guint64 n = g_ascii_strtoull(text + 1, &end, 10);
    if (end == text + 1 || *end || n > 12) {
        return false;
    }
    *reg = (int)n;
    return true;
}

static bool add_heap_hook(uint32_t addr, const char *kind, const char *name)
{
    char **f = g_strsplit(kind, "/", -1);
    unsigned n = g_strv_length(f);
    HeapHook *h = g_new0(HeapHook, 1);
    bool ok;

    h->addr = addr & ~1u;
    h->heap_reg = -1;
    if (g_str_equal(f[0], "alloc") && n <= 3) {
        h->kind = HEAP_ALLOC;
        ok = (n < 2 || parse_reg(f[1], &h->size_reg)) &&
             (n < 3 || parse_reg(f[2], &h->heap_reg));
    } else if (g_str_equal(f[0], "free") && n <= 3) {
        h->kind = HEAP_FREE;
        ok = (n < 2 || parse_reg(f[1], &h->ptr_reg)) &&
             (n < 3 || parse_reg(f[2], &h->heap_reg));
    } else if (g_str_equal(f[0], "realloc") && n != 2 && n <= 4) {
        h->kind = HEAP_REALLOC;
        h->size_reg = 1;
        ok = (n < 3 || (parse_reg(f[1], &h->ptr_reg) && parse_reg(f[2], &h->size_reg))) &&
             (n < 4 || parse_reg(f[3], &h->heap_reg));
    } else {
        ok = false;
    }
    g_strfreev(f);
    if (!ok || g_hash_table_contains(heap_hooks, GUINT_TO_POINTER(h->addr))) {
        fprintf(stderr, "funcprof: %s heap hook 0x%08" PRIx32 " %s\n",
                ok ? "duplicate" : "bad", h->addr, kind);
        g_free(h);
        return false;
    }
    h->name = g_strdup(name && *name ? name : kind);
    g_hash_table_insert(heap_hooks, GUINT_TO_POINTER(h->addr), h);
    return true;
}

static bool add_heap_spec(const char *addr_text, const char *kind, const char *name)
{
    char *end = NULL;
    guint64 addr = addr_text ? g_ascii_strtoull(addr_text, &end, 0) : 0;
    if (!addr_text || end == addr_text || *end || !kind) {
        fprintf(stderr, "funcprof: bad heap hook '%s %s'\n", addr_text ? addr_text : "",
                kind ? kind : "");
        return false;
    }
    return add_heap_hook((uint32_t)addr, kind, name);
}

static bool load_heap_hooks(const char *path)
{
    char *text;
    GError *error = NULL;
    if (!g_file_get_contents(path, &text, NULL, &error)) {
        fprintf(stderr, "funcprof: %s\n", error->message);
        g_error_free(error);
        return false;
    }
    bool ok = true;
    char **lines = g_strsplit(text, "\n", -1);
    for (char **line = lines; *line && ok; line++) {
        char *s = g_strstrip(*line);
        if (!*s || *s == '#') {
            continue;
        }
        char **f = g_strsplit_set(s, " \t", -1);
        char *field[3] = { NULL, NULL, NULL };
        for (unsigned i = 0, n = 0; f[i] && n < 3; i++) {
            if (*f[i]) {
                field[n++] = f[i];
            }
        }
        ok = add_heap_spec(field[0], field[1], field[2]);
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(text);
    return ok;
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

static void json_heap_stats(GString *s, const HeapStats *h)
{
    g_string_append_printf(s, "\"live_count\": %" PRIu64 ", \"live_bytes\": %" PRIu64
                           ", \"peak_bytes\": %" PRIu64 ", \"allocs\": %" PRIu64
                           ", \"bytes\": %" PRIu64 ", \"frees\": %" PRIu64
                           ", \"freed_bytes\": %" PRIu64,
                           h->live_count, h->live_bytes, h->peak_bytes, h->allocs,
                           h->bytes, h->frees, h->freed_bytes);
}

/* Keep a site's largest leaked blocks, sorted by size. */
static void note_leak(Site *site, uint32_t ptr, const Block *b)
{
    unsigned i;
    if (site->n_leak < LEAK_EXAMPLES) {
        i = site->n_leak++;
    } else if (site->leak[LEAK_EXAMPLES - 1].size < b->size) {
        i = LEAK_EXAMPLES - 1;
    } else {
        return;
    }
    for (; i > 0 && site->leak[i - 1].size < b->size; i--) {
        site->leak[i] = site->leak[i - 1];
        site->leak_ptr[i] = site->leak_ptr[i - 1];
    }
    site->leak[i] = *b;
    site->leak_ptr[i] = ptr;
}

static void dump_heap(GString *s)
{
    GHashTableIter it;
    gpointer key, value;

    /* Live and leaked totals per site come from the block table. */
    g_hash_table_iter_init(&it, sites);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        Site *site = value;
        site->live_count = site->live_bytes = site->leak_count = site->leak_bytes = 0;
        site->n_leak = 0;
    }
    g_hash_table_iter_init(&it, blocks);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        Block *b = value;
        Site *site = site_for(b->site);
        site->live_count++;
        site->live_bytes += b->size;
        if (b->epoch == heap_epoch) {
            site->leak_count++;
            site->leak_bytes += b->size;
            note_leak(site, GPOINTER_TO_UINT(key), b);
        }
    }

    g_string_append(s, "  \"heap\": {\n    \"totals\": {");
    json_heap_stats(s, &heap_total);
    g_string_append_printf(s, ", \"failed\": %" PRIu64 ", \"untracked_frees\": %" PRIu64
                           ", \"replaced\": %" PRIu64 ", \"dropped\": %" PRIu64
                           ", \"pending\": %u},\n",
                           heap_failed, heap_untracked, heap_replaced, heap_dropped,
                           n_heap_calls);
    g_string_append(s, "    \"heaps\": [");
    bool first = true;
    g_hash_table_iter_init(&it, heaps);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        g_string_append_printf(s, "%s\n      {\"heap\": \"0x%08x\", ", first ? "" : ",",
                               GPOINTER_TO_UINT(key));
        first = false;
        json_heap_stats(s, value);
        g_string_append(s, "}");
    }
    g_string_append(s, "\n    ],\n    \"sites\": [");
    first = true;
    g_hash_table_iter_init(&it, sites);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        Site *site = value;
        if (!site->allocs && !site->frees && !site->live_count) {
            continue;
        }
        g_string_append_printf(s, "%s\n      {\"addr\": \"0x%08x\", \"allocs\": %" PRIu64
                               ", \"bytes\": %" PRIu64 ", \"frees\": %" PRIu64
                               ", \"freed_bytes\": %" PRIu64 ", \"live_count\": %" PRIu64
                               ", \"live_bytes\": %" PRIu64 ", \"leak_count\": %" PRIu64
                               ", \"leak_bytes\": %" PRIu64 ", \"leaks\": [",
                               first ? "" : ",", GPOINTER_TO_UINT(key), site->allocs,
                               site->bytes, site->frees, site->freed_bytes,
                               site->live_count, site->live_bytes, site->leak_count,
                               site->leak_bytes);
        first = false;
        for (unsigned i = 0; i < site->n_leak; i++) {
            g_string_append_printf(s, "%s{\"ptr\": \"0x%08" PRIx32 "\", \"size\": %" PRIu32
                                   ", \"heap\": \"0x%08" PRIx32 "\", \"seq\": %" PRIu64 "}",
                                   i ? ", " : "", site->leak_ptr[i], site->leak[i].size,
                                   site->leak[i].heap, site->leak[i].seq);
        }
        g_string_append(s, "]}");
    }
    g_string_append(s, "\n    ]\n  },\n");
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
    if (g_hash_table_size(heap_hooks)) {
        dump_heap(s);
    }
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
                               ", \"unclosed\": %" PRIu64,
                               f->addr, f->calls, f->incl, f->self, f->max,
                               f->host_ns, f->unclosed);
        if (g_hash_table_size(heap_hooks)) {
            g_string_append_printf(s, ", \"alloc_calls\": %" PRIu64 ", \"alloc_bytes\": %"
                                   PRIu64 ", \"alloc_incl\": %" PRIu64 ", \"free_calls\": %"
                                   PRIu64 ", \"freed_bytes\": %" PRIu64 ", \"freed_incl\": %"
                                   PRIu64, f->alloc_calls, f->alloc_bytes, f->alloc_incl,
                                   f->free_calls, f->freed_bytes, f->freed_incl);
        }
        g_string_append_c(s, '}');
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
        f->alloc_calls = f->alloc_bytes = f->alloc_incl = 0;
        f->free_calls = f->freed_bytes = f->freed_incl = 0;
    }
    /* Live totals persist; window statistics restart and the peak restarts at live. */
    g_hash_table_iter_init(&it, sites);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        Site *site = value;
        site->allocs = site->bytes = site->frees = site->freed_bytes = 0;
    }
    g_hash_table_iter_init(&it, heaps);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        HeapStats *h = value;
        h->allocs = h->bytes = h->frees = h->freed_bytes = 0;
        h->peak_bytes = h->live_bytes;
    }
    heap_total.allocs = heap_total.bytes = heap_total.frees = heap_total.freed_bytes = 0;
    heap_total.peak_bytes = heap_total.live_bytes;
    heap_failed = heap_untracked = heap_replaced = heap_dropped = 0;
    heap_epoch++;
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
    if (n_heap_calls) {
        check_heap_return(pc);  /* first, so the caller's frame is still open */
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

    tb_common(e->addr);
    if (e->hook) {
        heap_entry(e->hook);
    }
    if (e->func) {
        push_frame(e->func);
    }
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
    HeapHook *h = g_hash_table_lookup(heap_hooks, GUINT_TO_POINTER(pc));

    if (f || h) {
        /* One per translation of an entry TB; retranslations are rare. */
        EntryTB *e = g_new(EntryTB, 1);
        e->func = f;
        e->hook = h;
        e->addr = pc;
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
    heap_hooks = g_hash_table_new(g_direct_hash, g_direct_equal);
    blocks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    sites = g_hash_table_new(g_direct_hash, g_direct_equal);
    heaps = g_hash_table_new(g_direct_hash, g_direct_equal);
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
        } else if (g_str_equal(key, "heap")) {
            char **parts = g_strsplit(value, ":", 3);
            ok = add_heap_spec(parts[0], parts[1], parts[1] ? parts[2] : NULL);
            g_strfreev(parts);
        } else if (g_str_equal(key, "heaps")) {
            ok = load_heap_hooks(value);
        } else {
            fprintf(stderr, "funcprof: unknown argument %s\n", argv[i]);
            ok = false;
        }
        g_strfreev(kv);
        if (!ok) {
            return -1;
        }
    }
    if (g_hash_table_size(funcs) == 0 && g_hash_table_size(heap_hooks) == 0) {
        fprintf(stderr, "funcprof: nothing to watch (syms=, fn= or heap=)\n");
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
    fprintf(stderr, "funcprof: watching %u functions, %u heap hooks\n",
            g_hash_table_size(funcs), g_hash_table_size(heap_hooks));
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans);
    qemu_plugin_register_atexit_cb(id, plugin_exit, NULL);
    return 0;
}
