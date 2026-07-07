/*
 * cubefb.c - render the cube to VRAM and CPU-read it back, to settle whether
 * the face-color bleed lives in the framebuffer or is introduced later.
 *
 * Three contamination signals, none of which the first version had all of:
 *   - BLENDS:      pixels that are no pure face color and not background
 *                  (a Gouraud/interpolation blend).
 *   - MISPLACED:   a face-color pixel sitting INSIDE another face's region --
 *                  4-connected, >=3 neighbors of a DIFFERENT face and no
 *                  background neighbor (so a clean shared edge, whose pixels
 *                  have only ~1 other-face neighbor, is not flagged). This is
 *                  the direct detector for "blue on teal": solid blue pixels
 *                  surrounded by teal.
 *   - EXCESS:      a visible face rendering more pixels than its projected
 *                  geometric area (sum of |det|/2 over its two triangles) --
 *                  catches a thin sliver spilling onto a neighbor even when it
 *                  does not form a pocket.
 *
 * The cube is rendered with FLAT full-saturation face colors (no lighting) at
 * several orientations and the framebuffer is CPU-read directly (bypassing the
 * monitor). MISPLACED == 0 and BLENDS == 0 and no EXCESS => the framebuffer is
 * clean and the bleed is monitor/scanout-side; otherwise the readback points
 * at the face pair and orientation.
 *
 * Build: make -B BACKEND=virge cubefb     Run: sudo ./cubefb
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <unistd.h>

#include "backends/virge/virge.h"

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

static void project(struct screen_vertex *o, const float in[3], int sw, int sh, float cd)
{
    float z = in[2] + cd;
    if (z < 0.1f) z = 0.1f;
    float s = (float)sh / z;
    o->sx = (float)sw * 0.5f + in[0] * s;
    o->sy = (float)sh * 0.5f - in[1] * s;
    o->sz = (in[2] + cd - 3.0f) / 4.0f;
    if (o->sz < 0.0f) o->sz = 0.0f;
    if (o->sz > 1.0f) o->sz = 1.0f;
}

static const float cube_verts[8][3] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};
static const int cube_faces[12][3] = {
    {0, 2, 1}, {0, 3, 2},  {4, 5, 6}, {4, 6, 7},  {0, 4, 7}, {0, 7, 3},
    {1, 2, 6}, {1, 6, 5},  {0, 1, 5}, {0, 5, 4},  {3, 7, 6}, {3, 6, 2},
};
static const float face_colors[6][3] = {
    {1,0,0},{0,1,0},{0,0,1},{1,1,0},{1,0,1},{0,1,1},
};
static const char face_letter[6] = { 'r', 'g', 'b', 'y', 'm', 'c' };

/* Classify a 555 pixel: 0..5 = face index, -1 = background, -2 = other. */
static int classify(uint16_t v)
{
    int r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
    const int HI = 27, LO = 4;
    if (r >= HI && g <= LO && b <= LO) return 0;
    if (r <= LO && g >= HI && b <= LO) return 1;
    if (r <= LO && g <= LO && b >= HI) return 2;
    if (r >= HI && g >= HI && b <= LO) return 3;
    if (r >= HI && g <= LO && b >= HI) return 4;
    if (r <= LO && g >= HI && b >= HI) return 5;
    if (r <= LO && g <= LO && b <= LO) return -1;
    return -2;
}

static float tri_area(const struct screen_vertex *a, const struct screen_vertex *b,
                      const struct screen_vertex *c)
{
    float det = (b->sx - a->sx) * (c->sy - a->sy) - (c->sx - a->sx) * (b->sy - a->sy);
    return fabsf(det) * 0.5f;
}

static volatile sig_atomic_t running = 1;
static void sighandler(int s) { (void)s; running = 0; }

int main(void)
{
    struct virge_ctx vctx;
    memset(&vctx, 0, sizeof(vctx));
    if (virge_init(&vctx, 800, 600, 2) < 0) { fprintf(stderr, "init failed\n"); return 1; }
    struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    uint8_t *vram = (uint8_t *)vctx.fb;
    int W = vctx.width, H = vctx.height;
    uint32_t stride = vctx.stride;

    printf("\ncubefb: cube framebuffer readback (%dx%d stride %u) -- bypasses the monitor\n",
           W, H, stride);
    printf("Signals: BLENDS (non-face color), MISPLACED (face color inside another face),\n");
    printf("         EXCESS (face renders more px than its projected area).\n\n");

    float angles[] = { 0.3f, 0.6f, 0.9f, 1.2f, 1.5f };
    long worst = 0;                 /* worst total contamination across angles */

    for (size_t a = 0; a < sizeof(angles)/sizeof(angles[0]); a++) {
        float angle = angles[a];
        for (int y = 0; y < H; y++) {
            uint16_t *row = (uint16_t *)(vram + (size_t)y * stride);
            for (int x = 0; x < W; x++) row[x] = 0;
        }
        virge_clear_z(&vctx, 1.0f);
        virge_wait_engine(&vctx);

        float rot[3][3];
        build_rotation(rot, angle, angle * 0.7f);
        struct screen_vertex projected[8];
        float transformed[8][3];
        for (int i = 0; i < 8; i++) {
            mat3_transform(transformed[i], rot, cube_verts[i]);
            project(&projected[i], transformed[i], W, H, 5.0f);
        }

        /* expected rendered px per face = sum of its 2 triangles' screen area */
        float expected[6] = {0};
        for (int f = 0; f < 6; f++) {
            for (int t = 0; t < 2; t++) {
                const int *fc = cube_faces[2*f + t];
                expected[f] += tri_area(&projected[fc[0]], &projected[fc[1]], &projected[fc[2]]);
            }
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
            float nv[3]; mat3_transform(nv, rot, fn);
            if (nv[2] >= 0.0f) continue;
            float r = face_colors[ci][0], g = face_colors[ci][1], b = face_colors[ci][2];
            int i0 = cube_faces[face][0], i1 = cube_faces[face][1], i2 = cube_faces[face][2];
            struct virge_vertex v0 = { .x=projected[i0].sx, .y=projected[i0].sy, .z=projected[i0].sz, .w=1, .r=r, .g=g, .b=b, .a=1 };
            struct virge_vertex v1 = { .x=projected[i1].sx, .y=projected[i1].sy, .z=projected[i1].sz, .w=1, .r=r, .g=g, .b=b, .a=1 };
            struct virge_vertex v2 = { .x=projected[i2].sx, .y=projected[i2].sy, .z=projected[i2].sz, .w=1, .r=r, .g=g, .b=b, .a=1 };
            virge_draw_triangle_gouraud(&vctx, v0, v1, v2);
        }
        virge_wait_engine(&vctx);

        /* pass 1: classify + count */
        long face_ct[6] = {0}, bg = 0, blends = 0;
        for (int y = 0; y < H; y++) {
            const uint16_t *row = (const uint16_t *)(vram + (size_t)y * stride);
            for (int x = 0; x < W; x++) {
                int c = classify(row[x]);
                if (c >= 0) face_ct[c]++;
                else if (c == -1) bg++;
                else blends++;
            }
        }

        /* pass 2: misplaced solid face color (a face pixel with >=3 neighbors
         * of a DIFFERENT face and no background neighbor -> inside another
         * face's region). */
        long misplaced = 0; int mp_x = -1, mp_y = -1, mp_face = -1, mp_other = -1;
        for (int y = 1; y < H-1; y++) {
            const uint16_t *rowm = (const uint16_t *)(vram + (size_t)(y-1) * stride);
            const uint16_t *row0 = (const uint16_t *)(vram + (size_t)y * stride);
            const uint16_t *rowp = (const uint16_t *)(vram + (size_t)(y+1) * stride);
            for (int x = 1; x < W-1; x++) {
                int c = classify(row0[x]);
                if (c < 0 || c > 5) continue;
                int nb[4] = { classify(row0[x-1]), classify(row0[x+1]),
                              classify(rowm[x]),    classify(rowp[x]) };
                int bg_n = 0, diff[6] = {0};
                for (int k = 0; k < 4; k++) {
                    if (nb[k] == -1) bg_n++;
                    else if (nb[k] >= 0 && nb[k] <= 5 && nb[k] != c) diff[nb[k]]++;
                }
                if (bg_n > 0) continue;
                for (int f = 0; f < 6; f++) {
                    if (diff[f] >= 3) {
                        misplaced++;
                        if (mp_x < 0) { mp_x = x; mp_y = y; mp_face = c; mp_other = f; }
                        break;
                    }
                }
            }
        }

        /* excess coverage: rendered >> expected (loose threshold; min 50px) */
        int excess_face = -1; long excess_by = 0;
        for (int f = 0; f < 6; f++) {
            if (expected[f] < 1.0f) continue;
            long thr = 50 + (long)(expected[f] * 0.10f);
            if (face_ct[f] > (long)expected[f] + thr) {
                long by = face_ct[f] - (long)expected[f];
                if (by > excess_by) { excess_by = by; excess_face = f; }
            }
        }

        long rendered = face_ct[0]+face_ct[1]+face_ct[2]+face_ct[3]+face_ct[4]+face_ct[5];
        long tot = blends + misplaced + (excess_face >= 0 ? 1 : 0);
        if (tot > worst) worst = tot;

        printf("angle %.2f: rendered %ld (bg %ld), blends %ld, misplaced %ld, excess %s\n",
               angle, rendered, bg, blends, misplaced,
               excess_face >= 0 ? "YES" : "no");
        if (misplaced)
            printf("    first misplaced: %c at (%d,%d) inside %c\n",
                   face_letter[mp_face], mp_x, mp_y, face_letter[mp_other]);
        if (excess_face >= 0)
            printf("    excess: %c rendered %ld vs expected ~%ld (+%ld)\n",
                   face_letter[excess_face], face_ct[excess_face],
                   (long)expected[excess_face], excess_by);

        if (a == 1) {
            int y = H / 2;
            const uint16_t *row0 = (const uint16_t *)(vram + (size_t)y * stride);
            printf("  middle row y=%d RLE: ", y);
            int prev = -99, cnt = 0, emitted = 0;
            for (int x = 0; x <= W; x++) {
                int c = (x < W) ? classify(row0[x]) : -99;
                if (x == W || c != prev) {
                    if (cnt > 0) {
                        char ch = (prev == -1) ? '.' : (prev == -2) ? '?' : face_letter[prev];
                        printf("%c%d ", ch, cnt);
                        if (++emitted % 12 == 0) printf("\n          ");
                    }
                    prev = c; cnt = 1;
                } else cnt++;
            }
            printf("\n  (r=Back g=Front b=Left y=Right m=Bottom c=Top .=bg ?=blend)\n");
        }
    }

    printf("\n");
    if (worst == 0)
        printf("=> Framebuffer CLEAN at every orientation (0 blends, 0 misplaced, no\n"
               "   excess). The bleed is NOT in VRAM -- monitor/scanout-side.\n");
    else
        printf("=> Framebuffer NOT clean at some orientation -- real VRAM artifact;\n"
               "   the misplaced/excess lines name the face pair and orientation.\n");

    printf("\nDone. Ctrl-C to exit.\n");
    while (running) { if (getchar() == EOF) break; }
    virge_cleanup(&vctx);
    return 0;
}
