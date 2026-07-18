/*
 * cube.c - Spinning Gouraud-shaded cube demo using L10GL.
 *
 * Hardware-agnostic — uses the L10GL frontend API. The backend is detected
 * at runtime or selected with L10GL_BACKEND=<name>.
 *
 * Build: make
 * Run:   sudo ./cube [width height bpp]
 *
 * Controls: Ctrl-C to exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>

#include "l10gl.h"

#define CAMERA_DIST 5.0f
#define FOV_Y_DEGREES 53.130102f
#define ANGLE_STEP_DEGREES 1.14591559f

/* -----------------------------------------------------------------------
 * Cube Geometry
 * -----------------------------------------------------------------------
 */

static const float cube_verts[8][3] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};

static const int cube_faces[12][3] = {
    /* Back   */ {0, 2, 1}, {0, 3, 2},
    /* Front  */ {4, 5, 6}, {4, 6, 7},
    /* Left   */ {0, 4, 7}, {0, 7, 3},
    /* Right  */ {1, 2, 6}, {1, 6, 5},
    /* Bottom */ {0, 1, 5}, {0, 5, 4},
    /* Top    */ {3, 7, 6}, {3, 6, 2},
};

static const float face_colors[6][3] = {
    {1.0, 0.0, 0.0},  /* Back:   red     */
    {0.0, 1.0, 0.0},  /* Front:  green   */
    {0.0, 0.0, 1.0},  /* Left:   blue    */
    {1.0, 1.0, 0.0},  /* Right:  yellow  */
    {1.0, 0.0, 1.0},  /* Bottom: magenta */
    {0.0, 1.0, 1.0},  /* Top:    cyan    */
};

static const float face_normals[6][3] = {
    { 0,  0, -1}, { 0,  0,  1},
    {-1,  0,  0}, { 1,  0,  0},
    { 0, -1,  0}, { 0,  1,  0},
};

/* -----------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------
 */

static volatile int running = 1;

static void sighandler(int sig)
{
    (void)sig;
    running = 0;
}

static int frame_limit_from_env(void)
{
    const char *value = getenv("L10GL_FRAMES");
    char *end;
    long limit;

    if (!value || !value[0])
        return 0;
    limit = strtol(value, &end, 10);
    if (*end || limit <= 0 || limit > INT_MAX) {
        fprintf(stderr, "Ignoring invalid L10GL_FRAMES='%s'\n", value);
        return 0;
    }
    return (int)limit;
}

int main(int argc, char **argv)
{
    int width = 640;
    int height = 480;
    int bpp = 2;  /* 16bpp */

    if (argc >= 3) {
        width = atoi(argv[1]);
        height = atoi(argv[2]);
    }
    if (argc >= 4) {
        bpp = atoi(argv[3]) / 8;
    }

    printf("L10GL Gouraud Cube Demo\n");
    printf("Initializing %dx%d @ %dbpp...\n", width, height, bpp * 8);

    /* Install before create: P2 may take VT ownership during initialization,
     * so SIGINT/SIGTERM must already be converted into orderly cleanup. */
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    struct l10gl_ctx ctx;
    if (l10gl_create_auto(&ctx, width, height, bpp) < 0) {
        fprintf(stderr, "Failed to initialize L10GL.\n");
        return 1;
    }
    printf("Selected backend: %s\n", ctx.backend->name);

    /* The backend may adopt the real screen mode instead of the request
     * (native scanout takeover on no-fbdev machines) -- render to what
     * we actually got or the projection/stride math is wrong. */
    if (ctx.width != width || ctx.height != height) {
        printf("Adopted actual screen %dx%d (requested %dx%d)\n",
               ctx.width, ctx.height, width, height);
        width = ctx.width;
        height = ctx.height;
    }

    /* L10GL_STATIC=1: render a single frame and idle, so it can be
     * photographed without tearing. Used to tell whether the animated
     * cube's "blacked out below ~2/5" is a render limit or vsync-less
     * single-buffer tearing (a static frame has no re-clear cycle). */
    int static_mode = getenv("L10GL_STATIC") != NULL;
    int frame_limit = frame_limit_from_env();

    /* Set clear values */
    l10gl_clear_color(&ctx, 0.0f, 0.0f, 0.0f);   /* black background */
    l10gl_clear_depth(&ctx, 1.0f);                /* far Z */
    /* Coverage is watertight at shared edges (seamtest: each boundary pixel
     * owned by exactly one triangle), so draw order and LESS vs LEQUAL do
     * not affect the cube -- plain LESS, fixed face order, no sort. */
    l10gl_depth_func(&ctx, L10GL_LESS);

    /* The original demo used scale = height / eye_depth. This field of view
     * gives the same screen projection. Reflecting Z converts its historical
     * +Z-into-screen convention to the pipeline's OpenGL -Z eye space. */
    l10gl_matrix_mode(&ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(&ctx);
    if (l10gl_perspective(&ctx, FOV_Y_DEGREES,
                          (float)width / (float)height, 3.0f, 7.0f) < 0) {
        fprintf(stderr, "Failed to configure projection.\n");
        l10gl_destroy(&ctx);
        return 1;
    }
    l10gl_cull_face(&ctx, L10GL_CULL_BACK);
    l10gl_enable_lighting(&ctx, 1);
    l10gl_light_color(&ctx, 0.8f, 0.8f, 0.8f);
    l10gl_light_ambient(&ctx, 0.2f, 0.2f, 0.2f);
    if (l10gl_light_dir(&ctx, 0.5f, 0.7f, 0.5f) < 0) {
        fprintf(stderr, "Failed to configure light direction.\n");
        l10gl_destroy(&ctx);
        return 1;
    }

    float angle = 0.0f;
    int frame = 0;

    printf("Rendering... (Ctrl-C to exit)%s",
           static_mode ? "  [L10GL_STATIC: one frame, then idle]" : "");
    if (frame_limit)
        printf("  [L10GL_FRAMES=%d]", frame_limit);
    putchar('\n');

    while (running) {
        /* Clear both buffers */
        l10gl_clear(&ctx);

        l10gl_matrix_mode(&ctx, L10GL_MATRIX_MODELVIEW);
        l10gl_load_identity(&ctx);
        l10gl_translatef(&ctx, 0, 0, -CAMERA_DIST);
        l10gl_scalef(&ctx, 1, 1, -1);
        l10gl_rotatef(&ctx, angle, 1, 0, 0);
        l10gl_rotatef(&ctx, angle * 0.7f, 0, 1, 0);

        for (int side = 0; side < 6; side++) {
            l10gl_material(&ctx, face_colors[side][0], face_colors[side][1],
                           face_colors[side][2], 1.0f);
            l10gl_normal3f(&ctx, face_normals[side][0],
                           face_normals[side][1], face_normals[side][2]);
            l10gl_begin(&ctx, L10GL_TRIANGLES);
            for (int triangle = 0; triangle < 2; triangle++) {
                const int *indices = cube_faces[side * 2 + triangle];
                const int order[3] = {0, 2, 1};

                for (int corner = 0; corner < 3; corner++) {
                    const float *vertex = cube_verts[indices[order[corner]]];
                    l10gl_vertex3f(&ctx, vertex[0], vertex[1], vertex[2]);
                }
            }
            l10gl_end(&ctx);
        }

        l10gl_wait_engine(&ctx);
        l10gl_swap_buffers(&ctx);   /* tear-free: publish frame at vblank, flip render target */

        frame++;
        if (frame_limit && frame >= frame_limit) {
            printf("Frame limit reached.\n");
            break;
        }

        if (static_mode) {
            printf("Static frame rendered. Ctrl-C to exit.\n");
            while (running)
                usleep(100000);
            break;
        }

        angle += ANGLE_STEP_DEGREES;
        if (angle > 360.0f)
            angle -= 360.0f;

        if (frame % 60 == 0)
            printf("Frame %d\n", frame);
    }

    printf("\nExiting after %d frames.\n", frame);
    l10gl_destroy(&ctx);
    return 0;
}
