# GLX implementation plan

Planned 2026-09-05, against L10GL `686cc7b`. This is Phase 9 of
`PLAN.md`. The user has expanded the project to include X11/GLX; earlier
project-wide exclusions of those interfaces are superseded. This document
is a plan, not an announcement of implemented GLX support.

## Target and implementation decision

Deliver **GLX 1.2 with L10GL software rendering**, then investigate native
ViRGE acceleration under X. GLX 1.2 is the first complete target because it
matches Phase 8's OpenGL 1.1 goal. GLX 1.3/1.4 introduce a larger version
contract, including support through OpenGL 1.2/1.3 respectively; implementing
their context functions alone would not satisfy that contract. See the
[GLX specification, chapter 6](https://registry.khronos.org/OpenGL/specs/gl/glx1.4.pdf).

The preferred complete implementation is an **L10GL provider for Xorg's
indirect GLX path**, using its existing request decoding and X resource
management. Applications use an existing GLX client library and render through
L10GL inside a dedicated test X server. The first supported mode is indirect;
direct rendering is optional, whereas indirect rendering is part of the GLX
contract. A client-side `XPutImage` prototype is a useful early milestone,
but is not a complete GLX implementation. See GLX sections 2.1–2.3.

Do not start by replacing the desktop's system `libGL.so`, writing a new GLX
wire decoder, or loading the fullscreen ViRGE backend into an X client.
Retain the console library and demos as a supported build with no X dependency.
X11 software rendering must explicitly select offscreen storage, irrespective
of `L10GL_BACKEND`, `L10GL_SWRAST_FB`, and native-modeset environment settings.

## What the current code requires

| Current code | Required change |
|---|---|
| `src/l10gl_gl.c`: one global `gl_state` | Separate context state, thread-current binding, and shared object storage. |
| `l10glMakeCurrent` releases textures and resets compatibility state | Binding must preserve state; destruction must be explicit. |
| `l10gl_ctx` combines state, target geometry, and backend ownership | Introduce drawable storage that can be rebound without recreating a GL context. |
| `swrast_private` owns color/depth buffers together with textures | Move drawable buffers out of context-owned renderer state; support shared texture lifetime. |
| swrast's color buffers are private; offscreen accepts only 16/24-bit storage | Add an internal surface/presentation interface and tested X visual conversion. |
| `glFlush` is effectively empty; `glReadPixels` is a compatibility stub | Implement real front-buffer visibility, reads, and X/GL synchronization. |
| GL state queries, display lists, and raster commands are incomplete | Reuse Phase 8 C1/C7/C8; do not conceal missing core behavior behind GLX. |
| ViRGE init assumes ownership of PCI MMIO, VRAM, scanout, and the console | An X-owned acceleration path needs a different ownership and submission contract. |

An X visual's depth is not its storage byte count: depth 24 often uses a
32-bit XImage. Test pixel masks, byte order, row padding, and drawable depth
independently. Keep storage conversion outside the triangle rasterizer.

## GX0. Pin the integration boundary and compatibility manifest

Before a broad refactor, run a bounded feasibility spike against one pinned
Xorg server revision. Trace its provider creation, GL dispatch-table setup,
context switching, drawable callbacks, and version/visual publication. Prove
how a provider can call L10GL without accidentally resolving GL symbols to
the host renderer. Inventory libglapi/DRI-loader coupling and record every
build dependency and upstream license notice.

The server has provider interfaces, but its existing software provider uses
DRI interfaces; it is not evidence of a stable, independent L10GL plugin ABI.
The deliverable is a reproducible server build and a small provider prototype
that reaches an L10GL clear operation. Prefer a narrow server patch/provider
over a new server. If GL dispatch cannot be separated without a substantial
loader dependency, document that concrete tradeoff before committing to the
rest of the server implementation. Reusing protocol/dispatch glue does not
make Mesa the renderer, and does not require adding kernel DRM support.
Sources: [Xorg screen interface](https://raw.githubusercontent.com/XQuartz/xorg-server/master/glx/glxscreens.h)
and [software provider](https://raw.githubusercontent.com/XQuartz/xorg-server/master/glx/glxdriswrast.c).

Create a manifest of all **21 core GLX 1.0–1.2 entry points**, including which
parts are supplied by the system client, the X server, and L10GL. Track
queries, errors, visual attributes, protocol requests, and observable behavior
alongside symbols. Derive the inventory from the
[Khronos GLX registry](https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/xml/glx.xml).
Full function-pointer lookup and GLVND vendor integration are later work,
not reasons to grow the first provider.

**Acceptance:** one pinned integration recipe, a provider/dispatch ownership
diagram, an executable clear probe, and a manifest whose missing behaviors
are explicit. This spike may run before Phase 8 is complete; it is not a
release gate.

## GX1. Separate context state and shared objects

Extract `l10gl_gl_state` into an allocated object owned by a context. Keep a
thread-local current-context pointer and separate the legacy owned-console
context from that binding. Initialize GL defaults once at creation. Rebinding
the same context must not reset matrices, errors, enables, or textures.
Failed binding must leave the previous binding usable.

Introduce reference-counted share groups for named textures and, when C8
lands, display lists. Texture zero stays context-local. Separate a shared
texture's lifetime from an individual context's backend allocation list;
deleting an object must not invalidate references retained by another context.
Use the same core ownership functions for console and GLX paths.

Coordinate thread-current and destruction rules with the existing GLX client
and server machinery. Reject a context made current simultaneously in two
threads; retain a destroyed-but-current context until it is released. Do not
make changing contexts call the current destructive `l10glMakeCurrent` helper.
Sources: [context binding](https://xorg.freedesktop.org/archive/X11R6.8.0/doc/glXMakeCurrent.3.html),
[sharing](https://xorg.freedesktop.org/archive/X11R6.8.0/doc/glXCreateContext.3.html),
and [destruction](https://www.x.org/archive/X11R7.0/doc/html/glXDestroyContext.3.html).

**Acceptance:** alternate two contexts with distinct state and textures;
exercise shared/unshared objects, deletion order, failed rebind, thread handoff,
and destruction while current. Existing console and GLQuake tests pass.

## GX2. Make render targets independent of contexts

Add a small internal drawable record containing actual dimensions, format,
stride, front/back/depth storage, and lifetime. A context binds a drawable;
two compatible contexts using one drawable see the same buffers. Textures
belong to their share group, not the window. Keep the existing backend vtable
where possible and extract only the ownership boundaries that are required.

Provide swrast helpers for creating, resizing, binding, and accessing completed
surfaces. Resizing allocates replacement storage before replacing the old
storage and preserves context state. First binding initializes the viewport
according to the selected API contract; later resize must not silently reset
the application's viewport or matrices. All size arithmetic must be checked.

Start with monoscopic RGBA rendering and truthful TrueColor configurations.
Support single and double buffering, RGB storage with no advertised alpha
where appropriate, and explicitly defined depth precision. Do not advertise
stencil, accumulation, stereo, or indexed-color configurations until their
storage and semantics work.

**Acceptance:** one context renders to two differently sized surfaces; two
contexts share a drawable's depth/color buffers; repeated resize and allocation
failure preserve valid state. Pixel dumps match the existing offscreen oracle.

## GX3. Prove X11 presentation without claiming GLX completion

Add an optional Xlib demo/harness using the surface interface. Start with
`XImage`/`XPutImage`; add MIT-SHM only after measured upload cost justifies it.
Convert to the selected visual's masks and storage layout, respecting XImage
ownership during cleanup. X retains window clipping and desktop ownership.
Use Xlib inside the client harness, not recursively inside the eventual Xorg
server provider. [Xlib image documentation](https://xorg.freedesktop.org/archive/current/doc/libX11/libX11/libX11.html)
defines image layout and drawable-depth matching.

The application owns its X connection, event loop, input, and windows. The
adapter must not consume arbitrary events from that queue. Handle resizing,
unmapped windows, window destruction, and Expose-driven redraw without console
operations. Maintain drawable storage while temporarily hidden. Software image
upload does not promise vblank synchronization or tear-free desktop scanout.

**Acceptance:** existing gears/texture geometry runs in an ordinary resizable
window without root. Capture and compare window pixels with swrast. Exercise
obscure/reveal, minimize/restore, and close. Use Xvfb for local automation and
an ordinary X desktop for presentation checks; Xvfb is not currently installed
in the development environment.

## GX4. Connect the indirect GLX provider

Turn the GX0 prototype into an optional Xorg provider using GX1/GX2 storage.
Reuse the server's authenticated connections, XIDs, request validation,
byte-swapping, Render/RenderLarge decoding, and X error routing. Connect its
GL dispatch table to the L10GL implementation through a defined symbol boundary.
Supply context creation/destruction, binding, drawable creation/destruction,
visual enumeration, and flush/finish/swap hooks.

Use the server's drawable/GC interfaces for presentation, including clip lists
and redirected pixmaps; avoid an uncoordinated private copy of screen pixels.
Keep each client's resources isolated except for explicitly shared objects.
Disconnects and malformed requests must release resources without terminating
the server or exposing another client's buffers.

The client API groups to validate through the real server are:

- Discovery: `glXQueryExtension`, `glXQueryVersion`, `glXGetClientString`,
  `glXQueryServerString`, `glXQueryExtensionsString`.
- Visuals: `glXChooseVisual`, `glXGetConfig`.
- Contexts: `glXCreateContext`, `glXDestroyContext`, `glXMakeCurrent`,
  `glXCopyContext`, `glXIsDirect`, `glXGetCurrentContext`,
  `glXGetCurrentDrawable`, `glXGetCurrentDisplay`.
- Drawables/presentation: `glXCreateGLXPixmap`, `glXDestroyGLXPixmap`,
  `glXSwapBuffers`, `glXWaitGL`, `glXWaitX`, `glXUseXFont`.

Report the actual renderer and indirect status. Query results must describe
the implemented client/server contract, not merely repeat the host desktop's
version or allocate fictional extension/error numbers. Handle a request
preferring direct rendering by returning an allowed indirect context with
truthful `glXIsDirect`, rather than starting native console takeover.

**Acceptance:** a client linked against a stock GLX library creates a window
on the dedicated L10GL test server and renders through L10GL. Establish this
with a renderer marker plus a call/capture trace, not just a successful image
that the host Mesa renderer might have produced.

## GX5. Complete GLX 1.2 behavior and its GL dependencies

Implement the remaining manifest rows rather than publishing success stubs.
This includes context copying, shared lists/textures, legacy GLX pixmaps,
front/back buffer selection, real pixel reads, and correct GL versus X command
ordering. Pixmap front storage must be visible to X clients, including X reads
after the required synchronization. Tests must cover X writes followed by GL
operations too; a one-way image uploader cannot satisfy those cases.

Honor single-buffer flush/finish visibility and implicit flushing associated
with buffer swaps. Implement the legacy pixmap swap behavior separately from
window swaps. Sources: [swap semantics](https://www.x.org/archive/X11R7.0/doc/html/glXSwapBuffers.3.html)
and [legacy pixmaps](https://xorg.freedesktop.org/archive/X11R6.8.0/doc/glXCreateGLXPixmap.3.html).

`glXUseXFont` requires bitmap glyph display lists, tying it directly to C7/C8;
missing fonts/lists are not safely replaced with a silent no-op.
[X font binding reference](https://www.x.org/archive/X11R7.0/doc/html/glXUseXFont.3.html).
Use Phase 8 C1's state model for context-copy masks and error/query behavior.
Preserve the distinction between GL errors and X/GLX asynchronous errors.

**Acceptance:** the 21-entry manifest is complete; every error, lifetime,
visual-selection, synchronization, pixmap, and sharing case has an integration
test. Phase 8's software OpenGL 1.1 acceptance passes before claiming complete
GLX 1.2 support. Audit server-wide version negotiation as well as provider
strings; an upstream server default must not overstate L10GL's accepted tier.

## GX6. Packaging and application acceptance

Keep `make`/`make check` independent of X development packages. Add explicit
X11 and server-provider targets using `pkg-config`, separate PIC objects where
needed, and a pinned test-server build recipe. Proposed artifacts are a small
`demos/x11_gears.c` proof, provider sources under `src/glx/`, tests under
`tests/glx/`, and `tools/glx-swrast-gate`; GX0 determines the exact server module
packaging. Do not copy system GL headers wholesale or install a replacement
system GL library as part of the test runner.

Run the integration server on a separate authenticated display, including the
explicit indirect-rendering enablement required by its pinned configuration.
An ordinary Xvfb with the host GLX provider is useful for GX3, but does not
validate the L10GL provider; GX4+ tests must start the instrumented server.

The final corpus includes a minimal stock-GLX triangle client, windowed gears
(after C8 if its source uses display lists), texture updates, two windows/two
contexts, shared objects, X/GL interleaving, fonts, pixmaps, and repeated client
disconnects. Exercise remote X transport and opposite-endian protocol decoding
where available. Record skipped platforms explicitly.

**Acceptance:** normal and sanitizer suites, window screenshots, request/error
tests, resource-leak checks, and console/Quake regression gates all pass. Save
exact L10GL/server/client revisions and renderer/version strings. This closes
the software GLX milestone; the machine needs no ViRGE for these tests.

## GX7. Native ViRGE windows under X

Begin only after software GLX works. The X server must be the sole owner of
scanout, mode changes, VRAM allocation, and engine arbitration. Investigate
server-side indirect acceleration first: the provider and X video driver
coordinate one engine rather than granting independent MMIO access to clients.
The existing native-console init/cleanup functions cannot be reused as-is.

Prove one server-owned offscreen RGB555 target, an explicit 2D/3D synchronization
boundary, and correct presentation into an X drawable. Address redirected
windows, changing clip regions, front/depth readback, RGB555-to-X-visual
conversion, and texture/buffer residency within the real 4 MiB card. The
documented unreliable CPU VRAM-read aperture is a concrete blocker for some
transfer strategies; use an engine-assisted path only after a dedicated probe.

If these ownership/transfer requirements cannot be met, keep software GLX
operational and record native X acceleration as blocked. Do not make software
GLX conditional on a successful hardware experiment. Direct hardware contexts,
DRI/DRM infrastructure, and a GLVND vendor library require separate design
work if later justified. GLVND supplies dispatch/vendor selection, not device
ownership or a renderer: [libglvnd architecture](https://github.com/NVIDIA/libglvnd/blob/master/README.md).

**Acceptance:** two independently moving/overlapping windows, resizing,
compositor redirection where supported, client crash, and VT switching do not
corrupt the desktop. Hardware runs follow the batched operator protocol in
`docs/HANDOFF.md`; no new native register path is accepted through software
tests alone.

## GX8. Later GLX 1.3/1.4 expansion

After the first release, separately scope FBConfigs, GLXWindow wrappers,
pbuffers, separate read/draw bindings, drawable events, context queries, and
core procedure lookup. Include the associated OpenGL version and protocol
requirements in that expansion. Swap-control extensions, modern context
creation, EGL, Wayland, and desktop compositing features are not implicit in
the GLX 1.2 milestone.

## Order and next implementation task

```text
GX0 -> GX1 -> GX2 -> GX3 -> GX4 -> GX5 -> GX6
                                     ^
Phase 8 C1/C7/C8 + software 1.1 gate --+
GX6 -> GX7 (native ViRGE investigation)
GX6 -> GX8 (separate version expansion)
```

GX0–GX4 can progress against the current supported GL subset, alongside the
relevant Phase 8 work. They do not close Q13 or bypass core-rendering acceptance.
The first implementation task is **GX0: pin and prove the Xorg provider and GL
dispatch boundary**. The largest uncertainties are that integration boundary
and eventual ViRGE ownership, not the number of `glX*` wrappers. Do not attach
a calendar estimate until the GX0 spike has measured those dependencies.
