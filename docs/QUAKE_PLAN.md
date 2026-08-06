# Quake compatibility plan

This document is the execution plan for Phase 7 of `PLAN.md`: making GLQuake
run on L10GL. It replaces the previous ordering in which full OpenGL 1.1
compliance was Phase 7; that work is now Phase 8 (`docs/GL11_PLAN.md`) and
builds directly on what lands here.

The priority is **up and running first**: a real GLQuake binary booting,
rendering, and completing `timedemo demo1` on the swrast backend is the
milestone that everything in Stage 1 and Stage 2 serves. ViRGE hardware
support (Stage 3) comes after the game is proven correct in software, exactly
as the rest of the project validates frontend work against swrast before
touching silicon.

## Why Quake is the right Phase 7 target

GLQuake (id Software, 1997) is the canonical era-correct workload for this
hardware class, and it is a *small* GL client: pure GL 1.0-style immediate
mode, no vertex arrays, no display lists, no stencil, no GLU dependency
(its `MYgluPerspective` calls `glFrustum` directly). Most of its state
surface already exists in `src/l10gl_gl.c` after Phase 4:

- immediate mode with triangles/strips/fans/quads, `glVertex2f/3f/3fv`,
  `glTexCoord2f`, `glColor3f/4f/4fv`;
- both matrix stacks, `glFrustum`/`glOrtho`/`glViewport`/`glDepthRange`
  (so `gl_ztrick`'s alternating `GL_LEQUAL`/`GL_GEQUAL` scheme and the
  weapon depth-range hack work — all eight depth functions are mapped);
- `glCullFace(GL_FRONT)`, depth test/mask, `glShadeModel`;
- texture objects, RGB/RGBA `glTexImage2D` (Quake expands its 8-bit
  palette to RGBA itself, and its `gl_solid_format`/`gl_alpha_format`
  integer internal formats 3 and 4 are already accepted);
- the full 1.1 blend-factor set on swrast, including the
  `glBlendFunc(GL_ZERO, GL_SRC_COLOR)` multiply that the lightmap pass
  needs (`src/backends/swrast/swrast.c`);
- fullscreen context ownership, double-buffered presentation, and console
  restore — the exact services GLQuake otherwise gets from GLX/wgl.

What is missing is a bounded list of entry points and four real semantic
gaps (rectangular textures, alpha test, `glTexSubImage2D`, texture
lifetime). Phase 7 closes exactly that list and nothing more; everything
else GL 1.1 requires stays in Phase 8.

## Scope, non-goals, and the license boundary

- **Target application:** the original id Software GLQuake source release
  (GPL-2.0), built for Linux, with its video/input layer ported to
  L10GL's `l10glCreateContext`/`l10glSwapBuffers` calls — the same
  substitution `gears` proved in Phase 4.
- **License boundary:** GLQuake's source is GPL-2.0 and this project is
  MIT. Follow the established 86Box rule (`PLAN.md` F4): run it as a
  separate program, never copy its code into this repository. The port
  (a fork of id's tree with a `vid_l10gl.c`/`in_l10gl.c`) lives in a
  **separate repository**, `L10GL-Quake` (`origin` =
  `github.com/linuxid10t/L10GL-Quake`, `upstream` =
  `github.com/id-Software/Quake`, forked at the id GPL release commit
  `bf4ac42`); this repository gains only GL API features, tests, and
  documentation. Nothing in `libl10gl.a` may include or link GPL code.

  **2026-07-28 (David):** `L10GL-Quake` stays private for now, so Q9 edits
  the vendored source directly in that repo (platform-layer files, the
  build system, small portability fixes) instead of following a strict
  read-only-audit-then-reimplement discipline — that discipline was in
  service of a public-distribution GPL/MIT boundary that doesn't bind a
  private repo the same way. The two rules that don't move: no GPL source
  enters this MIT `L10GL` tree, and the honest pre-1.1 version-string rule
  stays in force regardless. Revisit the private/public question, and
  whether any port-side changes need a distribution notice, if
  `L10GL-Quake` is ever made public.
- **Test data:** the Quake shareware episode (`pak0.pak`) is
  redistributable in unmodified form and drives all automated testing
  (`timedemo demo1`). Do not commit game data to this repository; the
  test rig fetches it.
- **Non-goals for Phase 7:** sound (Quake runs fine with `-nosound`),
  networking beyond what compiles by default, multitexture
  (`GL_SGIS/ARB_multitexture` — GLQuake probes and falls back cleanly),
  screenshots on hardware (`glReadPixels` from VRAM is a known-unreliable
  path, Phase 8 C7), and any GL feature Quake does not call.

## GL usage manifest (Q0 — authoritative)

Audited against the id Software GLQuake GPL-2.0 source release
(`id-Software/Quake`, commit `bf4ac42`), `WinQuake/gl_*.c` GL renderer and
`gl_vidlinuxglx.c` platform layer — the tree the Q9 port forks. The audit
is read-only and out-of-tree (the GPL boundary forbids the source from
entering this repository). The figures below come from a comment-stripped
sweep of *direct* core GL calls; the manifest that drives the automated
gate is `tests/quake_gl_symbols.def`, exercised by
`tests/test_quake_linkgate.c`.

**Link posture.** The Linux GLX build calls core GL directly (no `qgl`
function-pointer indirection for core functions), so every entry point in
the table must be present in `libl10gl.a` for the port to link. Extensions
are runtime-probed and skipped when `glGetString(GL_EXTENSIONS)` is empty,
so they are **not** link dependencies:

- `glMTexCoord2fSGIS` / `glSelectTextureSGIS` — SGIS_multitexture,
  `dlsym`'d into `qgl*` pointers in `gl_vidlinuxglx.c:554-555`; clean
  fallback to single-texture.
- `glColorTableEXT` / `gl3DfxSetPaletteEXT` — EXT shared texture palette,
  `dlsym`'d, `is8bit = false` path (`gl_vidlinuxglx.c:106`).
- `gl{Array,Color,TexCoord,Vertex,Texture}PointerEXT` — EXT_vertex_array;
  Quake-internal `PROC` pointers (`gl_vidnt.c`, Windows only; the Linux
  port defines/stubs them itself).
- `glBindTextureEXT` — Windows-only `wglGetProcAddress` probe.
- `glFog{,i,fv,f}` — **dead code**: the entire fog block is a commented-out
  "Experimental silly looking fog" section in `gl_rmain.c` (≈line 1131).

**Entry points.** 47 distinct direct GL core calls; 35 are already in
`libl10gl.a`, 12 are the Q0–Q6 gap (the gate reports exactly this set).
The 12 missing symbols, with the subsystem that uses them:

| Symbol | Used for | Source | Owner |
|---|---|---|---|
| `glGetString` | init banner, `GL_EXTENSIONS`/`GL_VENDOR`/`GL_RENDERER`/`GL_VERSION` probe | `gl_rmisc.c`, `gl_rmain.c` | Q1 |
| `glTexParameterf` | min/mag filter + wrap (`gl_texturemode`) | `gl_rsurf.c`, `gl_draw.c` (38 calls) | Q1 |
| `glColor3ubv` | particle color from `d_8to24table` | `r_part.c:720` | Q1 |
| `glPolygonMode` | `GL_FILL` at setup | `gl_rmain.c` | Q1 |
| `glDrawBuffer` | `GL_BACK`/`GL_FRONT` | `gl_rmisc.c`, `gl_rmain.c` | Q1 |
| `glReadBuffer` | `GL_FRONT`/`GL_BACK` around screenshot/envmap | `gl_rmisc.c:112,162` | Q1 |
| `glReadPixels` | screenshots, envmap capture | `gl_rmain.c` | Q1 stub (Phase 8 C7 real) |
| `glHint` | `GL_PERSPECTIVE_CORRECTION_HINT` (`gl_affinemodels`) | `gl_rmain.c:567,575` | Q1 |
| `glGetFloatv` | capture `GL_MODELVIEW_MATRIX` into `r_world_matrix` | `gl_rmain.c:927` | Q1 |
| `glTexSubImage2D` | dynamic lightmap updates | `gl_rsurf.c:444,546,711` | Q4 |
| `glAlphaFunc` | `GL_GREATER 0.666` for console text/HUD/sprites/fence | `gl_draw.c`, `gl_rsurf.c` | Q5 |
| `glTexEnvf` | `GL_MODULATE`/`GL_REPLACE` for alias-model passes | `gl_rmain.c:567` | Q6 |

The other 35 (`glBegin`/`glEnd`, the `glVertex*`/`glTexCoord*`/`glColor*`
immediate family, the matrix stack, `glFrustum`/`glOrtho`/`glViewport`/
`glDepthRange`, clear/enable/disable/cull/depth/blend/shade, `glTexImage2D`,
`glBindTexture`, `glFlush`/`glFinish`) are present from Phase 4. Note
GLQuake **self-manages texture-object names** — it never calls
`glGenTextures`/`glDeleteTextures`/`glIsTexture` (it `glBindTexture`s its
own integer names); those entry points remain for Phase 8 and other clients.

**Tokens.** 60 GLQuake-referenced tokens; 40 are already in
`include/GL/gl.h`. The 20 missing tokens land with the entry point that
uses them (defining a token makes no behavior claim and `GL_EXTENSIONS`
stays empty, so this is honest): `GL_POLYGON` (Q2), `GL_ALPHA_TEST` (Q5),
`GL_TEXTURE_ENV`/`GL_TEXTURE_ENV_MODE`/`GL_MODULATE`/`GL_REPLACE` (Q6),
`GL_LUMINANCE`/`GL_ALPHA`/`GL_INTENSITY`/`GL_RGBA4` (Q7/Q10 formats), and
`GL_FILL`, `GL_PERSPECTIVE_CORRECTION_HINT`, `GL_FASTEST`, `GL_NICEST`,
`GL_MODELVIEW_MATRIX`, `GL_VENDOR`/`GL_RENDERER`/`GL_VERSION`/
`GL_EXTENSIONS`, `GL_FLOAT` (all Q1). `GL_DECAL` is not referenced by
Quake; it arrives with its Q6 behavior. The guarded 8-bit paletted path
also references `GL_COLOR_INDEX`/`GL_COLOR_INDEX8_EXT`; the port defines
the latter locally and Q1 adds `GL_COLOR_INDEX` so the (runtime-skipped)
path compiles.

**Semantic gaps:**

1. **Rectangular textures.** `glTexImage2D` currently rejects
   `width != height` for every backend (`src/l10gl_gl.c`). Quake wall
   textures and skins are routinely non-square powers of two (128×32,
   64×128, the 512×256 console background); even booting to the console
   needs this. Lightmap blocks are 128×128 and unaffected.
2. **Alpha test.** No `GL_ALPHA_TEST` stage exists anywhere. Quake enables
   `glAlphaFunc(GL_GREATER, 0.666)` for console text, HUD pics, sprites,
   and fence textures; without it they render as opaque quads.
3. **`glTexSubImage2D`.** Dynamic lightmaps are updated through subimage
   uploads every frame the world lighting changes.
4. **Texture lifetime.** `glDeleteTextures` frees GL names only; backend
   storage lives until context teardown (swrast allocation list, ViRGE
   VRAM bump allocator). Quake creates a fresh texture set on every level
   load, so a level change or two exhausts memory.

## Stage 1 — API surface: link and boot on swrast

### Q0. GLQuake GL-usage manifest and link gate

Audit the GLQuake source (the id GPL release) and produce a checked-in
manifest — a table in this document plus a machine-readable list under
`tests/` — of **every** GL entry point, token, primitive mode, pixel
format, and internal format the engine references, annotated with the
subsystem that uses it (2D, world, lightmaps, alias models, particles,
sky/water, screenshots). This is the Quake-scoped analogue of Phase 8's
C0 and permanently replaces the informal inventory above.

Add a link-gate test: a translation unit that references exactly the
manifest's symbols must link against `libl10gl.a`. Until Q1 lands the
test documents the failures; afterward it must stay green.

*Acceptance:* the manifest is complete against the actual source (cite
file/function per row); `make check` reports the current link-gate status;
no row is marked "unknown".

### Q1. Trivial entry points, aliases, and tokens

Implement every manifest row that is a conversion or an honest stub:
`glGetString` (real vendor/renderer/version strings; the version string
must keep reporting the honest pre-1.1 tier per the Phase 8 rule; an
empty-but-valid `GL_EXTENSIONS` so GLQuake's multitexture probe cleanly
falls back), `glTexParameterf` delegating to the existing `glTexParameteri`
paths, the byte-color `glColor*` variants Q0 found, `glPolygonMode`
(accept `GL_FILL`, record `GL_INVALID_ENUM`-free state, error on modes the
project does not draw), `glDrawBuffer` (accept `GL_BACK`; `GL_FRONT`
returns an error until a use case exists), and `glReadPixels` as a
documented stub recording `GL_INVALID_OPERATION`. Add all missing tokens
to `include/GL/gl.h`.

*Acceptance:* the Q0 link gate passes; new entry points have `test-gl`
coverage for conversion correctness and error behavior; `glGetString`
output identifies backend and honest version tier.

### Q2. `GL_POLYGON` primitive assembly

Quake draws world surfaces, water, and sky as `glBegin(GL_POLYGON)` with
convex vertex loops. Add `GL_POLYGON` to the shim's primitive map and the
streaming assembler in `src/l10gl_pipeline.c` as a fixed-origin fan
(identical decomposition to `L10GL_TRIANGLE_FAN` for convex input, which
is all GL guarantees anyway), preserving flat-shading provoking-vertex
rules and the no-allocation property. Add `GL_LINE_LOOP` at the same time
if Q0 shows any use; otherwise leave it to Phase 8 C2.

*Acceptance:* pipeline tests cover polygon assembly, winding, culling, and
near-clip interaction; a swrast capture of a textured convex polygon
matches the equivalent triangle fan byte-for-byte.

### Q3. Rectangular power-of-two textures

Relax the shim's square-only rule into a per-backend capability:

- **Common frontend:** accept `width != height` when both are powers of
  two within the backend's limit; keep rejecting non-power-of-two sizes.
- **swrast:** store and sample rectangles natively — the sampler already
  takes independent width/height, so this is mostly removing the
  restriction and testing wrap behavior on both axes.
- **ViRGE:** DB019-B §19.4 (PDF p. 251) defines square 2^s textures only.
  Represent a W×H rectangle as its bounding square with the short axis
  **tile-replicated** to fill the square (e.g. 128×32 stored as 128×128
  containing four vertical repeats). Convert normalized U and V to texels
  with the original width and height independently, while the command's `s`
  field and source stride still describe the bounding square. Replication plus
  those per-axis scales keeps `GL_REPEAT` exact — Quake's world UVs span many
  repeats — at a VRAM cost that Q10's format work offsets. `GL_CLAMP` on a
  replicated axis remains inexact.

  The replication is a pure function (`virge_replicate_to_square`,
  `src/backends/virge/virge.c`) producing `dst[sy][sx] == src[sy%h][sx%w]`;
  `virge_be_tex_image_2d` replicates rectangular uploads into the `max(w,h)`
  square (square textures upload verbatim, byte-for-byte unchanged), and
  `bind_texture` programs the source stride from the square side while saving
  the original dimensions for U/V conversion. VRAM cost =
  `max(w,h)² / (w·h) = max/min`:

  | Rectangle | Stored square | VRAM cost |
  |-----------|---------------|-----------|
  | W×W       | W×W           | 1×        |
  | W×W/2     | W×W           | 2×        |
  | W×W/4     | W×W           | 4×        |
  | W×W/8     | W×W           | 8×        |

  Quake world textures are predominantly square or 2:1, so the typical
  overhead is 1–2×; tall or wide slivers (8:1+) are rare and pay the full
  square. `GL_CLAMP` on the replicated (short) axis stretches wrong because
  the sampler maps the axis over the whole square; `GL_REPEAT` is exact.

*Acceptance:* swrast renders repeated and clamped rectangular textures
correctly under new pixel tests in `make check`; the ViRGE path has a
replication unit test on the stored image plus a human hardware run of a
new `demos/` rectangle-texture proof; square-texture behavior is
unchanged byte-for-byte.

*Status (Q3, swrast gate):* DONE — frontend accepts rectangular POT
(`test_gl.c`), swrast repeat+clamp pixel tests on both axes
(`test_swrast.c`), ViRGE replication unit test (`test_virge_mode.c`),
square path byte-for-byte unchanged. The first full GLQuake silicon run exposed
that ViRGE drawing still scaled both axes by the bounding-square side: square
textures were unaffected, matching the report that weapon/model skins varied
while world textures were corrupt. Independent width/height scaling is now
unit-tested and awaits the Q11 rerun.

The per-axis silicon rerun looked materially unchanged, so that correction is
retained for exact rectangular semantics but is not the primary corruption.

### Q4. `glTexSubImage2D`

Keep the converted ARGB8888 image of every GL texture in shim-side system
memory (the conversion buffer `glTexImage2D` currently frees), apply
subrectangle updates there, and re-upload through the existing
`tex_image_2d` backend call. This is semantically exact and fast enough
for swrast; per-frame full re-uploads on ViRGE are a known cost recorded
for Q12, not a correctness problem. Enforce the specified error behavior
(offsets/sizes within the level, format/type as in `glTexImage2D`).

*Acceptance:* tests cover interior/edge subrectangles, unpack alignment,
and error cases; a swrast scene updated via subimage matches a control
scene uploaded whole; memory accounting shows one retained CPU copy per
texture, freed with the GL name.

### Q5. Alpha test

Add `glAlphaFunc` and the `GL_ALPHA_TEST` enable with full state
(function + reference), a frontend cap bit, and a swrast fragment stage in
specification order (alpha test before depth write and blending, so
rejected fragments touch neither color nor depth). All eight compare
functions, since the plumbing is shared with depth compare.

ViRGE has no alpha-test stage in silicon. For Stage 3, map Quake's usage
(binary 0/255 texture alpha, `GL_GREATER 0.666`) onto the chip's
texture-alpha blend mode (DB019-B §15.4.8.5) with the documented
depth-write caveat, as period miniGL drivers did; record it as an
approximation in the limitations notes. Exactness on hardware is Phase 8
C6/C10 territory.

*Acceptance:* a swrast truth-table test pins every compare function and
the no-depth-write-on-reject rule; the console-font case (text over a
scene, transparent texels invisible, depth untouched) renders correctly in
a pixel test.

*Status (Q5, swrast gate):* DONE — `glAlphaFunc`/`GL_ALPHA_TEST` push full
state (func + clamped ref) into the frontend ctx, swrast runs the test as
a fragment stage before depth/blend (rejected fragments touch neither),
the truth table pins all eight functions (`test_swrast.c`), and the
console-font depth-untouched rule is a pixel test. DEFERRED — the ViRGE
texture-alpha-blend approximation is a Stage 3 hardware task (Phase 8
C6/C10 for exactness).

### Q6. Texture environment: `GL_MODULATE`, `GL_REPLACE`, `GL_DECAL`

The pipeline currently hardwires modulate (vertex color × texel) in both
swrast (`swrast.c` texture path) and ViRGE (`l10gl_virge.c`, TB=01).
Add `glTexEnvf/i` with per-context environment state:

- `GL_MODULATE`: current behavior.
- `GL_REPLACE`: fragment = texel, ignoring vertex color — the mode
  GLQuake sets for world texture passes. Frontend can implement it
  exactly by forcing white vertex color into the modulate path; ViRGE
  additionally has a native decal mode to evaluate.
- `GL_DECAL`: implement per the RGB/RGBA equations in swrast; on ViRGE
  use the native decal blend only if equation-level tests prove it
  matches, per the Phase 8 C5 rule.

`GL_BLEND`-environment and `GL_ADD` stay out of scope (Quake does not use
them; Phase 8 C5/C6).

*Acceptance:* equation tests pin all three modes for RGB and RGBA
textures on swrast; REPLACE with non-white vertex color demonstrably
ignores the color; ViRGE runs a mode-comparison demo for human sign-off.

*Status (Q6, swrast gate):* DONE — `glTexEnvf`/`glTexEnvi` push a frontend
`tex_env_mode` (`L10GL_CAP_TEX_ENV`) and swrast's textured fragment path
applies all three modes. RGB equations are pinned with blend off (MODULATE
`C=v·t`, REPLACE `C=t` ignoring vertex color, DECAL `C=v·(1−ta)+t·ta` on
RGBA / texel copy on RGB); the alpha source is pinned by blending over
black (MODULATE `A=va·ta`, REPLACE `A=ta`, DECAL `A=va`). DEFERRED: ViRGE
silicon still hardwires MODULATE — native REPLACE/DECAL (TB decal blend) is
Stage 3/Phase 8 C5, gated on equation-level equivalence, plus the
mode-comparison demo. GL_BLEND/GL_ADD env stay out of scope (Phase 8 C6).

## Stage 2 — GLQuake runs correctly on swrast

### Q7. Lightmap formats and the multiply pass

Verify end-to-end that the world lightmap path works on swrast:
`-lm_4`-style `GL_RGBA` lightmaps upload through the existing path, and
the `glBlendFunc(GL_ZERO, GL_SRC_COLOR)` multiply pass produces a lit
world. Add `GL_LUMINANCE` (and, if Q0 confirms use, `GL_ALPHA`/
`GL_INTENSITY`) as an accepted `glTexImage2D` format/internal format with
1-byte unpack expanded to ARGB8888, so GLQuake's *default* configuration
(`gl_lightmap_format` luminance with `GL_ZERO, GL_ONE_MINUS_SRC_COLOR`)
also works rather than requiring a command-line switch.

*Acceptance:* a synthetic two-pass test (checker texture × gradient
lightmap) matches an analytically computed image on swrast for both the
RGBA and luminance formats; dynamic light updates through Q4 visibly
modulate the result.

*Status (Q7, swrast gate):* DONE — `glTexImage2D`/`glTexSubImage2D` accept
`GL_LUMINANCE`/`GL_ALPHA`/`GL_INTENSITY` (Q0 lists all three as
GLQuake-referenced tokens) as both format and internal format; the one
source byte is expanded to ARGB8888 in the shim before the backend sees it
(LUMINANCE → `(L,L,L,255)`, ALPHA → `(0,0,0,A)`, INTENSITY → `(I,I,I,I)`),
so every backend samples lightmaps natively. The multiply blends were
already wired (`GL_ZERO`/`GL_SRC_COLOR` and `GL_ZERO`/`GL_ONE_MINUS_SRC_COLOR`
map through from Phase 4); `test_lightmap_formats` pins the byte→word
expansion and error behavior at the GL layer, and `test_lightmap_multiply`
renders a real two-pass checker×gradient frame on swrast and pins it
against analytic values for the RGBA multiply (`fb = base·lm`), the
luminance multiply (`fb = base·(1−lm)`), and a `glTexSubImage2D` dynamic
lightmap update that visibly modulates the lit world. Q12 adds a fourth
case for GLQuake's actual black-RGB/darkness-alpha layout, numeric internal
format 4/ARGB4444, and source-alpha blending (`fb = base·(1−alpha)`). ViRGE
cannot perform the general `GL_ZERO`/`GL_SRC_COLOR` blend, but this fourth
identity is exact for Quake's grayscale lightmaps and is now verified on
silicon. `GL_RGBA4` 16-bit lightmap storage remains part of Q10.

### Q8. Texture lifetime and delete semantics

Make `glDeleteTextures` actually release storage so per-level texture
churn survives:

- **shim:** free the retained CPU copy (Q4) with the name.
- **swrast:** free the backend allocation on delete (it owns a private
  allocation list; add removal).
- **ViRGE:** replace the bump allocator's leak-until-teardown behavior
  with a free-list allocator over the texture heap region (first-fit with
  coalescing is sufficient at this scale), preserving the existing
  alignment rules. Deleting must make VRAM reusable; fragmentation limits
  are acceptable and documented.

*Acceptance:* a create/delete/create stress test at swrast and (human
sign-off) ViRGE shows stable memory across simulated level reloads; the
existing bump-allocation demos still pass; OOM still reports cleanly.

*Status (Q8, swrast gate):* DONE (shim + swrast) — the retained CPU copy was
already freed with the name; `glDeleteTextures` now also calls a new optional
backend `tex_free` hook (`l10gl_tex_free`), and swrast implements it by
unlinking the texture from its private allocation list and freeing the texel
store. `swrast_tex_image_2d` additionally frees the prior allocation on
re-upload, so Q4's whole-level subimage re-uploads (one per dynamic light
update per frame) no longer leak one dead image per call. `test_texture_lifetime`
drives the GL delete path on swrast and pins, via `swrast_debug_texture_count`,
that create/delete/create cycles, repeated re-uploads, and a `glTexSubImage2D`
storm all hold a stable allocation count (level-reload memory stability).
The ViRGE Stage 3 portion landed with Q10: its texture heap is now a first-fit
free-list with eight-byte allocation rounding and adjacent-block coalescing.
Re-uploading a name releases its old allocation before replacement, and
`tex_free` returns deleted names to the heap. The pure allocator test pins
first-fit reuse, capacity recovery, overlap rejection, and two-sided
coalescing. PENDING — human create/delete/reload sign-off on silicon.

### Q9. The GLQuake port and the swrast "up and running" gate

In the separate GPL repository: port GLQuake's platform layer to L10GL —
`vid_l10gl.c` using `l10glCreateContext`/`l10glSwapBuffers`/`glFinish`
(the Phase 4 `gears` substitution, plus honoring the actual geometry the
context reports), and `in_l10gl.c` reading raw keyboard from the owning VT
and mouse from evdev, following the console-ownership discipline
`src/console.c` established (the game must never fight P2 for the VT).
Build with `-nosound` first. Document the exact build/run steps in that
repository and link them from here.

This repository gains the automated gate: a scripted run (fetch shareware
data, build the port, run `L10GL_BACKEND=swrast` offscreen with
`L10GL_FRAMES`-bounded capture) that starts the game, plays
`timedemo demo1` to completion, and dumps frames.

*Acceptance — the Phase 7 headline milestone:* `timedemo demo1` completes
on swrast without GL errors on the console; captured frames show a
textured, lightmapped world, sky and water, alias models, particles, and
readable HUD/console text; the reported timedemo frame total matches the
demo's canonical frame count.

*Status (Q9, started 2026-07-28, automated gate implemented 2026-08-02):*
`vid_l10gl.c` and `in_l10gl.c` exist in `L10GL-Quake` (`WinQuake/`) and
`glquake.l10gl` **builds and links clean** against `libl10gl.a` on x86-64
via `WinQuake/Makefile.l10gl` (no asm dependency -- see that repo's
`L10GL_PORT.md` for why the four legacy `.s` files aren't needed on this
architecture). With real shareware data in place (`quake106.zip` from an
`ftp.idsoftware.com` mirror; its `resource.1` payload is a DOS
self-extracting LHA archive, extracted with `lha`/`lhasa` -- see
`L10GL_PORT.md` for the exact commands), **`VID_Init` runs end to end on
swrast**: `l10glCreateContext` creates a real double-buffered offscreen
context, `GL_Init`'s full fixed-function setup completes cleanly
(`GL_VENDOR`/`GL_RENDERER`/`GL_VERSION` print correctly, `GL_EXTENSIONS` is
honestly empty per Q1), and `L10GL_SWRAST_DUMP` frame captures of
`demo1.dem` playback (verified visually) show correctly textured 3D
geometry, monsters, the first-person weapon model, and a fully rendered
HUD/pickup-message overlay. `in_l10gl.c` degrades cleanly with no real
VT/evdev present (this class of sandbox) rather than crashing, and
`signal_handler`'s Ctrl-C/SIGTERM path was exercised successfully.
`tools/quake-swrast-gate` now supplies the required repeatable integration
run: it invokes the port's verified shareware fetcher when necessary, builds
the port, stages only a symlink to its untracked pak data, runs
`-nosound -noudp +timedemo demo1` at 320x240x16 with swrast PPM capture, and
requires Quake's canonical **969-frame** result before SIGTERM cleanly ends
its post-timedemo demo loop. Its no-data/no-network fixture is in
`tests/test-quake-swrast-gate.sh` and runs under `make check`. **Q9 swrast
acceptance: DONE (2026-08-02).** The real port/data run initialized
`L10GL/swrast` at 320x240x16, completed `demo1` at the canonical 969 frames
in 8.0 seconds (121.0 FPS), and retained 1,073 PPM presentations, including
loading frames before the timedemo measurement. The independent live-VT
keyboard/mouse exercise of `in_l10gl.c` remains a follow-up, but does not
block the swrast headline milestone. Stage 3 is now unblocked.

## Stage 3 — GLQuake on the ViRGE

Stage 3 begins only after the Q9 gate is green. Every item here follows
the established hardware rules: one behavioral change per commit,
`tools/l10gl-run`, human sign-off, full console recovery.

### Q10. VRAM budget: 16-bit texture uploads and level-fit policy

At 640×480 RGB555 double-buffered with Z, roughly 2.2MB of the 4MB card
remains for textures — before Q3's replication overhead. Add
ARGB1555/ARGB4444 upload paths in the shim (the backend formats already
exist in `l10gl_virge.c`; the shim currently converts everything to
ARGB8888) selected by internal format, halving texture VRAM. Publish a
sizing note: recommended `gl_max_size 256`, expected budget per shareware
level, and what OOM looks like when a level exceeds it (clean error, not
corruption). Q8's allocator makes level transitions survivable.

*Acceptance:* format-conversion tests pin 1555/4444 packing; a VRAM
accounting test walks a recorded shareware level's texture set and proves
it fits the budget; human verifies texture quality on hardware.

*Status (Q10, started 2026-08-02):* the shim now treats GLQuake's legacy
component-count internal formats as the compact upload policy: `3` uploads
ARGB1555, `4` uploads ARGB4444, and explicit `GL_RGBA4` also uploads
ARGB4444. It deliberately retains Q4's CPU-side ARGB8888 image and packs a
temporary 16-bit backend upload, so subimage/lightmap updates are lossless
before quantization. `test-gl` pins the exact packed words, including the
packed `GL_RGBA4` source shape used by GLQuake's `-lm_2` option. The Q9 swrast
gate records every successful base-level upload in `textures.tsv`; run
`tools/quake-vram-budget --trace <that file>` to apply the exact 640x480
RGB555 double-buffer + Z layout, ViRGE rectangle replication, and eight-byte
allocation alignment.

**First real budget result (2026-08-02): NOT FIT.** The recorded `demo1`
(E1M3) sequence consumed 8,934,528 bytes (8,725 KiB) while the 4 MiB target
has only 2,351,104 bytes (2,296 KiB) after color and depth buffers — an
overrun of 6,583,424 bytes. The trace contains 153 successful base-level
uploads, including 24 256x256 and 12 256x128 ARGB1555 allocations after
ViRGE's square replication. The gate passed `+gl_max_size 256`, but that
console command is processed too late to constrain all bootstrap uploads;
the trace is the proof. Do not begin Q11. Next Q10 work is to make the 256
cap effective before GL initialization (or establish a safe residency/eviction
policy), re-record the trace, and then publish the level-fit/OOM result.

**Debugging resolution (2026-08-02): FIT.** The lifetime-aware trace added GL
names, deletes, and subimage replacement events. It proved the first result
was not mainly re-upload leakage: 188 events collapsed to 151 genuinely live
objects, still using 8,672,384 bytes. Asset labels then identified rounded-up
alias-model skins as the dominant allocations; fixed 128x128 lightmap atlases
were only 416 KiB. The private port now sets `gl_max_size 256` and
`gl_picmip 2` as startup defaults before `Draw_Init`, rather than relying on
late `+` commands. `gl_picmip` is deliberately limited to mipmapped
world/model assets: applying it globally fit with more margin but reduced 8x8
charset glyphs to unreadable 2x2 blocks. The visually inspected final swrast
rerun kept the fixed UI atlases readable, completed canonical 969-frame
`demo1`, and peaked at **2,196,416 bytes (2,144 KiB), leaving 154,688 bytes**
in the 2,351,104-byte synchronized texture heap. The ViRGE free-list described
in Q8 now makes the 37 recorded replacement uploads reuse storage rather than
burn the remaining margin. Automated format, allocator, trace-lifetime,
first-fit fragmentation, and real level-fit gates are green. PENDING — human
texture-quality sign-off on ViRGE; Q11 remains hardware-gated until that
check.

### Q11. First hardware run: fullbright world

Run the ported GLQuake on the ViRGE/DX with lightmaps disabled
(`r_fullbright 1`, dynamic lighting off) — geometry, textures, alpha-test
approximation (Q5), sky, water, models, HUD, at 640×480@60 via the proven
P6 native modeset. This isolates rasterization and texture correctness
from the lightmap-blending question.

*Acceptance:* the shareware start map renders recognizably and navigably;
no register-level hangs across a full `timedemo demo1`; Ctrl-C and normal
exit both restore the console; FPS recorded as the hardware baseline.

*First silicon attempt (2026-08-03): DIAGNOSTIC REQUIRED.* The ViRGE/DX
(device 8a01) was selected correctly, detected 4 MiB, applied and read back
the proven 640x480@60 RGB555 native image, initialized the synchronized
double-buffer/Z layout, and completed GL plus E1M3 model loading without a
texture OOM. Execution then stopped before the first timedemo result, after
the final visible model-mesh messages. The driver still had unbounded
`virge_wait_engine` and `virge_wait_fifo` spin loops, so a wedged loading-frame
command produced no location or status report and could also trap cleanup.
Those polls are now bounded at one second: timeout output names the calling
function, dumps SUBSYS_STATUS/FIFO and active framebuffer/texture state, then
uses the documented S3d reset/enable sequence and invalidates every register
cache. The rerun must capture that first timeout line before changing any
render state; it distinguishes upload, FIFO submission, page-flip, and cleanup
stalls.

*Bounded-wait rerun (2026-08-03): software frustum clipping pending hardware
gate.* The run reached the demo and displayed an untextured weapon over a
garbled frame. The first failure was a textured-triangle state update:
`emit_cached_state` needed 14 FIFO slots while the busy engine remained at
eight free slots for one second. Repeated recovery reached the same state with
multiple valid texture addresses and sizes, excluding texture allocation and
one corrupt asset. The common pipeline was only clipping the near plane and
delegated X/Y to the ViRGE hardware clip rectangle, but ViRGE commands keep HC
disabled because that silicon path is unusable. Full six-plane homogeneous
triangle clipping is now implemented and capture-tested so off-screen GLQuake
world polygons cannot wrap ViRGE geometry fields or framebuffer addresses.
The next silicon run must verify that the first demo frame renders without a
FIFO timeout before Q11 can advance.

*Full-frustum rerun (2026-08-03): geometry/hang fixed; rectangular texture
correction pending.* The complete timedemo ran without the prior FIFO wedge,
and geometry rendered correctly. Weapon textures and some enemy skins were
reported correct, while world textures and other enemy skins were corrupt.
The backend replicated rectangular uploads into their required square ViRGE
allocation correctly, but then multiplied both normalized U and V by that
square side. For a 128x32 image, V=0..1 incorrectly traversed all four stored
copies instead of one 32-row source image. The draw path now scales U by the
original width and V by the original height, retaining 2^s fallback behavior
for raw diagnostics. The next run is the rectangular-texture hardware gate.

*Texture isolation reruns (2026-08-03): coordinate range pending hardware
gate.* Per-axis rectangle scaling left the corruption about the same. Forcing
`GL_NEAREST` also left it visually unchanged, while increasing frame rate, so
the unverified bilinear path is not the corruption source. Raising
`gl_picmip` from 2 to 3 changed the result only slightly and did not remove the
failure, excluding absolute upload size as the primary cause. A debugger trace
of the same shareware demo through swrast then captured the first large world
triangle at UVs `(-3.875,-4)`, `(-7.375,-4)`, and `(-7.375,-1.375)`; existing
ViRGE texture probes cover small positive coordinates almost exclusively. The
ViRGE GL path now subtracts one common integer period per triangle and axis
when `GL_REPEAT` is active. This is mathematically exact—fractional positions
and all gradients are unchanged—but moves negative/large coordinates into the
silicon-proven range. Raw diagnostics retain their unreduced inputs. The next
run is the repeat-coordinate hardware gate.

*Repeat-coordinate rerun (2026-08-03): texture data recognizable; perspective
precision pending hardware gate.* Rebasing made the textures recognizable,
confirming that texture upload, binding, dimensions, and repeat selection are
substantially coherent. Severe texture swimming still obscured final visual
sign-off. A debugger trace of actual world draws measured reciprocal W values
of approximately `0.0022` to `0.0040`, far below the `0.06` to `0.25` range
used by the successful ViRGE perspective probes. In S12.19 that throws away
most useful W-gradient precision. The L10GL ViRGE path now multiplies all
three W values in a triangle by one common power of two, bringing the largest
to `[1/8,1/4)` before forming U*W and V*W. Since numerator and denominator all
receive the same scale, the perspective result is mathematically unchanged;
only fixed-point precision improves. Raw diagnostics remain opt-out. The next
run is the W-precision hardware gate.

*First W-precision rerun (2026-08-03): substantially reduced swimming; target
precision refinement pending.* Raising world W into `[1/8,1/4)` substantially
reduced the visible texture movement, confirming fixed-point perspective
quantization as the remaining cause. It did not eliminate the movement. The
older silicon discriminator swept constant W through 128, and the production
divide/ufrac combination was matched exactly at W=1. The normalizer therefore
now targets `[1,2)` instead, recovering three additional effective fractional
bits while remaining far inside the proven S12.19 and divider ranges. The
common scale remains an exact power of two, and raw probes remain unchanged.
The next run is the refined W-precision hardware gate.

*Refined W-precision rerun (2026-08-03): PASSED.* With the per-triangle maximum
normalized into `[1,2)`, the full timedemo completed with no texture swimming.
This hardware-verifies perspective U/V/W interpolation for Quake's depth range
and closes the repeat-coordinate and W-precision gates. The run used
`GL_NEAREST`; Q11's remaining visual check is the normal `GL_LINEAR` path with
the now-stable coordinates, followed by texture-correctness sign-off.

*Linear-filter visual follow-up (2026-08-03): alias darkness is reference
behavior.* The normal `GL_LINEAR` run remained stable, but distant enemies were
reported black. This is not controlled by `r_fullbright`: original GLQuake
uses that cvar for world surfaces, while `R_DrawAliasModel` always obtains
enemy lighting from `R_LightPoint`, emits it with `glColor3f`, and selects
`GL_MODULATE`. An L10GL swrast capture with the exact fullbright/linear timedemo
settings reproduces the near-black distant monster. This closes the concern as
map/alias lighting rather than ViRGE texture corruption.

### Q12. ViRGE lightmap strategy

The general color-factor multiply blend does not exist in ViRGE silicon —
blending is fixed-function source-alpha only (`PLAN.md` capability table;
DB019-B §15.4.8.5). Q12 began with a spike per option in this order of
preference; the decision record below includes the exact source-alpha identity
discovered during hardware validation:

1. **CPU lightmap compositing** (the vQuake approach): pre-multiply
   lightmaps into the affected surface textures on the CPU and upload the
   composite — exact output, costs VRAM (a surface cache) and upload
   bandwidth on dynamic light changes; meshes well with Q4's retained CPU
   copies.
2. **Vertex lighting approximation:** sample the lightmap at polygon
   vertices into the Gouraud path — cheap, era-authentic look, visibly
   coarser.
3. **Defer to Phase 8 C10** hybrid software fallback — exact but far off;
   only if 1 and 2 both fail on quality or budget.

The decision, measurements, and rejected options get recorded here the
way Phase 6 recorded the autoexecute and triangle-reuse rejections.

*Acceptance:* the chosen path renders a lit E1M1 on hardware, visually
compared against the swrast reference; dynamic lights (rockets, muzzle
flashes) either work or are cleanly disabled by default with the cvar
documented.

*Implementation and decision (2026-08-04): RGBA alpha-darkening selected and
hardware-verified.* The CPU-compositing spike counted 5,139 lightmapped
surfaces in demo1/E1M3. At the shipping `gl_picmip 2`, rectangles including one
guard texel per edge total 2,743,451 texels, or 5,486,902 RGB555 bytes before
atlas packing. This exceeds the complete 4 MiB card, not merely the 2,351,104
bytes left after synchronized 640x480 color/depth buffers. `gl_picmip 3` still
requires an ideal 1,744,702 bytes before packing, while the already accepted
non-lightmap asset set consumes most of the texture heap. A bounded LRU cache
does not rescue the design with the Q4 upload contract: each cache miss would
make `glTexSubImage2D` re-upload a complete 256x256 atlas. CPU compositing is
therefore rejected for both capacity and upload-bandwidth reasons.

The GPL GLQuake port first implemented option 2. It samples the CPU lightmap
bilinearly at every brush-polygon vertex, emits the grayscale sample through
`GL_MODULATE`, and lets the ViRGE Gouraud unit fill the polygon interior. A
forced 640x480 swrast timedemo completed all 969 frames in 25.9 seconds (37.4
FPS). The real ViRGE/DX then completed the full timedemo at 7.7 FPS, proving
the mechanism and dynamic CPU-lightmap updates, but the user reported the
lighting as "very triangular." It fails the quality comparison and remains
only an optional diagnostic fallback.

The decisive follow-up found a fourth, exact option in GLQuake's existing
RGBA lightmap encoding. `R_BuildLightMap` writes black RGB and darkness to
alpha. With `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA`, ViRGE's supported fixed
blend evaluates `black*A + framebuffer*(1-A)`, exactly reproducing grayscale
lightmap multiplication. Numeric internal format 4 becomes ARGB4444 in L10GL,
so the hardware consumes texture alpha without new backend features. Dynamic
lights and animated lightstyles keep the ordinary atlas rebuild and
`glTexSubImage2D` path under `r_dynamic 1`.

`gl_virge_lightmaps 1` is the default and selects exact RGBA alpha lightmaps
only on `L10GL/virge`; value 0 restores original GLQuake format selection,
value 2 forces the coarse vertex fallback on any renderer, and value 3 forces
the exact mode for swrast/reference testing. A forced 320x240 swrast run
completed the canonical 969 frames in 13.0 seconds (74.8 FPS). On the real
ViRGE/DX, the full 640x480@60 discriminator run completed at 4.2 FPS and the
user reported that lighting appears correct. This passes Q12's visual and
hardware gates. The performance is acceptable under Q13's explicit
single-digit, non-blocking criterion. After the automatic-default integration
was pushed, a no-`-lm_4` follow-up printed `Q12: ViRGE RGBA alpha lightmaps
active` on the target machine. This confirms that renderer detection selects
the exact path without a command-line format override and fully closes Q12.

### Q13. Phase acceptance: playable Quake on the ViRGE

Close Phase 7 with the end-to-end gate on the target machine.

*Acceptance:* from a fresh boot, `l10gl-run`-launched GLQuake at
640×480@60 plays the shareware E1M1 start-to-exit with lighting per Q12,
level transition succeeds (Q8/Q10), exit and Ctrl-C restore the console,
and `timedemo demo1` FPS is recorded in this document. Playability, not
frame rate, is the bar — this chip was never fast at GLQuake and single-digit
FPS does not block acceptance. Performance follow-ups go to the Phase 6
methodology with before/after numbers.

*Status (Q13, started 2026-08-04): first hardware failure fixed; fresh-boot
rerun pending.* `tools/quake-virge-gate` stages the shareware data outside the
GPL/MIT boundary and drives the complete acceptance sequence through
`l10gl-run`. It forces the accepted 640×480@60 native, synchronized ViRGE
configuration. The first run starts E1M1 with developer map markers enabled,
requires the operator to play through its exit into E1M2, and then exercises
normal `quit`. The second runs canonical `timedemo demo1` and exercises the
port's raw-keyboard Ctrl-C signal path. The runner rejects a wrong renderer,
mode, or Q12 lightmap selection, a missing E1M1-to-E1M2 transition, a non-969
frame result, and any ViRGE engine timeout or texture OOM. It writes both logs
and an operator-attested report containing the exact FPS and repository
commits. A no-hardware two-run lifecycle fixture is part of `make check`.

The first target attempt on 2026-08-05 reached E1M1 server spawn, then raised
signal 11. A swrast reproduction and GDB first-fault trace found a native
x86-64 port bug: the original engine encoded pointers to C-owned strings as
signed 32-bit offsets from `pr_strings`, and the world-model offset truncated
when the regions landed more than 2 GiB apart. L10GL-Quake commit `89892bf`
keeps engine-published strings in the server hunk and the local-server
regression now enters E1M1. The target log also showed keyboard setup failing
with `KDGKBMODE`/`ENOTTY`. Although the invoking shell was `/dev/tty1`, sudo's
default `use_pty` policy moved the privileged process to `/dev/pts/1`.
L10GL-Quake commit `26cad5b` adds explicit physical-VT keyboard input. The
acceptance runner now validates sudo's original `SUDO_TTY`, passes that device
as `L10GL_KBD_DEV`, and requires its successful initialization while still
rejecting an SSH PTY before framebuffer detachment. Q13 remains open until
that direct-VT, fresh-boot rerun passes and its measured FPS is copied here.

## Execution order

```text
Q0 -> Q1 -> Q2 ------------------+
       |                         |
       +--> Q3 -> Q4 -> Q7 ------+--> Q9 (swrast gate)
       |          |              |
       +--> Q5 ---+---> Q8 ------+
       +--> Q6 ------------------+

Q9 -> Q10 -> Q11 -> Q12 -> Q13   (hardware stage, strictly after Q9)
```

Q1 through Q6 are independently committable against `make check`; Q9 is
the integration point and the "up and running" milestone the phase is
named for. Nothing in Stage 3 starts before Q9 passes.

## Relationship to Phase 8

Every Q-item is a scoped-down slice of a Phase 8 C-item and must be built
so Phase 8 extends rather than replaces it: Q1 feeds C1 (queries/ABI), Q2
feeds C2 (primitives), Q3/Q4/Q7/Q8/Q10 feed C5 (complete texture
behavior), Q5/Q6 feed C6 (per-fragment state), and Q12's findings feed
C10 (hybrid fallback). The honest-version rule is unchanged throughout:
`glGetString(GL_VERSION)` keeps reporting the pre-1.1 tier until Phase 8's
gates pass — running Quake does not make the driver OpenGL 1.1.
