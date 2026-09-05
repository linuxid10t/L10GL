# L10GL — Lightweight Legacy OpenGL Driver Framework

L10GL is a userspace OpenGL-style driver framework for vintage fixed-function
graphics hardware. Applications call a small hardware-independent API; L10GL
detects a supported PCI card and dispatches directly to its MMIO drawing
engine, or falls back to a plain-C software reference rasterizer.

There is no DRM, DRI, Mesa, kernel module, X11, or GLX. The current target is a
full-screen Linux console, programmed much like graphics hardware was in the
1990s.

## Current status

The S3 ViRGE is the primary backend and is tested on a real ViRGE/DX with 4 MB
of VRAM. The following paths are verified on silicon:

- Gouraud-shaded, depth-tested triangles
- perspective-correct texture mapping with repeat wrapping
- nearest and bilinear texture filtering
- depth-test, depth-mask, and depth-function state plumbing
- 2D rectangle fills
- native RGB555 scanout takeover when no fbdev driver owns the card
- P1 requested/actual geometry and stride publication on native takeover
- double-buffered, vsync-synchronized page flips

The X6 model-space `cube` and `textured_cube` ports render correctly and
tear-free on that machine, and produce byte-identical first frames to their
former screen-space implementations under swrast. The Matrox MGA-1064SG
backend now includes VRAM-capacity-aware, vsync-synchronized page flipping and
clean scanout/console restoration, but has not yet been validated on hardware.
The software backend provides deterministic,
double-buffered offscreen rendering and pixel-level tests on machines without
either card. Its fbdev path performs real vblank-activated page flips when the
driver exposes two mapped virtual pages, and otherwise uses direct
single-buffered rendering rather than a visibly torn CPU copy.

P2 console ownership is also verified on the target system: direct swrast
through the 800x600x32 `simple-framebuffer` moved VT1 to `KD_GRAPHICS`, kept the
static frame free of console scribble, and restored the fbdev mode and console
ownership on Ctrl-C. The `l10gl-run` no-fbdev ViRGE path remains unchanged.

The frontend now also has OpenGL-convention MODELVIEW and PROJECTION matrix
stacks plus an immediate-mode model-space geometry path. It captures current
color/normal/texture attributes, assembles triangles, strips, fans, and lines,
transforms them, applies opt-in directional plus ambient material lighting,
clips triangles against the full homogeneous frustum, performs CCW face
culling, and emits perspective-correct texture W with the established
screen-space backend primitives. The direct `l10gl_draw_triangle` API remains
available and unchanged.

Phase 4 has started with a real `<GL/gl.h>` compatibility surface. Its first
checkpoint maps immediate triangles/strips/fans/lines/quads/quad strips, current vertex
attributes, matrix stacks and projection calls, viewport/depth range, masked
clears, depth/blend/cull/lighting state, synchronization, and GL error
reporting onto that frontend. Directional LIGHT0, ambient/diffuse material,
and flat/smooth shading are sufficient for the new `gears` demo, now verified
under both swrast and the target ViRGE/DX. L10GL still owns fullscreen context
creation and buffer swapping.

The texture-object slice now supports GL texture names, level-zero RGB/RGBA
unsigned-byte uploads, unpack alignment, nearest/linear filtering, and
repeat/clamp selection. For a safe common hardware contract, images must be
square powers of two no larger than 512x512. The new `gltexture` proof and
pixel-level `test-gl` integration are verified under swrast, and the repeated
RGBA8888 checker proof renders correctly on the target ViRGE/DX.

Phase 4 is complete: both `gears` and `gltexture` run correctly through the
OpenGL compatibility surface on swrast and the ViRGE hardware backend.
Matrox parity is deferred until a Mystique is installed for testing; the
existing MGA-1064 code and regression tests remain in the tree. Performance
work on the verified ViRGE path is next.

The animated `cube`, `textured_cube`, and `gears` demos report completed-frame
FPS every two seconds and a whole-run average at exit. Normal measurements
include engine completion, page-flip presentation, and vsync. The ViRGE backend
also accepts `L10GL_VSYNC=0` for visible direct-front rendering: swaps retain
the engine-completion barrier but skip page flips and retrace waits. This raw
mode can tear because clears and drawing are visible while they happen. Use
the same presentation mode, resolution, and `L10GL_FRAMES` count for meaningful
before/after comparisons.

| Backend | Hardware | Status |
|---|---|---|
| `virge` | S3 ViRGE family | Primary; ViRGE/DX verified on silicon |
| `mga1064` | Matrox Mystique/MGA-1064SG | Double-buffering complete; hardware-unverified |
| `swrast` | No graphics hardware required | Double-buffered offscreen; fbdev pan or direct fallback |

The detailed hardware history and test evidence live in
[`docs/HANDOFF.md`](docs/HANDOFF.md). The implementation roadmap is
[`PLAN.md`](PLAN.md). The active roadmap is Quake compatibility,
[`docs/QUAKE_PLAN.md`](docs/QUAKE_PLAN.md) (Phase 7); the maximum OpenGL 1.1
compatibility roadmap that follows it, including the
native-ViRGE/software-fallback boundary, is
[`docs/GL11_PLAN.md`](docs/GL11_PLAN.md) (Phase 8). X11/GLX support is planned
in [`docs/GLX_PLAN.md`](docs/GLX_PLAN.md) (Phase 9), starting with software
GLX 1.2 and a later native-ViRGE ownership investigation; it is not implemented.

## Architecture

```text
Application / demo
        │
        ▼
Optional OpenGL 1.1 compatibility shim (`include/GL/gl.h`)
        │
        ▼
Transform layer (matrix stacks + immediate primitive pipeline)
        │
        ▼
L10GL frontend (render-state cache + runtime backend registry)
        │
        ├── S3 ViRGE glue ────── ViRGE register driver
        ├── MGA-1064 glue ────── MGA-1064 register driver
        └── swrast ───────────── plain-C reference rasterizer
```

Each backend implements `struct l10gl_backend` from `src/l10gl.h`. The frontend
owns common render state and dispatches through that vtable. Backend glue
converts generic vertices and state into the low-level chip driver's formats.

PCI discovery is shared in `src/pci_scan.c`. Detection is read-only: ViRGE is
tried first, followed by MGA-1064, then the always-available software fallback.
Initialization and hardware access only begin after a backend has been
selected.

## Requirements

- GCC, GNU Make, and standard Linux development headers

Offscreen swrast needs no graphics hardware or elevated privileges. Hardware
backends additionally require Linux on x86, a supported PCI card, permission
to use legacy VGA I/O ports, and normally root privileges for PCI resource
mappings and I/O-port access.

The ViRGE test machine intentionally has no `/dev/fb0`. In that configuration,
the backend adopts the live CRTC raster and changes scanout to the RGB555 format
required by the ViRGE 3D engine. A conventional fbdev console is still expected
by parts of the MGA path.

## Temporarily disable the kernel framebuffer

Use the reversible launcher when a kernel framebuffer or DRM fbdev-emulation
driver owns the card:

```sh
sudo tools/l10gl-run -- ./cube
```

The launcher selects the same card as L10GL, detaches every bound framebuffer
console (`fbcon`), unbinds the driver that owns `/dev/fb0`, and unbinds the
selected PCI function if it has a different driver. After the program exits it
rebinds the exact drivers in reverse order and then reattaches `fbcon`. It
finally switches to a spare virtual console and immediately back so fbcon
repaints its retained text over any L10GL color/Z data left in VRAM.

Inspect the complete plan without changing kernel state:

```sh
sudo tools/l10gl-run --dry-run -- ./cube
```

Backend and card overrides are supported:

```sh
sudo env L10GL_BACKEND=virge tools/l10gl-run \
    --device 0000:01:00.0 -- ./cube
```

Run this from SSH when possible: detaching `fbcon` can blank the local console
until L10GL takes over. Normal exits and signals restore ownership, but
`SIGKILL`, a launcher crash, or a system crash cannot run cleanup. Do not use
the launcher on a GPU serving an active graphical desktop.

The first opt-in P6 native-modeset hardware gate is restricted to the already
proven 800x600@60 raster. Its hardware-verified path preserves the live
vertical timing and changes only the proven scanout subset plus the
programmable DCLK. Run it over SSH:

```sh
sudo env L10GL_BACKEND=virge L10GL_MODESET=native \
    tools/l10gl-run -- ./cube 800 600 16
```

This test programs and verifies the ViRGE DCLK/CRTC image and restores the
complete saved register state on exit. The first true resolution-change gate,
640x480@60, is also hardware-verified:

```sh
sudo env L10GL_BACKEND=virge L10GL_MODESET=native \
    tools/l10gl-run -- ./cube 640 480 16
```

This uses the complete fixed CRTC image plus the ViRGE's built-in 25.175MHz
VGA clock. The validated encoder leaves CR5D pulse-length extensions clear for
the standard blank and sync widths. The hardware-verified 75Hz gate keeps the
same resolution and selects its fixed timing plus a 31.5MHz programmable DCLK:

```sh
sudo env L10GL_BACKEND=virge L10GL_MODESET=native L10GL_REFRESH=75 \
    tools/l10gl-run -- ./cube 640 480 16
```

Its initial top-half blanking exposed an off-by-one CR16 vertical-blank wrap;
the corrected image displays the complete cube. Omitting `L10GL_REFRESH`
selects the 60Hz default. The hardware-verified 800x600@75 gate applies the
complete timing image:

```sh
sudo env L10GL_BACKEND=virge L10GL_MODESET=native L10GL_REFRESH=75 \
    tools/l10gl-run -- ./cube 800 600 16
```

The 1024x768 entries are intentionally unavailable on the 4MB target: two
RGB555 color pages plus a 16-bit Z buffer require 4,718,592 bytes before any
texture allocation. Their fixed timings remain covered by encoder tests.

The detach/reattach sequence follows the Linux kernel's
[`fbcon` documentation](https://docs.kernel.org/fb/fbcon.html) and the PCI
driver [`bind`/`unbind` sysfs ABI](https://docs.kernel.org/admin-guide/abi-testing-files.html#abi-sys-bus-pci-drivers-unbind).

## Build and run

Build the static library, all backends, demos, and retained diagnostics:

```sh
make
make check
```

`make check` exercises the launcher fixture, console ownership/restore state
machine, and swrast output pixels for
top-left coverage, blending, depth ordering, perspective correction, bilinear
filtering, RGB565 conversion, and PPM serialization. It also validates matrix
ordering, stack bounds, projections, viewport conversion, depth range,
attribute capture, primitive assembly, texture dispatch, face culling, the
Phase 4 GL-to-L10GL state/matrix/immediate-mode mappings, and the
requested-versus-actual display-mode contract (including padded stride and
RGB555/RGB565/RGB888 channel layouts).

### GLQuake swrast acceptance gate

Phase 7's Q9 gate builds the sibling private `L10GL-Quake` port, fetches the
untracked redistributable shareware data when needed, and runs `timedemo demo1`
through the offscreen swrast backend. It verifies the canonical **969-frame**
result and keeps the log plus PPM capture sequence in a fresh ignored results
directory (roughly 220 MiB at the default 320x240 capture size):

```sh
tools/quake-swrast-gate
```

The command never copies Quake source or game data into this repository. It
stages a symlink to `pak0.pak` in the results directory, so GLQuake's generated
configuration also stays there. Use `--quake-dir`, `--output-dir`,
`--skip-fetch`, `--skip-build`, or `--timeout` to control a local run; the
script's `--help` documents each option. The lightweight fixture runs as part
of `make check`; the actual gate is intentionally an explicit integration run
because it downloads shareware data and creates the capture artifacts.

For the 4 MiB ViRGE/DX, the private GLQuake port starts with
`gl_max_size 256` and `gl_picmip 2`. This policy must be active before
`Draw_Init`; command-line `+` commands arrive too late for bootstrap textures.
The Q13 quality policy selectively raises brush textures whose original sides
are at most 128 texels to picmip 1; larger brush textures and every model or
sprite skin retain picmip 2. Fixed charset, HUD scraps, and other UI textures
remain readable. The canonical shareware `demo1`/E1M3 trace peaks at
2,187,328 texture bytes, leaving 163,776 bytes after 640x480 RGB555 double
buffers and 16-bit Z. L10GL-Quake commit `c1693ca` also releases mipmapped
world/model/sprite textures and old lightmap atlases between maps. The gate's
E1M1-to-E1M2-to-E1M1 cycle peaks at 2,232,384 bytes, leaving 118,720. Inspect
any gate run with:

```sh
tools/quake-vram-budget --trace /path/to/results/textures.tsv
```

The ViRGE allocator uses first-fit reuse and coalescing, so dynamic lightmap
re-uploads replace their old storage. If no block is large enough it prints
the requested and total free byte counts; `glTexImage2D` reports
`GL_OUT_OF_MEMORY` and does not write beyond detected VRAM.

### GLQuake ViRGE phase acceptance

Phase 7's final Q13 gate is an interactive target-machine run. Log in directly
on a physical Linux text VT (`/dev/ttyN`), not through SSH, while no other
graphics workload owns the card. A reboot is not required. Run:

```sh
sudo tools/quake-virge-gate
```

Use this full gate only for final Q13 acceptance. For a narrow diagnostic,
use the smallest direct command that exercises that behavior; do not copy the
gate's expanded defaults into an operator command. Target checkouts are
`$HOME/L10GL` and `$HOME/L10GL-Quake` (no username-specific paths). Finish and
batch local diagnostics before requesting a physical-console check so one
target visit can cover the complete observation list. The mandatory agent
protocol and the minimal E1M1-to-E1M2 discriminator are documented in
`docs/HANDOFF.md` under "Q13 operator-effort protocol."

By default the runner verifies or fetches the shareware pak and lets `make`
confirm that the statically linked GLQuake binary contains the current L10GL
library. `--skip-fetch` and `--skip-build` are available for an already
prepared checkout. The gate forces the accepted
ViRGE baseline (native 640x480x16 at 60 Hz, synchronized presentation, and
the non-experimental submission paths) and uses an isolated game directory.
First it starts E1M1 for a complete start-to-exit playthrough and normal
`quit`; then it runs canonical `timedemo demo1` and asks for Ctrl-C after the
result appears. Follow the on-screen checklist for lighting and console
recovery.

The default runner validates sudo's original `SUDO_TTY`, rejecting SSH PTYs
and noninteractive launches before touching the framebuffer. Current sudo may
run the privileged command on a proxy `/dev/pts` even when invoked correctly;
the gate passes the original `/dev/ttyN` to GLQuake for keyboard-mode control.
Sudo remains the physical-VT reader and forwards medium-raw bytes through the
child's stdin. The gate requires this proxy-aware input build before touching
the framebuffer, then checks the renderer/mode/lightmap/keyboard markers,
E1M1-to-E1M2 transition, map-texture reclamation, 969-frame timedemo result,
absence of unexpected signals or ViRGE timeout/OOM diagnostics, and the
Ctrl-C signal path. It retains `play.log`, `timedemo.log`, and an
operator-attested `q13-report.txt` under a fresh ignored `out/quake-q13.*`
directory.
Use `--quake-dir`, `--output-dir`, or `--runner` for non-default checkout and
launcher locations.

### Software rendering and frame dumps

Force offscreen swrast, render a bounded sequence, and write one PPM per frame:

```sh
env L10GL_BACKEND=swrast \
    L10GL_SWRAST_DUMP='frame%04d.ppm' \
    L10GL_FRAMES=1 \
    ./cube 640 480 16

env L10GL_BACKEND=swrast \
    L10GL_SWRAST_DUMP='textured%04d.ppm' \
    L10GL_FRAMES=1 \
    ./textured_cube 640 480 24
```

Offscreen output supports 16-bit RGB565 and 24-bit RGB888. The dump template
accepts one `%d` or zero-padded `%0Nd` frame conversion; without a conversion,
each frame replaces the same file. A one-shot application that never swaps is
dumped during context cleanup.

To draw through an existing packed true/direct-color fbdev mode instead, opt in
with `L10GL_SWRAST_FB`:

```sh
sudo env L10GL_BACKEND=swrast L10GL_SWRAST_FB=/dev/fb0 ./cube
```

This uses a packed 16-, 24-, or 32-bit fbdev mode. The demo geometry/depth is a
request: if the current mode differs, L10GL asks the framebuffer driver to
switch it, re-reads the actual mode and stride, and fails clearly if the driver
refuses or ignores the request. Before that negotiation, L10GL saves the
original fbdev mode and puts the active owning VT into `KD_GRAPHICS`; normal
cleanup and the demos' handled SIGINT/SIGTERM paths restore the original mode
and prior KD state. Offscreen swrast and no-fbdev native takeover do not touch
a VT.

Run a demo as root:

```sh
sudo ./cube
sudo ./textured_cube
sudo ./gears
sudo ./gltexture
sudo ./cube 800 600 16
```

Frontend demos select hardware at runtime. Force a particular backend when
testing discovery or a multi-card machine:

```sh
sudo env L10GL_BACKEND=virge ./cube
sudo env L10GL_BACKEND=mga1064 ./cube
env L10GL_BACKEND=swrast L10GL_FRAMES=1 ./cube
env L10GL_BACKEND=swrast L10GL_FRAMES=1 ./gears
env L10GL_BACKEND=swrast L10GL_FRAMES=1 ./gltexture
```

Collect the ViRGE performance baseline over SSH before enabling Phase 6
submission optimizations:

```sh
sudo env L10GL_FRAMES=600 tools/l10gl-run -- ./cube 800 600 16
sudo env L10GL_FRAMES=600 tools/l10gl-run -- ./textured_cube 800 600 16
sudo env L10GL_FRAMES=600 tools/l10gl-run -- ./gears 800 600 16
```

Retain the `L10GL FPS:` interval lines and the `L10GL FPS final:` line from
each run. Vsync may cap lightweight workloads near the monitor refresh rate;
the heavier gears workload helps expose submission overhead.

The pre-optimization ViRGE/DX baseline at 800x600 RGB555 over 600 frames is
57.37 FPS for `cube`, 30.01 FPS for `textured_cube`, and 30.13 FPS for
`gears`. Phase 6 FIFO-aware submission uses the documented MM8504 S3d
free-slot count to queue bounded register groups while the rasterizer is busy;
full engine drains remain at CPU VRAM and page-flip boundaries.
The hardware-verified FIFO result is 57.74, 30.13, and 30.13 FPS respectively;
the sub-one-percent changes are effectively neutral under this
presentation-limited workload.

Dirty-state tracking is also hardware-verified. It preserves FIFO ordering and
selectively re-arms the two 3D values known to be clobbered by 2D commands on
DX silicon. Its synchronized results were 58.49, 30.11, and 30.13 FPS; the
30-FPS pair demonstrates half-refresh quantization rather than equal raw
rendering cost.

Measure completed rendering without the retrace quantization using the opt-in
visible direct-front mode:

```sh
sudo env L10GL_VSYNC=0 L10GL_FRAMES=600 \
  tools/l10gl-run -- ./cube 800 600 16
sudo env L10GL_VSYNC=0 L10GL_FRAMES=600 \
  tools/l10gl-run -- ./textured_cube 800 600 16
sudo env L10GL_VSYNC=0 L10GL_FRAMES=600 \
  tools/l10gl-run -- ./gears 800 600 16
```

The init log must say `direct front buffer`; tearing or visible partial clears
are expected. This mode uses one color buffer, places Z immediately after it,
and reclaims one 960,000-byte 800x600 RGB555 page for textures. Omit the
variable (or set `L10GL_VSYNC=1`) for normal tear-free presentation.

The first direct-front hardware results were 63.85 FPS for `cube`, 32.24 FPS
for `textured_cube`, and 34.56 FPS for `gears`. The latter two runs were
interrupted at 297 and 217 frames because direct-front tearing is intentionally
severe, but their interval rates were stable. These values show that the two
30-FPS synchronized results occupied the same two-retrace presentation bucket,
not that the workloads cost the same.

ViRGE/DX hardware testing rejected DB019-B autoexecute as an optimization.
With `L10GL_AUTOEXEC=1`, cube fell to 22.19 FPS, textured cube fell to 4.58
FPS and rendered incorrectly, and gears fell to 26.51 FPS. The exact legacy
control restored textured cube to its 30.11-FPS synchronized baseline. Normal
operation therefore uses the silicon-proven B500-per-triangle launch by
default. Autoexecute remains available only as an explicit diagnostic through
`L10GL_AUTOEXEC=1`; it must not be used for production rendering on the tested
ViRGE/DX.

ViRGE/DX hardware testing also rejected Phase 6 item 4 triangle-parameter
reuse. Even with the proven B500-per-triangle launch retained, suppressing
identical color/Z, texture, and edge values produced severe, unstable visual
corruption. The register descriptions do not guarantee that execution leaves
the interpolation state safe for partial reprogramming. Normal operation
therefore always emits the full parameter image through the default
`L10GL_TRI_REUSE=0`. Strict `L10GL_TRI_REUSE=1` remains only to reproduce the
rejected experiment and now warns that corruption is expected; do not use it
for production rendering on ViRGE/DX.

An unknown override is rejected and prints the available backend names. If no
supported card is present, automatic selection uses offscreen swrast without
attempting MMIO access; it prints a reminder when no dump path was configured.

`make` produces `libl10gl.a`. A native application can include `src/l10gl.h`,
link the archive and `libm`, then create a context with
`l10gl_create_auto()`. A Phase 4 application instead includes `<GL/gl.h>`
with `-Iinclude`, calls `l10glCreateContext(width, height, bits_per_pixel)`,
uses the implemented GL subset, presents with `l10glSwapBuffers()`, and ends
with `l10glDestroyContext()`.

Texture storage follows the vintage backends' lifetime: `glDeleteTextures`
releases the GL name and unbinds it, while the uploaded swrast allocation or
ViRGE VRAM bump allocation is reclaimed when the context is destroyed.
Backends expose one active filter and one wrap selector, so the most recently
specified min/mag filter and S/T wrap value is effective for each object when
it is bound. ViRGE implements `GL_CLAMP` with its wrap-disabled border
behavior; `GL_REPEAT` is the portable hardware path.

## Diagnostics

The repository retains the small ViRGE probes used to settle hardware behavior:
`scantest`, `filltest`, `tritest`, `gltritest`, `fliptest`, `dztest`, `seamtest`,
`cubefb`, `diagap`, and `texprobe`. They build with the normal `make` invocation
and intentionally bypass some frontend abstractions.

`rawtri` is the canonical static raw screen-space Gouraud bring-up demo.
`triangle` remains as a compatibility executable built from the same source.

Use them only for the investigation described in their source comments and in
`docs/HANDOFF.md`; several directly manipulate scanout or inspect VRAM.

## Important limitation on exit

On the no-fbdev ViRGE/DX test machine, Ctrl-C restores the original CRTC mode
and scanout address, but it does not restore the original console pixels. The
BAR0 CPU mapping is write-combined and reads as zero, so the existing CPU
snapshot cannot capture the console. The parked fix is an engine-assisted
BitBLT to temporary VRAM (or a system-memory transfer path). Until then, the
last rendered frame remains visible in the restored console mode.

## Project layout

```text
src/
├── l10gl.c, l10gl.h             frontend API and backend registry
├── console.c, console.h         fbdev mode save/restore and VT ownership
├── fbdev.c, fbdev.h             shared mode negotiation/description
├── pci_scan.c, pci_scan.h       shared PCI sysfs discovery
└── backends/
    ├── virge/                   S3 ViRGE glue and register driver
    ├── mga1064/                 Matrox Mystique glue and register driver
    └── swrast/                  software reference rasterizer
demos/                           demos and hardware diagnostics
tests/                           launcher and swrast regression tests
tools/l10gl-run                  reversible fbcon/driver handoff launcher
docs/datasheets/                 primary hardware documentation
docs/HANDOFF.md                  silicon results and active handoff
PLAN.md                          phased implementation roadmap
```

See [`docs/BACKEND.md`](docs/BACKEND.md) before adding another card.
Transform conventions and the X1 API are documented in
[`docs/XFORM.md`](docs/XFORM.md).
Immediate submission, homogeneous frustum clipping, and current limits are
documented in
[`docs/PIPELINE.md`](docs/PIPELINE.md).

## License

MIT
