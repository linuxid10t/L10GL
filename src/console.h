#ifndef L10GL_CONSOLE_H
#define L10GL_CONSOLE_H

#include <linux/fb.h>

#include "l10gl.h"

/* Semantic OS operations keep the ownership state machine deterministic in
 * tests without exposing raw variadic ioctl calls. Implementations return an
 * fd or a negative errno-style value. open_active_vt returns -ENOLINK when
 * the target framebuffer is not owned by the active VT. */
struct l10gl_console_ops {
    int (*open_fb)(const char *path);
    int (*get_fb_mode)(int fd, struct fb_var_screeninfo *mode);
    int (*put_fb_mode)(int fd, const struct fb_var_screeninfo *mode);
    int (*open_active_vt)(int fb_fd, int *vt_number);
    int (*get_kd_mode)(int fd, int *mode);
    int (*set_kd_mode)(int fd, int mode);
    int (*close_fd)(int fd);
};

int l10gl_console_acquire(struct l10gl_ctx *ctx,
                          const struct l10gl_backend *backend);
int l10gl_console_release(struct l10gl_ctx *ctx);

/* Internal test seam: production passes the real Linux operations. */
int l10gl_console_acquire_path(struct l10gl_ctx *ctx, const char *path,
                               const struct l10gl_console_ops *ops);

#endif
