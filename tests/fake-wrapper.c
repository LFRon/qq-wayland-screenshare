#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

static bool
paint_root_white(Display *display, int screen)
{
    XWindowAttributes attributes;
    Window root = RootWindow(display, screen);
    GC gc;

    if (!XGetWindowAttributes(display, root, &attributes))
        return false;
    gc = XCreateGC(display, root, 0, NULL);
    if (!gc)
        return false;

    XSetForeground(display, gc, WhitePixel(display, screen));
    XFillRectangle(display, root, gc, 0, 0,
                   (unsigned int) attributes.width,
                   (unsigned int) attributes.height);
    XFreeGC(display, gc);
    XSync(display, False);
    return true;
}

bool
fake_qq_wrapper_partial_xgetimage_untouched(void)
{
    Display *display;
    XImage *image = NULL;
    int screen;
    bool untouched = false;

    display = XOpenDisplay(NULL);
    if (!display)
        return false;
    screen = DefaultScreen(display);
    if (!paint_root_white(display, screen))
        goto out;

    image = XGetImage(display,
                      RootWindow(display, screen),
                      0, 0, 2, 2, AllPlanes, ZPixmap);
    untouched = image &&
        XGetPixel(image, 0, 0) == WhitePixel(display, screen);

out:
    if (image)
        XDestroyImage(image);
    XCloseDisplay(display);
    return untouched;
}

bool
fake_qq_wrapper_screenshot_is_black(void)
{
    Display *display;
    XWindowAttributes attributes;
    XImage *image = NULL;
    Window root;
    size_t image_size;
    size_t index;
    int screen;
    bool black = false;

    display = XOpenDisplay(NULL);
    if (!display)
        return false;
    screen = DefaultScreen(display);
    root = RootWindow(display, screen);
    if (!paint_root_white(display, screen) ||
        !XGetWindowAttributes(display, root, &attributes))
        goto out;

    image = XGetImage(display, root, 0, 0,
                      (unsigned int) attributes.width,
                      (unsigned int) attributes.height,
                      AllPlanes, ZPixmap);
    if (!image || !image->data || image->bytes_per_line <= 0 ||
        image->height <= 0)
        goto out;

    image_size = (size_t) image->bytes_per_line * (size_t) image->height;
    black = true;
    for (index = 0; index < image_size; index++) {
        if ((uint8_t) image->data[index] != 0) {
            black = false;
            break;
        }
    }

out:
    if (image)
        XDestroyImage(image);
    XCloseDisplay(display);
    return black;
}
