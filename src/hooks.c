/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#include "capture.h"
#include "screenshot.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

#define QQ_PRELOAD_EXPORT __attribute__((visibility("default")))

typedef Bool (*xshm_attach_function)(Display *, XShmSegmentInfo *);
typedef Bool (*xshm_get_image_function)(Display *, Drawable, XImage *,
                                        int, int, unsigned long);
typedef XImage *(*xget_image_function)(Display *, Drawable, int, int,
                                      unsigned int, unsigned int,
                                      unsigned long, int);
typedef int (*xclose_display_function)(Display *);
typedef Status (*xget_window_attributes_function)(Display *, Window,
                                                  XWindowAttributes *);
typedef XImage *(*xcreate_image_function)(Display *, Visual *,
                                         unsigned int, int, int, char *,
                                         unsigned int, unsigned int,
                                         int, int);

struct real_functions {
    xshm_attach_function xshm_attach;
    xshm_get_image_function xshm_get_image;
    xget_image_function xget_image;
    xclose_display_function xclose_display;
    xget_window_attributes_function xget_window_attributes;
    xcreate_image_function xcreate_image;
};

struct tracked_display {
    Display *display;
    struct tracked_display *next;
};

static struct real_functions real_functions;
static pthread_once_t real_functions_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t displays_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct tracked_display *tracked_displays;

static bool
hook_debug_enabled(void)
{
    const char *value = getenv("QQ_PRELOAD_DEBUG");

    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void __attribute__((format(printf, 1, 2)))
hook_log(const char *format, ...)
{
    va_list args;

    if (!hook_debug_enabled())
        return;

    fprintf(stderr, "[qq-preload:hook] ");
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

static void
resolve_real_functions(void)
{
    real_functions.xshm_attach =
        (xshm_attach_function) dlsym(RTLD_NEXT, "XShmAttach");
    real_functions.xshm_get_image =
        (xshm_get_image_function) dlsym(RTLD_NEXT, "XShmGetImage");
    real_functions.xget_image =
        (xget_image_function) dlsym(RTLD_NEXT, "XGetImage");
    real_functions.xclose_display =
        (xclose_display_function) dlsym(RTLD_NEXT, "XCloseDisplay");
    real_functions.xget_window_attributes =
        (xget_window_attributes_function)
            dlsym(RTLD_NEXT, "XGetWindowAttributes");
    real_functions.xcreate_image =
        (xcreate_image_function) dlsym(RTLD_NEXT, "XCreateImage");
}

static void
ensure_real_functions(void)
{
    pthread_once(&real_functions_once, resolve_real_functions);
}

static const char *
path_basename(const char *path)
{
    const char *slash;

    if (!path)
        return NULL;
    slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool
caller_module_is(void *return_address, const char *module_name)
{
    Dl_info information;
    const char *basename;

    if (!return_address ||
        dladdr(return_address, &information) == 0 ||
        !information.dli_fname)
        return false;

    basename = path_basename(information.dli_fname);
    return basename && strcmp(basename, module_name) == 0;
}

static bool
caller_is_broadcast_core(void *return_address)
{
    return caller_module_is(return_address, "broadcast-core.so");
}

static bool
caller_is_qq_wrapper(void *return_address)
{
    return caller_module_is(return_address, "wrapper.node");
}

static bool
display_is_tracked_locked(Display *display)
{
    struct tracked_display *entry;

    for (entry = tracked_displays; entry; entry = entry->next) {
        if (entry->display == display)
            return true;
    }
    return false;
}

static bool
display_is_tracked(Display *display)
{
    bool tracked;

    pthread_mutex_lock(&displays_mutex);
    tracked = display_is_tracked_locked(display);
    pthread_mutex_unlock(&displays_mutex);
    return tracked;
}

static void
track_display(Display *display)
{
    struct tracked_display *entry;
    bool added = false;

    if (!display)
        return;

    pthread_mutex_lock(&displays_mutex);
    if (!display_is_tracked_locked(display)) {
        entry = calloc(1, sizeof(*entry));
        if (entry) {
            entry->display = display;
            entry->next = tracked_displays;
            tracked_displays = entry;
            added = true;
        }
    }
    pthread_mutex_unlock(&displays_mutex);

    if (added)
        qq_capture_acquire();
}

static bool
untrack_display(Display *display)
{
    struct tracked_display **link;
    struct tracked_display *entry = NULL;
    bool removed;

    pthread_mutex_lock(&displays_mutex);
    for (link = &tracked_displays; *link; link = &(*link)->next) {
        if ((*link)->display == display) {
            entry = *link;
            *link = entry->next;
            break;
        }
    }
    pthread_mutex_unlock(&displays_mutex);

    removed = entry != NULL;
    free(entry);
    return removed;
}

QQ_PRELOAD_EXPORT Bool
XShmAttach(Display *display, XShmSegmentInfo *segment)
{
    Bool result = False;
    void *return_address = __builtin_return_address(0);
    bool broadcast_core = caller_is_broadcast_core(return_address);

    ensure_real_functions();
    if (real_functions.xshm_attach)
        result = real_functions.xshm_attach(display, segment);

    hook_log("XShmAttach caller=%s display=%p result=%d",
             broadcast_core ? "broadcast-core.so" : "other",
             (void *) display, result);
    if (broadcast_core)
        track_display(display);
    return result;
}

QQ_PRELOAD_EXPORT Bool
XShmGetImage(Display *display, Drawable drawable, XImage *image,
             int x, int y, unsigned long plane_mask)
{
    bool tracked = display_is_tracked(display);

    (void) drawable;
    (void) x;
    (void) y;
    (void) plane_mask;

    hook_log("XShmGetImage display=%p tracked=%d",
             (void *) display, tracked);
    if (tracked) {
        qq_capture_copy_to_ximage(image);
        return True;
    }

    ensure_real_functions();
    if (!real_functions.xshm_get_image)
        return False;
    return real_functions.xshm_get_image(display, drawable, image,
                                         x, y, plane_mask);
}

static XImage *
create_capture_image(Display *display, Drawable drawable,
                     unsigned int width, unsigned int height,
                     int format)
{
    XWindowAttributes attributes;
    XImage *image;
    size_t allocation_size;

    if (width == 0 || height == 0)
        return NULL;
    ensure_real_functions();
    if (!real_functions.xget_window_attributes ||
        !real_functions.xcreate_image)
        return NULL;
    if (!real_functions.xget_window_attributes(display,
                                               (Window) drawable,
                                               &attributes))
        return NULL;

    image = real_functions.xcreate_image(display,
                                        attributes.visual,
                                        (unsigned int) attributes.depth,
                                        format,
                                        0,
                                        NULL,
                                        width,
                                        height,
                                        32,
                                        0);
    if (!image)
        return NULL;
    if (image->bytes_per_line <= 0 || image->height <= 0) {
        XDestroyImage(image);
        return NULL;
    }
    if ((size_t) image->height >
        SIZE_MAX / (size_t) image->bytes_per_line) {
        XDestroyImage(image);
        return NULL;
    }

    allocation_size =
        (size_t) image->bytes_per_line * (size_t) image->height;
    image->data = calloc(1, allocation_size);
    if (!image->data) {
        XDestroyImage(image);
        return NULL;
    }

    return image;
}

static bool
is_qq_screenshot_request(Display *display, Drawable drawable,
                         int x, int y,
                         unsigned int width, unsigned int height,
                         unsigned long plane_mask, int format,
                         void *return_address)
{
    XWindowAttributes attributes;
    int screen;

    if (!display || !caller_is_qq_wrapper(return_address) ||
        x != 0 || y != 0 || plane_mask != AllPlanes || format != ZPixmap)
        return false;

    screen = DefaultScreen(display);
    if (drawable != RootWindow(display, screen))
        return false;

    ensure_real_functions();
    if (!real_functions.xget_window_attributes ||
        !real_functions.xget_window_attributes(display,
                                               (Window) drawable,
                                               &attributes))
        return false;

    return attributes.width > 0 && attributes.height > 0 &&
           width == (unsigned int) attributes.width &&
           height == (unsigned int) attributes.height;
}

QQ_PRELOAD_EXPORT XImage *
XGetImage(Display *display, Drawable drawable, int x, int y,
          unsigned int width, unsigned int height,
          unsigned long plane_mask, int format)
{
    XImage *image;
    void *return_address = __builtin_return_address(0);

    if (display_is_tracked(display)) {
        image = create_capture_image(display, drawable,
                                     width, height, format);
        if (image)
            qq_capture_copy_to_ximage(image);
        return image;
    }

    if (is_qq_screenshot_request(display, drawable, x, y,
                                 width, height, plane_mask, format,
                                 return_address)) {
        image = create_capture_image(display, drawable,
                                     width, height, format);
        if (image)
            qq_screenshot_copy_to_ximage(image);
        hook_log("XGetImage redirected QQ screenshot to portal: %s",
                 image ? "yes" : "image allocation failed");
        return image;
    }

    ensure_real_functions();
    if (!real_functions.xget_image)
        return NULL;
    return real_functions.xget_image(display, drawable, x, y,
                                     width, height, plane_mask, format);
}

QQ_PRELOAD_EXPORT int
XCloseDisplay(Display *display)
{
    bool tracked = untrack_display(display);

    hook_log("XCloseDisplay display=%p tracked=%d",
             (void *) display, tracked);
    if (tracked)
        qq_capture_release();

    ensure_real_functions();
    if (!real_functions.xclose_display)
        return 0;
    return real_functions.xclose_display(display);
}

static void __attribute__((destructor))
qq_preload_shutdown(void)
{
    struct tracked_display *entry;

    qq_screenshot_shutdown();

    pthread_mutex_lock(&displays_mutex);
    entry = tracked_displays;
    tracked_displays = NULL;
    pthread_mutex_unlock(&displays_mutex);

    while (entry) {
        struct tracked_display *next = entry->next;

        free(entry);
        entry = next;
    }
    qq_capture_force_stop();
}
