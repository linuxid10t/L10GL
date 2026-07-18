/* Deterministic tests for the P2 console ownership state machine. */

#include <errno.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <stdio.h>
#include <string.h>

#include "console.h"
#include "l10gl.h"

enum event {
    EVENT_OPEN_FB,
    EVENT_GET_FB,
    EVENT_OPEN_VT,
    EVENT_GET_KD,
    EVENT_SET_GRAPHICS,
    EVENT_PUT_FB,
    EVENT_SET_SAVED_KD,
    EVENT_CLOSE_VT,
    EVENT_CLOSE_FB,
};

struct fake_state {
    enum event events[32];
    int event_count;
    int open_fb_result;
    int open_vt_result;
    int get_fb_result;
    int get_kd_result;
    int set_graphics_result;
    int put_fb_result;
    int set_saved_result;
    int saved_kd_mode;
    struct fb_var_screeninfo original_fb;
    struct fb_var_screeninfo restored_fb;
};

static struct fake_state fake;
static int failures;

static void record(enum event event)
{
    if (fake.event_count < (int)(sizeof(fake.events) / sizeof(fake.events[0])))
        fake.events[fake.event_count++] = event;
}

static void reset_fake(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.open_fb_result = 10;
    fake.open_vt_result = 11;
    fake.saved_kd_mode = KD_TEXT;
    fake.original_fb.xres = 800;
    fake.original_fb.yres = 600;
    fake.original_fb.bits_per_pixel = 32;
    fake.original_fb.red.offset = 16;
    fake.original_fb.green.offset = 8;
    fake.original_fb.blue.offset = 0;
}

static void expect(int condition, const char *label)
{
    if (condition)
        return;
    fprintf(stderr, "test-console: FAIL: %s\n", label);
    failures++;
}

static void expect_events(const enum event *expected, int count,
                          const char *label)
{
    int match = fake.event_count == count;

    for (int i = 0; match && i < count; i++)
        match = fake.events[i] == expected[i];
    expect(match, label);
}

static int fake_open_fb(const char *path)
{
    expect(strcmp(path, "/dev/fakefb") == 0, "fbdev path");
    record(EVENT_OPEN_FB);
    return fake.open_fb_result;
}

static int fake_get_fb(int fd, struct fb_var_screeninfo *mode)
{
    expect(fd == 10, "get fb fd");
    record(EVENT_GET_FB);
    if (fake.get_fb_result)
        return fake.get_fb_result;
    *mode = fake.original_fb;
    return 0;
}

static int fake_put_fb(int fd, const struct fb_var_screeninfo *mode)
{
    expect(fd == 10, "put fb fd");
    record(EVENT_PUT_FB);
    fake.restored_fb = *mode;
    return fake.put_fb_result;
}

static int fake_open_vt(int fb_fd, int *vt_number)
{
    expect(fb_fd == 10, "open vt fb fd");
    record(EVENT_OPEN_VT);
    if (fake.open_vt_result < 0)
        return fake.open_vt_result;
    *vt_number = 7;
    return fake.open_vt_result;
}

static int fake_get_kd(int fd, int *mode)
{
    expect(fd == 11, "get KD fd");
    record(EVENT_GET_KD);
    if (fake.get_kd_result)
        return fake.get_kd_result;
    *mode = fake.saved_kd_mode;
    return 0;
}

static int fake_set_kd(int fd, int mode)
{
    expect(fd == 11, "set KD fd");
    if (mode == KD_GRAPHICS && fake.saved_kd_mode != KD_GRAPHICS) {
        record(EVENT_SET_GRAPHICS);
        return fake.set_graphics_result;
    }
    record(EVENT_SET_SAVED_KD);
    expect(mode == fake.saved_kd_mode, "restore exact KD mode");
    return fake.set_saved_result;
}

static int fake_close(int fd)
{
    if (fd == 11)
        record(EVENT_CLOSE_VT);
    else if (fd == 10)
        record(EVENT_CLOSE_FB);
    else
        expect(0, "close known fd");
    return 0;
}

static const struct l10gl_console_ops fake_ops = {
    .open_fb = fake_open_fb,
    .get_fb_mode = fake_get_fb,
    .put_fb_mode = fake_put_fb,
    .open_active_vt = fake_open_vt,
    .get_kd_mode = fake_get_kd,
    .set_kd_mode = fake_set_kd,
    .close_fd = fake_close,
};

static void test_full_lifecycle(void)
{
    static const enum event expected[] = {
        EVENT_OPEN_FB, EVENT_GET_FB, EVENT_OPEN_VT, EVENT_GET_KD,
        EVENT_SET_GRAPHICS, EVENT_PUT_FB, EVENT_SET_SAVED_KD,
        EVENT_CLOSE_VT, EVENT_CLOSE_FB,
    };
    struct l10gl_ctx ctx = {0};

    reset_fake();
    expect(l10gl_console_acquire_path(&ctx, "/dev/fakefb", &fake_ops) == 0,
           "acquire full console");
    expect(ctx.console_data != NULL, "console state installed");
    expect(l10gl_console_release(&ctx) == 0, "release full console");
    expect(ctx.console_data == NULL, "console state cleared");
    expect(!memcmp(&fake.restored_fb, &fake.original_fb,
                   sizeof(fake.original_fb)), "restore exact fbdev mode");
    expect_events(expected, (int)(sizeof(expected) / sizeof(expected[0])),
                  "full lifecycle ordering");
    expect(l10gl_console_release(&ctx) == 0, "idempotent release");
}

static void test_unowned_framebuffer(void)
{
    static const enum event expected[] = {
        EVENT_OPEN_FB, EVENT_GET_FB, EVENT_OPEN_VT, EVENT_PUT_FB,
        EVENT_CLOSE_FB,
    };
    struct l10gl_ctx ctx = {0};

    reset_fake();
    fake.open_vt_result = -ENOLINK;
    expect(l10gl_console_acquire_path(&ctx, "/dev/fakefb", &fake_ops) == 0,
           "acquire unowned framebuffer mode");
    expect(ctx.console_data != NULL, "unowned framebuffer state installed");
    expect(l10gl_console_release(&ctx) == 0,
           "release unowned framebuffer mode");
    expect_events(expected, (int)(sizeof(expected) / sizeof(expected[0])),
                  "unowned framebuffer ordering");
}

static void test_existing_graphics_mode_preserved(void)
{
    static const enum event expected[] = {
        EVENT_OPEN_FB, EVENT_GET_FB, EVENT_OPEN_VT, EVENT_GET_KD,
        EVENT_PUT_FB, EVENT_SET_SAVED_KD, EVENT_CLOSE_VT, EVENT_CLOSE_FB,
    };
    struct l10gl_ctx ctx = {0};

    reset_fake();
    fake.saved_kd_mode = KD_GRAPHICS;
    expect(l10gl_console_acquire_path(&ctx, "/dev/fakefb", &fake_ops) == 0,
           "acquire existing graphics VT");
    expect(l10gl_console_release(&ctx) == 0,
           "release existing graphics VT");
    expect_events(expected, (int)(sizeof(expected) / sizeof(expected[0])),
                  "existing graphics KD mode preserved");
}

static void test_acquire_failure_unwinds(void)
{
    static const enum event expected[] = {
        EVENT_OPEN_FB, EVENT_GET_FB, EVENT_OPEN_VT, EVENT_GET_KD,
        EVENT_SET_GRAPHICS, EVENT_CLOSE_VT, EVENT_CLOSE_FB,
    };
    struct l10gl_ctx ctx = {0};

    reset_fake();
    fake.set_graphics_result = -EPERM;
    expect(l10gl_console_acquire_path(&ctx, "/dev/fakefb", &fake_ops) ==
           -EPERM, "graphics-mode failure returned");
    expect(ctx.console_data == NULL, "failed acquire leaves no state");
    expect_events(expected, (int)(sizeof(expected) / sizeof(expected[0])),
                  "acquire failure unwind ordering");
}

static void test_restore_failure_continues(void)
{
    static const enum event expected[] = {
        EVENT_OPEN_FB, EVENT_GET_FB, EVENT_OPEN_VT, EVENT_GET_KD,
        EVENT_SET_GRAPHICS, EVENT_PUT_FB, EVENT_SET_SAVED_KD,
        EVENT_CLOSE_VT, EVENT_CLOSE_FB,
    };
    struct l10gl_ctx ctx = {0};

    reset_fake();
    fake.put_fb_result = -EIO;
    expect(l10gl_console_acquire_path(&ctx, "/dev/fakefb", &fake_ops) == 0,
           "acquire before restore failure");
    expect(l10gl_console_release(&ctx) == -EIO,
           "fbdev restore failure returned");
    expect_events(expected, (int)(sizeof(expected) / sizeof(expected[0])),
                  "restore failure still restores KD and closes");
}

static void test_unavailable_fbdev_is_noop(void)
{
    static const enum event expected[] = { EVENT_OPEN_FB };
    struct l10gl_ctx ctx = {0};

    reset_fake();
    fake.open_fb_result = -ENOENT;
    expect(l10gl_console_acquire_path(&ctx, "/dev/fakefb", &fake_ops) == 0,
           "missing fbdev is optional");
    expect(ctx.console_data == NULL, "missing fbdev installs no state");
    expect_events(expected, 1, "missing fbdev ordering");
}

int main(void)
{
    test_full_lifecycle();
    test_unowned_framebuffer();
    test_existing_graphics_mode_preserved();
    test_acquire_failure_unwinds();
    test_restore_failure_continues();
    test_unavailable_fbdev_is_noop();
    if (failures)
        return 1;
    printf("test-console: PASS (ownership, unwind, restore ordering)\n");
    return 0;
}
