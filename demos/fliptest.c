/*
 * fliptest.c - CRTC page-flip probe: settle the display-start-address unit
 *              and find a working vsync detector, BEFORE any engine code
 *              relies on either.
 *
 * Double-buffering on the ViRGE repoints the CRTC display-start address
 * (CR0C/CR0D + extension bits) at the back buffer each swap. DB019-B PDF
 * p.193 says CR31 bit 3 (ENH MAP, which the driver sets) selects DOUBLEWORD
 * addressing, so the start value should be byte_offset/4 -- but the
 * dump_crtc_truth comment hedges ("x4 if dword-addressed"), and register-
 * unit mistakes shear/garble the image, so confirm it on silicon.
 *
 * Separately, virge_wait_vsync times out (250ms) every run: it polls the
 * VSY INT latch, but DB019-B pp.299-301 show that latch only reports an
 * interrupt that is ENABLED (VSY ENB, Subsystem Control bit 8) -- which the
 * code never sets. This probe measures both the retrace and the latch.
 *
 * What it does:
 *   1. CPU-draws solid RED at VRAM offset 0 and a GREEN-top/BLUE-bottom
 *      split at stride*height (the back-buffer offset).
 *   2. Reports the working vsync source: 0x3DA bit-3 live-retrace edges,
 *      and the VSY INT latch with VSY ENB off vs on.
 *   3. Cycles the CRTC start address through byte divisors x4/x2/x8/x1 of
 *      stride*height, holding each 3s. Whichever divisor shows the clean
 *      GREEN/BLUE split is the real start-address unit (x4 = dword, per the
 *      datasheet). A wrong divisor shears/garbles or shows RED (offset 0).
 *
 * Build: make fliptest   (virge.c only, like tritest/filltest)
 * Run:   sudo ./fliptest      (Ctrl-C to exit; restores the start address)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <sys/io.h>

#include "backends/virge/virge.h"

/* Subsystem Control bit 8: VSY ENB - enable the vertical-sync interrupt so
 * the VSY INT status latch (Subsystem Status bit 0) can fire. DB019-B PDF
 * p.300. Distinct from VSY CLR (bit 0, already in virge.h). Bits 15-14 =
 * 00b means "no S3d reset / no change" (PDF p.301), so this is safe to OR
 * onto a control write. Defined locally until commit 2 folds it into virge.h. */
#define VSY_ENB_BIT            (1u << 8)

static volatile sig_atomic_t running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

/* RGB555 pack (CR67 Mode 9 scanout, forced by the takeover). */
static uint16_t pack555(int r, int g, int b)
{
    return (uint16_t)(((r & 31) << 10) | ((g & 31) << 5) | (b & 31));
}

/* Solid-fill one screen at a VRAM byte offset. */
static void cpu_fill(struct virge_ctx *ctx, uint32_t byte_off, uint16_t color)
{
    uint16_t *p = (uint16_t *)((uint8_t *)ctx->fb + byte_off);
    int n = ctx->width * ctx->height;
    for (int i = 0; i < n; i++) p[i] = color;
}

/* Back-buffer reference pattern: green top half, blue bottom half. A clean
 * horizontal split only scans out correctly when the start address AND the
 * stride match; a wrong start unit shears it into diagonals or shows RED. */
static void cpu_draw_split(struct virge_ctx *ctx, uint32_t byte_off)
{
    uint8_t *base = (uint8_t *)ctx->fb + byte_off;
    uint16_t green = pack555(0, 31, 0);
    uint16_t blue  = pack555(0, 0, 31);
    for (int y = 0; y < ctx->height; y++) {
        uint16_t *row = (uint16_t *)(base + (size_t)y * ctx->stride);
        uint16_t c = (y < ctx->height / 2) ? green : blue;
        for (int x = 0; x < ctx->width; x++) row[x] = c;
    }
}

/* Program the CRTC display-start address to scan out from byte_off, using a
 * candidate divisor MULT (the register unit under test).
 *   CR0D = start bits 7-0, CR0C = bits 15-8 (classic VGA start addr).
 *   CR69 bits 3-0 = bits 19-16 and, when non-zero, SUPERSEDE CR31[5:4] and
 *   CR51[1:0] (DB019-B PDF p.193), so we drive CR69 unconditionally. CR0C/
 *   CR0D (>= index 8) are not behind the CR11 bit-7 write protect on CR00-07;
 *   CR69 is CR40+, already unlocked (CR39=A5) by virge_init. */
static void set_start(uint32_t byte_off, int mult)
{
    uint32_t v = byte_off / (uint32_t)mult;
    virge_crtc_poke(0x0C, (uint8_t)((v >> 8) & 0xFF));
    virge_crtc_poke(0x0D, (uint8_t)(v & 0xFF));
    virge_crtc_poke(0x69, (uint8_t)((v >> 16) & 0x0F));
}

/* Count 0x3DA bit-3 (live vertical retrace) rising edges over a CPU-time
 * bound. Input Status #1 (0x3DA) bit 3 is high during retrace (classic
 * VGA); ioperm(0x3C0,0x20,1) in virge_init covers 0x3DA. Bounded so a dead
 * bit degrades to "0 edges" instead of a spin. */
static int probe_retrace_edges(int ms)
{
    int edges = 0, prev = 0;
    clock_t end = clock() + (clock_t)((long)ms * CLOCKS_PER_SEC / 1000);
    do {
        int v = (inb(0x3DA) >> 3) & 1;
        if (v && !prev) edges++;
        prev = v;
    } while (clock() < end);
    return edges;
}

/* Does the VSY INT status latch set within a CPU-time bound? ctrl_bits are
 * OR'd onto the Subsystem Control write (0 = interrupt disabled; VSY_ENB_BIT
 * = enabled). Always also set VSY CLR so we start from a cleared latch. */
static int probe_vsy_latch(struct virge_ctx *ctx, uint32_t ctrl_bits, int ms)
{
    virge_write32(ctx, VIRGE_SUBSYS_CONTROL, ctrl_bits | VIRGE_SSC_VSY_CLR);
    clock_t end = clock() + (clock_t)((long)ms * CLOCKS_PER_SEC / 1000);
    do {
        if (virge_read32(ctx, VIRGE_SUBSYS_STATUS) & VIRGE_STATUS_VSYNC)
            return 1;
    } while (clock() < end);
    return 0;
}

int main(void)
{
    struct virge_ctx vctx;
    memset(&vctx, 0, sizeof(vctx));
    if (virge_init(&vctx, 800, 600, 2) < 0) {
        fprintf(stderr, "virge_init failed\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int W = vctx.width, H = vctx.height;
    uint32_t stride = vctx.stride;
    uint32_t back = stride * H;          /* back-buffer byte offset */

    printf("\nfliptest: %dx%d stride %u; front @0x0, back @0x%x\n",
           W, H, stride, back);

    /* Save the original start-address registers; restore at exit so the
     * console is left as we found it (cleanup restores timing, not these). */
    uint8_t s_0c = virge_crtc_peek(0x0C);
    uint8_t s_0d = virge_crtc_peek(0x0D);
    uint8_t s_69 = virge_crtc_peek(0x69);

    /* Draw both reference patterns, start on the front (RED). */
    cpu_fill(&vctx, 0, pack555(31, 0, 0));
    cpu_draw_split(&vctx, back);
    set_start(0, 1);
    printf("Drew RED @0 and GREEN/BLUE split @0x%x. Screen should be RED.\n",
           back);

    /* ---- Vsync detector probe ---- */
    printf("\nVsync detector probe (~60ms windows):\n");
    int edges = probe_retrace_edges(60);
    printf("  0x3DA bit-3 retrace edges : %d  (>=2 => live retrace @~60Hz)\n",
           edges);
    int l_off = probe_vsy_latch(&vctx, 0u, 60);
    int l_on  = probe_vsy_latch(&vctx, VSY_ENB_BIT, 60);
    printf("  VSY INT latch sets        : ENB off=%d  ENB on=%d  (need on=1)\n",
           l_off, l_on);
    printf("  => recommended vsync source: %s\n",
           edges >= 2 ? "0x3DA bit-3 (live retrace)" :
           l_on      ? "VSY INT latch with VSY ENB" :
                       "NONE FOUND -- investigate");

    /* ---- Start-address unit probe ---- */
    printf("\nStart-address unit probe: photograph the screen each hold.\n");
    printf("  RED = front (offset 0); clean GREEN-top/BLUE-bottom = back.\n");
    printf("  The divisor whose hold shows the clean split is the unit.\n");
    int divs[] = { 4, 2, 8, 1 };                 /* dword hypothesis first */
    const char *names[] = { "x4 (dword)", "x2 (word)", "x8 (qword)", "x1 (byte)" };

    while (running) {
        for (int i = 0; i < 4 && running; i++) {
            set_start(back, divs[i]);
            uint32_t v = back / (uint32_t)divs[i];
            printf("  [hold %-10s] start=0x%05x -> CR0C=%02x CR0D=%02x "
                   "CR69=%02x  photograph now\n",
                   names[i], v, (v >> 8) & 0xFF, v & 0xFF, (v >> 16) & 0xF);
            for (int s = 0; s < 3 && running; s++)
                sleep(1);
        }
    }

    /* Restore the original start address, then normal cleanup. */
    virge_crtc_poke(0x0C, s_0c);
    virge_crtc_poke(0x0D, s_0d);
    virge_crtc_poke(0x69, s_69);
    virge_cleanup(&vctx);
    printf("\nRestored original start address; exiting.\n");
    return 0;
}
