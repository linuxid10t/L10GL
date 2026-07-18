/*
 * cubediag.c - rotating cube + per-face color legend, to inspect bleedthrough.
 *
 * Why this exists: after back-face culling landed (3d4e49c) the cube still
 * shows OCCASIONAL bleedthrough. It is now confirmed to be shared-edge
 * Z-fighting between the VISIBLE faces: at a shared silhouette edge two faces
 * have IDENTICAL z, and under L10GL_LESS the EARLIER-drawn face wins the tie,
 * so its color bleeds into the LATER face's edge. David's observations match
 * the fixed draw order (Back,Front,Left,Right,Bottom,Top) exactly:
 *   blue (Left, idx 2)  -> teal Top (idx 5)
 *   green (Front, idx 1)-> purple Bottom (idx 4)
 *
 * This demo preserves the PRE-FIX cube path (front-end API, L10GL_LESS, fixed
 * face draw order, back-face cull, widened depth range sz=(eye_z+2)/4) with
 * FULL-SATURATION flat face colors (no lighting dimming) and a static color
 * LEGEND down the right side, so the face in view and the face bleeding
 * through can be identified EXACTLY by matching the on-cube color to a swatch.
 * (demos/cube.c now sorts visible faces back-to-front under L10GL_LEQUAL,
 * commit d97577a, and renders clean; this diagnostic intentionally keeps the
 * buggy LESS/fixed-order path as a reference for the Z-fighting it exposed.)
 *
 *   Back=red  Front=green  Left=blue  Right=yellow  Bottom=magenta  Top=cyan
 *   Legend swatches (right side, top->bottom): Back,Front,Left,Right,Bottom,Top
 *
 * Args: `sudo ./cubediag [angle_deg]` -- with an angle, render that single
 * orientation statically (to photograph a specific bleed) and print which
 * faces are visible; without one, slow-rotate.
 *
 * Build: make cubediag     Run: sudo ./cubediag [angle]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>

#include "l10gl.h"

#define PI 3.14159265358979323846f

static void build_rotation(float m[3][3], float ax, float ay)
{
    float sx = sinf(ax), cx = cosf(ax);
    float sy = sinf(ay), cy = cosf(ay);
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

struct screen_vertex { float sx, sy, sz; };

/* Same depth mapping as cube.c (widened: sz = (eye_z+2)/4), but the cube is
 * shifted left and shrunk so the right side is clear for the legend. */
static void project(struct screen_vertex *out, const float in[3],
                    int cx, int cy, float scale, float camera_dist)
{
    float z = in[2] + camera_dist;
    if (z < 0.1f) z = 0.1f;
    float s = scale / z;
    out->sx = (float)cx + in[0] * s;
    out->sy = (float)cy - in[1] * s;
    out->sz = (in[2] + camera_dist - 3.0f) / 4.0f;
    if (out->sz < 0.0f) out->sz = 0.0f;
    if (out->sz > 1.0f) out->sz = 1.0f;
}

static const float cube_verts[8][3] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};

static const int cube_faces[12][3] = {
    {0, 2, 1}, {0, 3, 2},   /* Back   */
    {4, 5, 6}, {4, 6, 7},   /* Front  */
    {0, 4, 7}, {0, 7, 3},   /* Left   */
    {1, 2, 6}, {1, 6, 5},   /* Right  */
    {0, 1, 5}, {0, 5, 4},   /* Bottom */
    {3, 7, 6}, {3, 6, 2},   /* Top    */
};

static const float face_colors[6][3] = {
    {1.0, 0.0, 0.0},  /* Back:   red     */
    {0.0, 1.0, 0.0},  /* Front:  green   */
    {0.0, 0.0, 1.0},  /* Left:   blue    */
    {1.0, 1.0, 0.0},  /* Right:  yellow  */
    {1.0, 0.0, 1.0},  /* Bottom: magenta */
    {0.0, 1.0, 1.0},  /* Top:    cyan    */
};
static const char *face_names[6] = {"Back", "Front", "Left", "Right", "Bottom", "Top"};

/* Flat-colored legend swatch (rectangle) as two z=0 triangles so it always
 * paints on top of the cube. Drawn off to the side, away from the cube. */
static void draw_swatch(struct l10gl_ctx *ctx, int x0, int y0, int x1, int y1,
                        float r, float g, float b)
{
    struct l10gl_vertex a  = { .x = x0, .y = y0, .z = 0.0f, .w = 1.0f, .r = r, .g = g, .b = b, .a = 1.0f };
    struct l10gl_vertex bb = { .x = x1, .y = y0, .z = 0.0f, .w = 1.0f, .r = r, .g = g, .b = b, .a = 1.0f };
    struct l10gl_vertex c  = { .x = x1, .y = y1, .z = 0.0f, .w = 1.0f, .r = r, .g = g, .b = b, .a = 1.0f };
    struct l10gl_vertex d  = { .x = x0, .y = y1, .z = 0.0f, .w = 1.0f, .r = r, .g = g, .b = b, .a = 1.0f };
    l10gl_draw_triangle(ctx, a, bb, c);
    l10gl_draw_triangle(ctx, a, c, d);
}

static volatile int running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

int main(int argc, char **argv)
{
    int width = 800, height = 600, bpp = 2;
    float start_angle = 0.0f;
    int static_mode = 0;
    if (argc >= 2) {
        start_angle = atof(argv[1]) * (float)PI / 180.0f;
        static_mode = 1;
    }

    printf("cubediag: rotating cube + per-face color legend\n");

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    struct l10gl_ctx ctx;
    if (l10gl_create_auto(&ctx, width, height, bpp) < 0) {
        fprintf(stderr, "l10gl_create failed\n");
        return 1;
    }
    printf("Selected backend: %s\n", ctx.backend->name);
    if (ctx.width != width || ctx.height != height) {
        printf("Adopted actual screen %dx%d (requested %dx%d)\n",
               ctx.width, ctx.height, width, height);
        width = ctx.width;
        height = ctx.height;
    }

    l10gl_clear_color(&ctx, 0.0f, 0.0f, 0.0f);
    l10gl_clear_depth(&ctx, 1.0f);
    l10gl_depth_func(&ctx, L10GL_LESS);   /* same depth state as cube.c */

    printf("Face colors: Back=red Front=green Left=blue Right=yellow Bottom=magenta Top=cyan\n");
    printf("Legend swatches (right side, top->bottom): Back, Front, Left, Right, Bottom, Top\n");
    printf("%s\n", static_mode
           ? "Static orientation -- photograph the bleed, Ctrl-C to exit."
           : "Slow rotation -- Ctrl-C to exit.");

    /* Cube sits left of center; legend column on the right. */
    int cx = (int)(width * 0.40f);
    int cy = height / 2;
    float scale = height * 0.78f;

    int leg_x0 = (int)(width * 0.70f);
    int leg_x1 = width - 30;
    int leg_h = 52, leg_gap = 16;
    int leg_y0 = (height - (6 * leg_h + 5 * leg_gap)) / 2;

    float angle = start_angle;
    int frame = 0;
    int first_frame = 1;

    while (running) {
        float rot[3][3];
        build_rotation(rot, angle, angle * 0.5f);

        l10gl_clear(&ctx);

        struct screen_vertex projected[8];
        float transformed[8][3];
        for (int i = 0; i < 8; i++) {
            mat3_transform(transformed[i], rot, cube_verts[i]);
            project(&projected[i], transformed[i], cx, cy, scale, 5.0f);
        }

        for (int face = 0; face < 12; face++) {
            int ci = face / 2;
            float fn[3];
            switch (ci) {
            case 0: fn[0]=0;  fn[1]=0;  fn[2]=-1; break;
            case 1: fn[0]=0;  fn[1]=0;  fn[2]=1;  break;
            case 2: fn[0]=-1; fn[1]=0;  fn[2]=0;  break;
            case 3: fn[0]=1;  fn[1]=0;  fn[2]=0;  break;
            case 4: fn[0]=0;  fn[1]=-1; fn[2]=0;  break;
            case 5: fn[0]=0;  fn[1]=1;  fn[2]=0;  break;
            }
            float nv[3];
            mat3_transform(nv, rot, fn);
            /* Perspective-correct back-face cull (same test as cube.c):
             * visible iff dot(normal, center - eye) < 0; unit-cube face
             * center == normal, eye at origin, cube center at z = +5. */
            if (nv[0]*nv[0] + nv[1]*nv[1] + nv[2]*(nv[2] + 5.0f) >= 0.0f)
                continue;

            float r = face_colors[ci][0];
            float g = face_colors[ci][1];
            float b = face_colors[ci][2];

            int i0 = cube_faces[face][0];
            int i1 = cube_faces[face][1];
            int i2 = cube_faces[face][2];
            struct l10gl_vertex v0 = { .x=projected[i0].sx, .y=projected[i0].sy, .z=projected[i0].sz, .w=1.0f, .r=r, .g=g, .b=b, .a=1.0f };
            struct l10gl_vertex v1 = { .x=projected[i1].sx, .y=projected[i1].sy, .z=projected[i1].sz, .w=1.0f, .r=r, .g=g, .b=b, .a=1.0f };
            struct l10gl_vertex v2 = { .x=projected[i2].sx, .y=projected[i2].sy, .z=projected[i2].sz, .w=1.0f, .r=r, .g=g, .b=b, .a=1.0f };
            l10gl_draw_triangle(&ctx, v0, v1, v2);

            if (static_mode && first_frame && (face % 2 == 0))
                printf("  %-7s visible  (%.0f,%.0f,%.0f)\n",
                       face_names[ci], r, g, b);
        }

        /* Legend: 6 swatches, top->bottom = Back..Top. */
        for (int f = 0; f < 6; f++) {
            int y0 = leg_y0 + f * (leg_h + leg_gap);
            draw_swatch(&ctx, leg_x0, y0, leg_x1, y0 + leg_h,
                        face_colors[f][0], face_colors[f][1], face_colors[f][2]);
        }

        l10gl_wait_engine(&ctx);
        l10gl_swap_buffers(&ctx);

        first_frame = 0;

        if (static_mode) {
            printf("Static frame rendered. Ctrl-C to exit.\n");
            while (running)
                usleep(100000);
            break;
        }

        angle += 0.004f;
        if (angle > 2.0f * PI)
            angle -= 2.0f * PI;

        frame++;
        if (frame % 90 == 0)
            printf("Frame %d\n", frame);
    }

    printf("Exiting.\n");
    l10gl_destroy(&ctx);
    return 0;
}
