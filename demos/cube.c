/*
 * cube.c - Spinning Gouraud-shaded cube demo using L10GL.
 *
 * Hardware-agnostic — uses the L10GL frontend API. The backend is selected
 * at compile time (currently mga1064, but any backend implementing the
 * l10gl_backend vtable works).
 *
 * Build: make
 * Run:   sudo ./cube [width height bpp]
 *
 * Controls: Ctrl-C to exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>

#include "l10gl.h"

/* -----------------------------------------------------------------------
 * Minimal 3D Math
 * -----------------------------------------------------------------------
 */

#define PI 3.14159265358979323846f

/* Build a rotation matrix for combined X and Y rotation.
 * Result is RotY * RotX, row-major 3x3. */
static void build_rotation(float m[3][3], float angle_x, float angle_y)
{
    float sx = sinf(angle_x), cx = cosf(angle_x);
    float sy = sinf(angle_y), cy = cosf(angle_y);

    m[0][0] = cy;     m[0][1] = 0;      m[0][2] = sy;
    m[1][0] = sx*sy;  m[1][1] = cx;     m[1][2] = -sx*cy;
    m[2][0] = -cx*sy; m[2][1] = sx;     m[2][2] = cx*cy;
}

static void mat3_transform(float out[3], const float m[3][3], const float in[3])
{
    out[0] = m[0][0]*in[0] + m[0][1]*in[1] + m[0][2]*in[2];
    out[1] = m[1][0]*in[0] + m[1][1]*in[1] + m[1][2]*in[2];
    out[2] = m[2][0]*in[0] + m[2][1]*in[1] + m[2][2]*in[2];
}

/* Perspective project a 3D point to screen coordinates */
struct screen_vertex {
    float sx, sy, sz;
};

static void project(struct screen_vertex *out, const float in[3],
                     int screen_w, int screen_h, float camera_dist)
{
    float z = in[2] + camera_dist;
    if (z < 0.1f) z = 0.1f;

    float scale = (float)screen_h / z;

    out->sx = (float)screen_w * 0.5f + in[0] * scale;
    out->sy = (float)screen_h * 0.5f - in[1] * scale;
    /* Map eye-z to a wide [0,1] slice (~32k of 65536 Z levels separate
     * front/back), not the old /1000 collapse (~130 levels -> back faces
     * Z-fight through front at grazing angles). */
    out->sz = (in[2] + camera_dist - 3.0f) / 4.0f;

    if (out->sz < 0.0f) out->sz = 0.0f;
    if (out->sz > 1.0f) out->sz = 1.0f;
}

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

/* The 4 corner vertices of each face (for depth-sorting visible faces
 * back-to-front). Index order is irrelevant -- only the mean is used. */
static const int face_quads[6][4] = {
    {0, 1, 2, 3}, /* Back   */
    {4, 5, 6, 7}, /* Front  */
    {0, 3, 4, 7}, /* Left   */
    {1, 2, 5, 6}, /* Right  */
    {0, 1, 4, 5}, /* Bottom */
    {2, 3, 6, 7}, /* Top    */
};

static const float face_colors[6][3] = {
    {1.0, 0.0, 0.0},  /* Back:   red     */
    {0.0, 1.0, 0.0},  /* Front:  green   */
    {0.0, 0.0, 1.0},  /* Left:   blue    */
    {1.0, 1.0, 0.0},  /* Right:  yellow  */
    {1.0, 0.0, 1.0},  /* Bottom: magenta */
    {0.0, 1.0, 1.0},  /* Top:    cyan    */
};

/* -----------------------------------------------------------------------
 * Lighting
 * -----------------------------------------------------------------------
 */

static float light_dir[3] = { 0.5f, 0.7f, -0.5f };

static void normalize(float v[3])
{
    float len = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len < 1e-6f) return;
    v[0] /= len; v[1] /= len; v[2] /= len;
}

static float diffuse_light(const float normal[3])
{
    float d = -(normal[0]*light_dir[0] +
                normal[1]*light_dir[1] +
                normal[2]*light_dir[2]);
    if (d < 0.0f) d = 0.0f;
    return 0.2f + 0.8f * d;
}

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

    /* Select backend at compile time.
     * The Makefile defines -DBACKEND_VIRGE or -DBACKEND_MGA1064. */
#ifdef BACKEND_VIRGE
    const struct l10gl_backend *backend = &virge_backend;
#else
    const struct l10gl_backend *backend = &mga1064_backend;
#endif

    printf("L10GL Gouraud Cube Demo (backend: %s)\n", backend->name);
    printf("Initializing %dx%d @ %dbpp...\n", width, height, bpp * 8);

    struct l10gl_ctx ctx;
    if (l10gl_create(&ctx, backend, width, height, bpp) < 0) {
        fprintf(stderr, "Failed to initialize L10GL.\n");
        return 1;
    }

    /* The backend may adopt the real screen mode instead of the request
     * (native scanout takeover on no-fbdev machines) -- render to what
     * we actually got or the projection/stride math is wrong. */
    if (ctx.width != width || ctx.height != height) {
        printf("Adopted actual screen %dx%d (requested %dx%d)\n",
               ctx.width, ctx.height, width, height);
        width = ctx.width;
        height = ctx.height;
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    normalize(light_dir);

    /* L10GL_STATIC=1: render a single frame and idle, so it can be
     * photographed without tearing. Used to tell whether the animated
     * cube's "blacked out below ~2/5" is a render limit or vsync-less
     * single-buffer tearing (a static frame has no re-clear cycle). */
    int static_mode = getenv("L10GL_STATIC") != NULL;

    /* Set clear values */
    l10gl_clear_color(&ctx, 0.0f, 0.0f, 0.0f);   /* black background */
    l10gl_clear_depth(&ctx, 1.0f);                /* far Z */
    /* LEQUAL + back-to-front draw order: at a shared silhouette edge two
     * faces tie in z, and LEQUAL lets the later-drawn (nearer) face win the
     * tie. Drawing visible faces far-to-near therefore makes the nearer
     * face own each shared edge, so the earlier face no longer bleeds
     * through (which happened under LESS, where the later face lost ties). */
    l10gl_depth_func(&ctx, L10GL_LEQUAL);

    float angle = 0.0f;
    int frame = 0;

    printf("Rendering... (Ctrl-C to exit)%s\n",
           static_mode ? "  [L10GL_STATIC: one frame, then idle]" : "");

    while (running) {
        float rot[3][3];
        build_rotation(rot, angle, angle * 0.7f);

        /* Clear both buffers */
        l10gl_clear(&ctx);

        /* Transform all 8 cube vertices */
        struct screen_vertex projected[8];
        float transformed[8][3];

        for (int i = 0; i < 8; i++) {
            mat3_transform(transformed[i], rot, cube_verts[i]);
            project(&projected[i], transformed[i],
                    width, height, 5.0f);
        }

        /* Draw the cube in two phases:
         *   1) cull back-faces, and for each visible face record a
         *      representative depth (mean eye-z of its 4 corners) and its
         *      lit color;
         *   2) draw the visible faces BACK-TO-FRONT (far first).
         * Under L10GL_LEQUAL a shared silhouette edge -- where two faces
         * have identical z -- is won by whichever face is drawn LAST, so
         * far-to-near order hands each edge to the nearer (correct) face
         * and removes the bleedthrough. */
        int vis_face[6];
        float vis_depth[6];
        float vis_r[6], vis_g[6], vis_b[6];
        int n_vis = 0;

        for (int f = 0; f < 6; f++) {
            float face_normal[3];
            switch (f) {
            case 0: face_normal[0]=0;  face_normal[1]=0;  face_normal[2]=-1; break;
            case 1: face_normal[0]=0;  face_normal[1]=0;  face_normal[2]=1;  break;
            case 2: face_normal[0]=-1; face_normal[1]=0;  face_normal[2]=0;  break;
            case 3: face_normal[0]=1;  face_normal[1]=0;  face_normal[2]=0;  break;
            case 4: face_normal[0]=0;  face_normal[1]=-1; face_normal[2]=0;  break;
            case 5: face_normal[0]=0;  face_normal[1]=1;  face_normal[2]=0;  break;
            }
            float normal_view[3];
            mat3_transform(normal_view, rot, face_normal);

            /* Back-face cull: camera at origin looking +Z, so a face is
             * visible iff its view-space normal points back toward the
             * camera (a negative Z component). */
            if (normal_view[2] >= 0.0f)
                continue;

            float cz = 0.0f;  /* mean eye-z of the face's 4 corners */
            for (int k = 0; k < 4; k++)
                cz += transformed[face_quads[f][k]][2];
            cz *= 0.25f;

            float intensity = diffuse_light(normal_view);
            vis_face[n_vis]  = f;
            vis_depth[n_vis] = cz;
            vis_r[n_vis]     = face_colors[f][0] * intensity;
            vis_g[n_vis]     = face_colors[f][1] * intensity;
            vis_b[n_vis]     = face_colors[f][2] * intensity;
            n_vis++;
        }

        /* Insertion sort, far (large eye-z) first. At most 3 visible faces. */
        for (int a = 1; a < n_vis; a++) {
            int vf = vis_face[a];
            float vd = vis_depth[a];
            int j = a - 1;
            while (j >= 0 && vis_depth[j] < vd) {
                vis_face[j + 1]  = vis_face[j];
                vis_depth[j + 1] = vis_depth[j];
                j--;
            }
            vis_face[j + 1]  = vf;
            vis_depth[j + 1] = vd;
        }

        /* Draw visible faces far-to-near, both triangles per face. */
        for (int v = 0; v < n_vis; v++) {
            int color_idx = vis_face[v];
            float r = vis_r[v], g = vis_g[v], b = vis_b[v];
            for (int t = 0; t < 2; t++) {
                int face = color_idx * 2 + t;
                int i0 = cube_faces[face][0];
                int i1 = cube_faces[face][1];
                int i2 = cube_faces[face][2];
                struct l10gl_vertex v0 = {
                    .x = projected[i0].sx, .y = projected[i0].sy,
                    .z = projected[i0].sz, .r = r, .g = g, .b = b, .a = 1.0f
                };
                struct l10gl_vertex v1 = {
                    .x = projected[i1].sx, .y = projected[i1].sy,
                    .z = projected[i1].sz, .r = r, .g = g, .b = b, .a = 1.0f
                };
                struct l10gl_vertex v2 = {
                    .x = projected[i2].sx, .y = projected[i2].sy,
                    .z = projected[i2].sz, .r = r, .g = g, .b = b, .a = 1.0f
                };
                l10gl_draw_triangle(&ctx, v0, v1, v2);
            }
        }

        l10gl_wait_engine(&ctx);
        l10gl_swap_buffers(&ctx);   /* tear-free: publish frame at vblank, flip render target */

        if (static_mode) {
            printf("Static frame %d rendered. Ctrl-C to exit.\n", frame);
            while (running)
                usleep(100000);
            break;
        }

        angle += 0.02f;
        if (angle > 2.0f * PI)
            angle -= 2.0f * PI;

        frame++;
        if (frame % 60 == 0)
            printf("Frame %d\n", frame);
    }

    printf("\nExiting after %d frames.\n", frame);
    l10gl_destroy(&ctx);
    return 0;
}
