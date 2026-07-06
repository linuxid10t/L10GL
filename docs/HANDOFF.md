# L10GL handoff brief — state as of 2026-07-06 (evening)

Audience: an implementing agent picking up this project cold. Read
`PLAN.md` (roadmap + current-state snapshot) and
`docs/datasheets/README.md` (datasheet page index + verified register
facts) before changing code. This file records the live debugging
state, the two open symptoms, and the exact next steps.

## Test setup (fixed, do not re-derive)

- Machine `david-ta970`: S3 ViRGE/DX (PCI id 0x8a01), 4MB VRAM,
  x86-64 Linux, tested over SSH by the human (David). He runs the
  binaries and reports logs + photos of the monitor. You cannot see
  the screen; design every change so one run produces decisive output.
- **There is no `/dev/fb0` on this machine** — no kernel fb driver is
  bound to the card. The chip boots into the bootloader's leftover VBE
  mode: 800×600 raster, CR67 Mode 13, 32-bit pixels, pitch 3200.
- Because of that, `virge_init` performs a **native scanout takeover**
  (`virge_scanout_takeover` in `src/backends/virge/virge.c`): switches
  scanout to Mode 9 (15-bit RGB555), doubles all horizontal CRTC
  timings (15/16bpp modes count two character clocks per 8 pixels on
  this chip family), sets CR50 bits 5-4 = 01b, programs pitch in
  quadwords (LSW = pitch/8, CR43 bit 2 clear), adopts 800×600/stride
  1600 into the ctx, and restores everything at cleanup. **This is
  hardware-verified working** as of commit `301d448`: `scantest`'s
  CPU-drawn full-screen pattern displays correctly (border, corner
  markers, color bands, 555 discriminator green).

## Ground rules (from PLAN.md, non-negotiable)

- Register facts need citations from `docs/datasheets/` (DB019-B, by
  PDF page). Where the databook is silent (per-depth timing scaling,
  CR50), the kernel `s3fb` driver and 86Box are the documented
  behavioral references — consult, never copy code (86Box is GPL).
- One logical change per commit; never mix refactor with behavior.
  Every commit builds clean: `make -B BACKEND=virge && make scantest`.
- Never write an unbounded register-poll loop (see the
  `virge_wait_vsync` latched-VSY-INT lesson in virge.c).
- State in each commit message exactly what the human should observe
  on hardware.

## Open symptom 1: dark vertical band at ~1/3 of screen width

In the otherwise-correct `scantest` pattern there is a small dark
vertical band roughly one third across the screen. It appears in
**CPU-drawn** content, so it is scanout-side (display fetch), not
engine-side. Hypotheses, in order:

1. **Monitor auto-adjust.** An analog VGA LCD that just saw the
   timing change may simply need auto-adjustment. Ask David to press
   the monitor's auto-adjust button first. Zero-cost.
2. **CR3B (Start Display FIFO Fetch)** — the takeover recomputes it as
   `new_CR00_value - 5` per the "typically 5 less than the value
   programmed in CR0" rule (DB019-B §18, PDF p.203, register CR3B;
   bit 8 in CR5D bit 6; only active if CR34 bit 4 = 1). If the rule or
   the doubling interacts badly, the display FIFO refetch happens at a
   fixed horizontal position → a vertical artifact band. Try: dump
   CR34/CR3B in the boot log; test with CR34 bit 4 cleared (disables
   CR3B) or with the BIOS's *relation* preserved (old_SFF relative to
   old_CR00, doubled) instead of the -5 rule.
3. **EHB/EHS re-encode** — blank/sync end are modulo fields
   re-encoded by the takeover (virge.c, `virge_scanout_takeover`). A
   wrong bit would typically black a column range. Verify decode →
   double → re-encode against DB019-B §16 PDF pp.148-153 (CR00 =
   chars-5, CR01 = chars-1, CR02/CR04 raw, CR03/CR05/CR5D field
   layout) with the actual register values from the boot log's
   "CRTC raw" lines.

## Open symptom 2: engine writes don't land (the next big one)

`./triangle` leaves the **previous scantest pattern** on screen with
some noise at the top. Scanout is correct (symptom 1 aside), so this
means the 2D clear (`virge_fill_rect`), Z-clear, and 3D triangle are
not writing the framebuffer where/how they should. The "noise at top"
is probably where the engine output actually went.

What to check, in order:

1. Re-verify the 2D fill on the now-correct scanout: a standalone test
   that fills known rectangles at known coords (e.g. 100×100 at
   (50,50), full-width bar at y=300) and then **CPU-reads back** VRAM
   through `ctx->fb` at stride 1600 and prints pass/fail per corner —
   plus a photo. That separates "engine writes wrong address/stride"
   from "engine doesn't run".
2. The engine register programming after the takeover: `engine_init_3d`
   and every 2D op program `VIRGE_2D/3D_DEST_SRC_STR` with
   `ctx->stride` (1600) and clip/width with `ctx->width/height`
   (800/600) — confirm in the boot log ("Screen: 800x600 ... stride
   1600") and re-read the datasheet units (2D width in pixels at 16bpp,
   stride in bytes, DB019-B PDF p.232/246).
3. PLAN.md's two old mysteries — "row ceiling ~299" and "narrow widths
   fill nothing" — were measured through the old wrong scanout and are
   flagged likely-artifacts, but if fills still misbehave now they are
   real engine bugs: re-run those exact tests on the fixed scanout
   before theorizing.
4. CR50: the takeover sets only bits 5-4 (pixel length, per s3fb). On
   older S3 cores CR50 bits 7-6/1-0 also encode a "GE screen width"
   used by the blit engine. DB019-B does not document CR50 at all;
   86Box's `vid_s3_virge.c` is the reference for whether the ViRGE 2D
   engine consumes it. If it does, the engine may be blitting with a
   wrong internal pitch — which would fit symptom 2.
5. The V8-era color packing and V9/V10 triangle fixes are already in;
   once fills land correctly, `./triangle` should show a clean scalene
   triangle with a smooth red→green→blue ramp. Shape/gradient defects
   at that point are V7/V9 sub-pixel issues (see PLAN.md Phase 0).

## Diagnostic inventory

- `sudo ./scantest` — phase 1: three CPU-drawn layout-hypothesis strips
  (C = 555/1600 must be the clean one); phase 2: full-screen CPU
  pattern at adopted geometry. No engine involvement.
- `sudo ./triangle`, `./cube` — engine paths (2D clear + Z-clear + 3D).
- `sudo ./fbtest` — fbdev-based pattern; useless on this machine (no
  /dev/fb0), kept for machines that have one.
- Boot log prints: FB/fbdev status, "CRTC raw"/"CRTC truth" dump
  (pre-takeover), takeover geometry + register readbacks, engine
  banner. Always ask for the full log with any photo.
- Reference sources: `docs/datasheets/DB019-B_*.pdf` (use `pdftotext -f
  <page> -l <page> -layout`), kernel `drivers/video/fbdev/s3fb.c`
  (fetch from kernel.org), 86Box `src/video/vid_s3_virge.c`.

## Recent commit trail (context for git archaeology)

`b155aaa` CRTC truth dump · `d3cca61` scantest v1 · `266ac9f`
wait_vsync latch fix · `bc0fc30` takeover v1 (CR67-only — put the
monitor out of range) · `50cab91` demos adopt geometry · `301d448`
takeover v2 (hmul=2 + quadword pitch — works) · `3671c38` PLAN.md
update. Read `301d448`'s message for the full timing-scaling story.
