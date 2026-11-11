/*
 * S5L8702 display pipe, 0x38900000.
 *
 * NO SOURCE WE HAVE NAMES THIS BLOCK. Rockbox's s5l87xx.h does not list it,
 * freemyipod's model has nothing there, and Rockbox never touches it -- Rockbox
 * pushes its frame through DMAC0 into LCD_WDATA instead (lcd-s5l8702.c:172).
 * Everything below comes from disassembling Apple's own osos, and from a
 * measured boot; where the two disagree the measurement wins and it is said so.
 *
 * WHY IT MATTERS. 0x08127f14, the retailOS's "draw the whole screen", ends
 * with two stores to the LCD frame engine (LCD +0x74 geometry, LCD +0x70 = 1
 * "go") and no pixel ever goes through the LCD data port. Nothing in the LCD's
 * own 256-byte window says where that frame comes from. This block does: it has
 * the panel geometry, three layer descriptors, five-plus source-address slots,
 * three 256-entry gamma tables and four colour-matrix coefficient tables. It is
 * the composition engine the LCD frame engine presents.
 *
 * ---------------------------------------------------------------- registers
 *
 * Every entry below is either (V) verified from a store or load in the osos
 * whose base register was loaded with the literal 0x38900000 -- the complete
 * list of those twenty-seven sites is in wiki/11 -- or (M) additionally
 * confirmed by the value seen in a real boot, or (U) undetermined.
 *
 *  +0x000  (M) written 1 by the init path. Meaning UNDETERMINED.
 *  +0x004  (M) written 1 by 0x0812729c. Meaning UNDETERMINED.
 *  +0x008  (V,M) THE CONTROL REGISTER. Six different functions do
 *          read-modify-write on it, one bit field each, which is why leaving
 *          this block unimplemented was actively harmful: reads returned 0, so
 *          every RMW threw away every bit set before it.
 *            bit 0      0x080a508c arg r3 == 1
 *            bit 1      0x080a511c arg != 0
 *            bits 2..7  0x081271c8, six selectors (6,5,4,3,2,7 for sel 0..5)
 *            bit 8      0x080822e8 arg == 0
 *            bit 15     0x0808dfe4 arg == 1
 *            bits 17:16 0x080a508c byte arg at sp+10 (1 or 2)
 *            bit 20     0x080a508c byte arg at sp+9 == 1
 *            bits 25:24 0x080a508c byte arg at sp+8 (1 or 2)
 *            bit 28     0x080a508c arg r1 == 1
 *            bit 29     0x080a508c arg r0 != 0
 *            bit 30     0x080be518 arg != 0
 *          MEASURED, diskless boot: the values that reach it are 0x01110001,
 *          0x00008000, 0x00000100 and 0x40000000 -- each one a single field
 *          written onto a zero readback. 0x01110001 is exactly what
 *          0x080a508c(0, 0, {1,1,1}, 1) computes, which is the call the init
 *          path makes, so the decode above is confirmed end to end.
 *  +0x00c  (M) 0x000f0f0f from a literal at 0x08127460. UNDETERMINED.
 *  +0x020  (M) written 1 next to +0x004. UNDETERMINED.
 *  +0x024  (M) 0x00ffffff. UNDETERMINED.
 *  +0x028..+0x054  (V,M) THE VIDEO WINDOW, window 5: a scaled planar YUV
 *          scanout window, enabled by CONTROL bit 7. DisplayPipe_ConfigureWindow
 *          (0x08143c30) and DisplayPipe_SetWindowSource (0x081445cc), case 5 of
 *          each. See S5L8702_DISP_VIDEO_* below.
 *            +0x028  mode, 0x100 for format 8 (YUV 4:2:0), else 0
 *            +0x02c  luma stride | chroma stride << 16, bytes
 *            +0x030  source crop start, (x << 16 | y)
 *            +0x034  source crop end,   (x << 16 | y)
 *            +0x038  Y plane
 *            +0x03c  Cb, surface plane 3
 *            +0x040  0 for format 8
 *            +0x044  Cr, surface plane 2
 *            +0x04c  scale step, h << 16 | v, 4.12 fixed point (0x1000 = 1:1)
 *            +0x050  position on the panel, (x << 16 | y)
 *            +0x054  size on the panel,     (w << 16 | h)
 *          MEASURED while the main menu scrolls: mode 0x100, strides 320/160,
 *          crop 0..320x240, step 1:1, position 0,0, size 320x240. The Y plane
 *          is the grey preview-pane gradient.
 *  +0x060  (V) SOURCE ADDRESS, slot 0  \
 *  +0x078  (V) SOURCE ADDRESS, slot 1   |  0x081276dc cases 0..4. Stride 0x18.
 *  +0x090  (V) SOURCE ADDRESS, slot 2   |  Which slot is used is chosen by the
 *  +0x0a8  (V) SOURCE ADDRESS, slot 3   |  caller, not by this block.
 *  +0x0c0  (V) SOURCE ADDRESS, slot 4  /
 *  +0x0d4  (V,M) WINDOW ORDER, (order | b << 8). The low byte selects which
 *          blend register each blend column gets; see S5L8702_DISP_ORDER below.
 *          Bits 15:8 are UNDETERMINED (2 on menus, 0 in a game).
 *  +0x0d8, +0x0e0, +0x0e8  (V,M) THE THREE BLEND REGISTERS, one per stacking
 *          position, indexed by the window order rather than by window.
 *            bits 31:28 and 15:12  blend mode, DisplayPipe_SetWindowBlendMode
 *                       (0x0814472c): modes 0..4 are the pairs (1,0), (0,1),
 *                       (5,4), (9,8), (11,10)
 *            bits 7:0   global alpha, DisplayPipe_SetWindowAlpha (0x08144410)
 *          The modes are NOT MODELLED. MEASURED on menus: 0x900080ff (mode 3,
 *          alpha 0xff) at +0x0d8, which with order 1 belongs to the video
 *          window.
 *  +0x0f0..+0x17c (V) nine 4-word entries, copied by 0x081276a0
 *  +0x180..+0x1c4 (V) nine 2-word entries, copied by 0x08127664
 *  +0x1ec..+0x1fc (V,M) five words copied straight out of a struct by
 *          0x080bf110. MEASURED 0, 0, 0x10, 0, 0. UNDETERMINED.
 *  +0x200  (V,M) bit 0 set/cleared by 0x08096eb8, and OR'd with 0x00010080 by
 *          0x081273f8. Both are read-modify-write and it is genuinely read
 *          back, so a zero readback here really did lose state.
 *  +0x204  (V,M) 2, at 0x0812740c.   +0x208 (V,M) 0, at 0x08127428.
 *  +0x20c  (V,M) 2, at 0x08127410.
 *  +0x210  (V,M) 0x00010110, a literal at 0x08127464.
 *  +0x214  (V,M) 0x013f00ef -- (319 << 16) | 239, THE PANEL GEOMETRY, to the
 *          pixel, a literal at 0x08127468. Nothing else in this block is as
 *          unambiguous.
 *          These six live in the init function 0x08127268, which reaches them
 *          through r6 rather than a fresh literal, which is why scanning for
 *          pc-relative loads of 0x38900000 did not find them. The trace found
 *          them: DISPTRACE prints the guest PC with every access.
 *  +0x31c..+0x360 (V) nine 2-word entries, copied by 0x081275ec
 *  +0x364..+0x3a8 (V) nine 2-word entries, copied by 0x08127628
 *  +0x3ac  (V,M) 0x080bcef8 builds it as
 *          (w << 20) | (h << 8) | (c << 2) | (d << 1) | e, and 0x08127f54 calls
 *          it with (64, 64, 0, 0, 0) -> 0x04004000, which is exactly what the
 *          boot writes. Bit 0 is read back by the blit at five places and
 *          swaps width for height everywhere it is tested, so bit 0 is
 *          "rotated 90 degrees". INFERRED from that use, not from a datasheet.
 *  +0x400..+0x7fc (M) 256-entry gamma/palette table, channel 0
 *  +0x800..+0xbfc (M) channel 1
 *  +0xc00..+0xffc (M) channel 2
 *          0x0806fe9c is the writer and 0x08127268 builds the argument as
 *          tbl[i] = i * 4 for i in 0..255, so what the boot loads is the
 *          identity ramp, left-shifted by two. MEASURED: all 768 words, value
 *          == index * 4, in all three tables.
 *
 * Nothing above +0xfff was touched in a whole diskless boot, so the window is
 * 4 KB and the rest of the 1 MB block still falls to the machine's io floor,
 * where -d unimp will report it if that ever stops being true.
 *
 * WHAT THIS MODEL DOES: store every register honestly, and compose the layers
 * into the panel when the panel is about to be shown. See the layer-descriptor
 * note below, and ipod_classic_disp.c for the composition itself.
 *
 * WHAT IT DOES NOT DO: the gamma tables, the four colour-matrix coefficient
 * tables, the scaler filter tables, the blend modes and the rotate bit in
 * +0x3ac are all stored and read back but none of them affects the picture.
 * The gamma tables are the identity in every boot measured so far -- literally
 * tbl[i] = i * 4 -- so honouring them would change nothing today. The video
 * window's scaler steps are honoured by nearest-neighbour sampling.
 *
 * A boot with no disk never writes a source address into any slot at all
 * (MEASURED: zero writes to +0x038..+0x0c0 in a 40-second diskless boot,
 * because the user interface dies for want of fonts long before it composes
 * anything), so it draws nothing and the panel stays black. That is the guest's
 * behaviour, not the model's. Set DISPTRACE=1 to watch the registers.
 */

#ifndef HW_ARM_IPOD_CLASSIC_DISP_H
#define HW_ARM_IPOD_CLASSIC_DISP_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_IPOD_CLASSIC_DISP "s5l8702.disp"
OBJECT_DECLARE_SIMPLE_TYPE(IPodClassicDispState, IPOD_CLASSIC_DISP)

/* The window we answer for. Everything above this falls to the io floor. */
#define S5L8702_DISP_MEM_SIZE   0x1000
#define S5L8702_DISP_NREG       (S5L8702_DISP_MEM_SIZE / 4)

/*
 * THE FIVE LAYER DESCRIPTORS, and this is what puts pixels on the panel.
 *
 * 0x0812705c..0x0812716c is one five-way switch on the layer index (jump table
 * at 0x08127068, arms at 0x08127080, 0x081270b0, 0x081270e0, 0x08127110 and
 * 0x08127140); every arm writes the same FIVE registers at base 0x058 + k*0x18:
 *
 *   +0x00  source stride in bytes            (r2 = bpp * srcW / 2 ... see below)
 *   +0x04  (align << 24) | (fmt << 8) | ...  bit 16 set when NOT 32 bpp
 *   +0x0c  (W << 16) | H          destination size
 *   +0x10  W * bytes-per-pixel / 4          (a cross-check, not used here)
 *   +0x14  (X << 16) | Y          destination position
 *
 * The sixth word of the descriptor, and the one that matters most --
 *
 *   +0x08  SOURCE ADDRESS, already advanced to the first visible pixel
 *
 * -- is NOT written by this switch. It comes from a separate five-way switch in
 * 0x081276dc, which is why a boot can program a descriptor's geometry without
 * ever giving it something to read from.
 *
 * VERIFIED against a real boot with a disk. Layer 0:
 *   +0x058 = 0x00000500 (1280 = 320 * 4), +0x05c = 0x00000700 (fmt 7, and bit
 *   16 clear so 32 bpp), +0x060 = 0x09767a24, +0x064 = 0x014000f0 (320 x 240),
 *   +0x06c = 0. Layer 1:
 *   +0x070 = 0x00000280 (640 = 320 * 2), +0x074 = 0x02010300 (fmt 3, bit 16
 *   set so 16 bpp), +0x078 = 0x0be2326c, +0x07c = 0x006c00f0 (108 x 240),
 *   +0x084 = 0x00d40000 (x = 212, and 212 + 108 = 320 exactly).
 *
 * The bytes-per-pixel table comes from the switch at 0x08126f90, which maps the
 * format field to a multiplier r1 in {1, 2, 4, 8} and then computes the stride
 * as r1 * srcW / 2 -- so r1 is twice the bytes per pixel:
 *   fmt 0 -> 4 bpp, fmt 1 -> 8 bpp, fmt 2..5 -> 16 bpp, fmt 6..7 -> 32 bpp.
 * Both measured layers agree with it to the byte.
 *
 * W and H are SIGNED. MEASURED: while the right-hand pane slides off screen the
 * firmware writes +0x07c = 0xffc900f0, i.e. width -55. Anything with a
 * non-positive width or height, or an origin off the panel, is not drawn.
 */
#define S5L8702_DISP_SLOT_BASE   0x058
#define S5L8702_DISP_SLOT_STEP   0x018
#define S5L8702_DISP_NSLOTS      5
#define S5L8702_DISP_SLOT_STRIDE 0x00
#define S5L8702_DISP_SLOT_FORMAT 0x04
#define S5L8702_DISP_SLOT_SRC    0x08
#define S5L8702_DISP_SLOT_SIZE   0x0c
#define S5L8702_DISP_SLOT_POS    0x14

/* Named registers, all justified in the comment above. */
#define S5L8702_DISP_UNK00      0x000
#define S5L8702_DISP_UNK04      0x004
#define S5L8702_DISP_CONTROL    0x008
#define S5L8702_DISP_UNK0C      0x00c
#define S5L8702_DISP_VIDEO_MODE     0x028
#define S5L8702_DISP_VIDEO_STRIDES  0x02c
#define S5L8702_DISP_VIDEO_CROP0    0x030
#define S5L8702_DISP_VIDEO_CROP1    0x034
#define S5L8702_DISP_VIDEO_Y        0x038
#define S5L8702_DISP_VIDEO_CB       0x03c
#define S5L8702_DISP_VIDEO_CR       0x044
#define S5L8702_DISP_VIDEO_STEP     0x04c
#define S5L8702_DISP_VIDEO_POS      0x050
#define S5L8702_DISP_VIDEO_SIZE     0x054
#define S5L8702_DISP_VIDEO_MODE_YUV420 0x100
#define S5L8702_DISP_VIDEO_EN_BIT   0x80
#define S5L8702_DISP_SRC_PLANE0 0x038
#define S5L8702_DISP_SRC_PLANE3 0x044
#define S5L8702_DISP_SRC_SLOT0  0x060
#define S5L8702_DISP_SRC_STRIDE 0x018
#define S5L8702_DISP_NSLOT      5
#define S5L8702_DISP_LAYER0     0x0d8
#define S5L8702_DISP_LAYER_STEP 0x008
#define S5L8702_DISP_NLAYER     3
#define S5L8702_DISP_STATUS200  0x200
#define S5L8702_DISP_GEOMETRY   0x214
#define S5L8702_DISP_FORMAT     0x3ac
#define S5L8702_DISP_GAMMA0     0x400

/* Control-register bit that the blit path reads back. See +0x3ac above. */
#define S5L8702_DISP_FORMAT_ROT 0x1

/*
 * THE PER-LAYER ENABLE BITS, in CONTROL (+0x008). Bit 6 turns layer 0 on,
 * bit 5 layer 1, bit 4 layer 2 -- so layer k is bit (6 - k).
 *
 * This is the gate that was missing, and without it the model kept drawing
 * layers the guest had switched off but left fully programmed, which covered
 * the whole user interface with a stale surface. Two independent lines of
 * evidence, both from a boot with a disk:
 *
 * 1. THE SETTER, disassembled. 0x081271c8 takes an index in r1 and a boolean
 *    in r2, jumps through the table at 0x081271d8, and does
 *    "bic r0, r0, #BIT / orrne r0, r0, #BIT" on the word it read from +0x008
 *    before storing it back at 0x08127250. The arms are, in index order:
 *      0 -> 0x40   1 -> 0x20   2 -> 0x10   3 -> 0x08   4 -> 0x04   5 -> 0x80
 *    The index is the window: 0..4 are the five layer descriptors and 5 is
 *    the video window (DisplayPipe_SetWindowEnable, 0x081440b8). The guest
 *    sets bit 7 for every menu slide and preview-pane change.
 *
 * 2. THE ORDER OF WRITES, traced with DISPTRACE=1. Every single time, the
 *    guest programs a descriptor, writes its SOURCE ADDRESS, and only then
 *    sets the matching bit:
 *      +0x060 = 0x089d42d4 (layer 0) ... +0x008: 0x41118101 -> 0x41118141
 *      +0x078 = 0x0be2326c (layer 1) ... +0x008: 0x41118141 -> 0x41118161
 *      +0x090 = 0x0bfb4ff8 (layer 2) ... +0x008: 0x41118161 -> 0x41118171
 *    and when a transition ends it clears them again, 0x41118151 then
 *    0x41118141, WITHOUT clearing the source address or the size. A settled
 *    screen sits at 0x41118141: layer 0 alone.
 *
 * Not to be confused with bits 16, 20 and 24, which also look one-per-layer.
 * An earlier version of the compositor weighed those, found them set once
 * during init and never touched again, and rejected them as a gate -- which
 * was correct about 16/20/24, and became the wrong conclusion only because it
 * stopped there and declared that nothing gated a layer at all.
 *
 * Layers 3 and 4 follow the same rule, bits 3 and 2.
 */
#define S5L8702_DISP_LAYER_EN_BIT(k) (1u << (6 - (k)))

/*
 * THE WINDOW ORDER, +0x0d4. The low byte picks how the windows stack.
 *
 * Windows fall into three blend columns: layer 0 is column 0, layers 1..3
 * share column 1, the video window is column 2; layer 4 has no blend register.
 * DisplayPipe_SetOrder (0x0814482c) stores (order | b << 8) here, and every
 * per-window setter then gives column c the blend register
 * DisplayLayerOrders[order][c] -- the 18 bytes at 0x083f35ec, copied in the
 * compositor. MEASURED: menus and Now Playing run with order 1, a game with
 * order 4.
 *
 * The blend register index is taken as the stacking position, 0 at the back.
 * INFERRED, not proven, but it is what every screen measured needs: with order
 * 1 the video window is at the back, where layer 0's alpha-0 holes show it,
 * and the player interface in layer 0 is in front of the album art in layer 1.
 * With order 4 a game's frame in layer 1 covers the grey surface in layer 0.
 */
#define S5L8702_DISP_ORDER       0x0d4
#define S5L8702_DISP_NORDER      6

struct IPodClassicDispState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    /*
     * A flat register file. Readback is the point: six functions in the osos
     * read-modify-write +0x008 and one reads +0x200, and with the block
     * unimplemented every one of those read zero and destroyed whatever the
     * previous caller had set.
     */
    uint32_t reg[S5L8702_DISP_NREG];

    /* Scratch for one source row, biggest case: 320 pixels at 32 bpp. */
    uint8_t rowbuf[320 * 4];

    /*
     * The frame being composed, 320x240 RGB565. Layers land here on a cleared
     * background and the result replaces the panel image in one go, so nothing
     * a layer stopped covering survives into the next frame.
     */
    uint16_t frame[320 * 240];
};

/*
 * Composite whatever the layer descriptors currently point at into a 320x240
 * RGB565 image, which is what the panel behind the LCD port holds. Returns
 * true if anything was drawn. Called by the LCD device, because on this SoC the
 * panel's image memory is what the user finally sees -- see ipod_classic_lcd.c.
 */
bool ipod_classic_disp_compose(IPodClassicDispState *s, uint16_t *fb,
                               unsigned width, unsigned height);

#endif
