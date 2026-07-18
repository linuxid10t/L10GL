#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/major.h>
#include <linux/vt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "console.h"

struct l10gl_console_state {
    const struct l10gl_console_ops *ops;
    struct fb_var_screeninfo saved_fb_mode;
    int fb_fd;
    int vt_fd;
    int vt_number;
    int saved_kd_mode;
};

static int linux_open_fb(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);

    return fd >= 0 ? fd : -errno;
}

static int linux_get_fb_mode(int fd, struct fb_var_screeninfo *mode)
{
    return ioctl(fd, FBIOGET_VSCREENINFO, mode) == 0 ? 0 : -errno;
}

static int linux_put_fb_mode(int fd, const struct fb_var_screeninfo *mode)
{
    struct fb_var_screeninfo request = *mode;

    request.activate = FB_ACTIVATE_NOW;
    return ioctl(fd, FBIOPUT_VSCREENINFO, &request) == 0 ? 0 : -errno;
}

static int linux_open_active_vt(int fb_fd, int *vt_number)
{
    struct vt_stat state;
    struct fb_con2fbmap mapping;
    struct stat fb_stat;
    char path[64];
    int control_fd;
    int vt_fd;

    control_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (control_fd < 0) {
        if (errno == ENOENT || errno == ENODEV || errno == ENXIO)
            return -ENOLINK;
        return -errno;
    }
    if (ioctl(control_fd, VT_GETSTATE, &state) < 0) {
        int error = errno;
        close(control_fd);
        return -error;
    }

    /* Do not put an unrelated active console into graphics mode on a
     * multi-framebuffer machine. If the mapping query is unsupported, retain
     * the traditional assumption that the active VT owns this fbdev. */
    memset(&mapping, 0, sizeof(mapping));
    mapping.console = state.v_active;
    if (fstat(fb_fd, &fb_stat) == 0 && S_ISCHR(fb_stat.st_mode) &&
        major(fb_stat.st_rdev) == FB_MAJOR &&
        ioctl(fb_fd, FBIOGET_CON2FBMAP, &mapping) == 0 &&
        mapping.framebuffer != (unsigned int)minor(fb_stat.st_rdev)) {
        close(control_fd);
        return -ENOLINK;
    }
    close(control_fd);

    snprintf(path, sizeof(path), "/dev/tty%u", state.v_active);
    vt_fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (vt_fd < 0)
        return -errno;
    *vt_number = state.v_active;
    return vt_fd;
}

static int linux_get_kd_mode(int fd, int *mode)
{
    return ioctl(fd, KDGETMODE, mode) == 0 ? 0 : -errno;
}

static int linux_set_kd_mode(int fd, int mode)
{
    return ioctl(fd, KDSETMODE, mode) == 0 ? 0 : -errno;
}

static int linux_close_fd(int fd)
{
    return close(fd) == 0 ? 0 : -errno;
}

static const struct l10gl_console_ops linux_console_ops = {
    .open_fb = linux_open_fb,
    .get_fb_mode = linux_get_fb_mode,
    .put_fb_mode = linux_put_fb_mode,
    .open_active_vt = linux_open_active_vt,
    .get_kd_mode = linux_get_kd_mode,
    .set_kd_mode = linux_set_kd_mode,
    .close_fd = linux_close_fd,
};

static int unavailable_error(int error)
{
    return error == -ENOENT || error == -ENODEV || error == -ENXIO;
}

int l10gl_console_acquire_path(struct l10gl_ctx *ctx, const char *path,
                               const struct l10gl_console_ops *ops)
{
    struct l10gl_console_state *state;
    int ret;

    if (!ctx || !path || !path[0] || !ops || !ops->open_fb ||
        !ops->get_fb_mode || !ops->put_fb_mode || !ops->open_active_vt ||
        !ops->get_kd_mode || !ops->set_kd_mode || !ops->close_fd)
        return -EINVAL;
    if (ctx->console_data)
        return -EBUSY;

    state = calloc(1, sizeof(*state));
    if (!state)
        return -ENOMEM;
    state->ops = ops;
    state->fb_fd = -1;
    state->vt_fd = -1;
    state->vt_number = -1;

    ret = ops->open_fb(path);
    if (ret < 0) {
        free(state);
        if (unavailable_error(ret))
            return 0;
        fprintf(stderr, "L10GL console: cannot open %s: %s\n",
                path, strerror(-ret));
        return ret;
    }
    state->fb_fd = ret;

    ret = ops->get_fb_mode(state->fb_fd, &state->saved_fb_mode);
    if (ret < 0) {
        fprintf(stderr, "L10GL console: cannot save %s mode: %s\n",
                path, strerror(-ret));
        ops->close_fd(state->fb_fd);
        free(state);
        return ret;
    }

    ret = ops->open_active_vt(state->fb_fd, &state->vt_number);
    if (ret == -ENOLINK) {
        /* The mode still needs restoration, but no active fbcon owns this
         * framebuffer and therefore no VT handoff is necessary. */
        ctx->console_data = state;
        printf("L10GL console: saved %s mode; framebuffer is not owned by "
               "the active VT\n", path);
        return 0;
    }
    if (ret < 0) {
        fprintf(stderr, "L10GL console: cannot open the active VT: %s\n",
                strerror(-ret));
        ops->close_fd(state->fb_fd);
        free(state);
        return ret;
    }
    state->vt_fd = ret;

    ret = ops->get_kd_mode(state->vt_fd, &state->saved_kd_mode);
    if (ret < 0) {
        fprintf(stderr, "L10GL console: cannot read VT%d KD mode: %s\n",
                state->vt_number, strerror(-ret));
        ops->close_fd(state->vt_fd);
        ops->close_fd(state->fb_fd);
        free(state);
        return ret;
    }
    if (state->saved_kd_mode != KD_GRAPHICS) {
        ret = ops->set_kd_mode(state->vt_fd, KD_GRAPHICS);
        if (ret < 0) {
            fprintf(stderr,
                    "L10GL console: cannot put VT%d into graphics mode: %s\n",
                    state->vt_number, strerror(-ret));
            ops->close_fd(state->vt_fd);
            ops->close_fd(state->fb_fd);
            free(state);
            return ret;
        }
    }

    ctx->console_data = state;
    printf("L10GL console: saved %s mode; VT%d is in KD_GRAPHICS\n",
           path, state->vt_number);
    return 0;
}

int l10gl_console_acquire(struct l10gl_ctx *ctx,
                          const struct l10gl_backend *backend)
{
    const char *path = NULL;

    if (!ctx || !backend)
        return -EINVAL;
    if (backend->fbdev_env) {
        const char *value = getenv(backend->fbdev_env);

        if (value && value[0])
            path = value;
    }
    if (!path)
        path = backend->fbdev_path;
    if (!path)
        return 0;
    return l10gl_console_acquire_path(ctx, path, &linux_console_ops);
}

int l10gl_console_release(struct l10gl_ctx *ctx)
{
    struct l10gl_console_state *state;
    const struct l10gl_console_ops *ops;
    int result = 0;
    int ret;

    if (!ctx || !ctx->console_data)
        return 0;
    state = ctx->console_data;
    ctx->console_data = NULL;
    ops = state->ops;

    /* Keep fbcon quiescent while the original raster is restored, then give
     * the VT back in precisely the KD mode in which it was found. */
    ret = ops->put_fb_mode(state->fb_fd, &state->saved_fb_mode);
    if (ret < 0) {
        fprintf(stderr, "L10GL console: WARNING: failed to restore fbdev "
                        "mode: %s\n", strerror(-ret));
        result = ret;
    }
    if (state->vt_fd >= 0) {
        ret = ops->set_kd_mode(state->vt_fd, state->saved_kd_mode);
        if (ret < 0) {
            fprintf(stderr, "L10GL console: WARNING: failed to restore VT%d "
                            "KD mode: %s\n",
                    state->vt_number, strerror(-ret));
            if (!result)
                result = ret;
        }
        ops->close_fd(state->vt_fd);
    }
    ops->close_fd(state->fb_fd);
    free(state);
    if (!result)
        printf("L10GL console: restored fbdev mode and console ownership\n");
    return result;
}
