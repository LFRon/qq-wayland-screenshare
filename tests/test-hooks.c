#include <dlfcn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

typedef bool (*probe_function)(void);

static probe_function
load_probe(void *module, const char *name)
{
    void *symbol = dlsym(module, name);
    probe_function function = NULL;

    memcpy(&function, &symbol, sizeof(function));
    return function;
}

static bool
unrelated_xgetimage_is_untouched(void)
{
    Display *display;
    int screen;
    Pixmap pixmap = None;
    GC gc = NULL;
    XImage *image = NULL;
    bool untouched = false;

    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "unrelated capture: XOpenDisplay failed\n");
        return false;
    }
    screen = DefaultScreen(display);
    pixmap = XCreatePixmap(display,
                           RootWindow(display, screen),
                           2,
                           2,
                           (unsigned int) DefaultDepth(display, screen));
    gc = XCreateGC(display, pixmap, 0, NULL);
    if (pixmap == None || !gc)
        goto out;

    XSetForeground(display, gc, WhitePixel(display, screen));
    XFillRectangle(display, pixmap, gc, 0, 0, 2, 2);
    XSync(display, False);

    image = XGetImage(display, pixmap, 0, 0, 2, 2, AllPlanes, ZPixmap);
    if (!image) {
        fprintf(stderr, "unrelated capture: XGetImage failed\n");
        goto out;
    }
    untouched = XGetPixel(image, 0, 0) == WhitePixel(display, screen);

out:
    if (image)
        XDestroyImage(image);
    if (gc)
        XFreeGC(display, gc);
    if (pixmap != None)
        XFreePixmap(display, pixmap);
    XCloseDisplay(display);
    return untouched;
}

int
main(int argc, char **argv)
{
    const char *session_type;
    void *module;
    probe_function untracked_xgetimage_untouched;
    probe_function captures_black;

    if (argc != 2) {
        fprintf(stderr, "usage: %s /path/to/broadcast-core.so\n", argv[0]);
        return EXIT_FAILURE;
    }

    session_type = getenv("XDG_SESSION_TYPE");
    if (!session_type || strcmp(session_type, "x11") != 0) {
        fprintf(stderr, "test was not launched with the X11 contract\n");
        return EXIT_FAILURE;
    }
    if (!unrelated_xgetimage_is_untouched()) {
        fprintf(stderr, "unrelated XGetImage call was intercepted\n");
        return EXIT_FAILURE;
    }

    module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }

    untracked_xgetimage_untouched = load_probe(
        module, "fake_broadcast_core_untracked_xgetimage_untouched");
    captures_black = load_probe(module, "fake_broadcast_core_captures_black");
    if (!untracked_xgetimage_untouched || !captures_black) {
        fprintf(stderr, "could not load fake broadcast-core probes\n");
        dlclose(module);
        return EXIT_FAILURE;
    }

    if (!untracked_xgetimage_untouched()) {
        fprintf(stderr,
                "untracked broadcast-core XGetImage was intercepted\n");
        dlclose(module);
        return EXIT_FAILURE;
    }
    if (!captures_black()) {
        fprintf(stderr, "QQ X11 black-frame hooks failed\n");
        dlclose(module);
        return EXIT_FAILURE;
    }

    dlclose(module);
    if (!unrelated_xgetimage_is_untouched()) {
        fprintf(stderr, "QQ display remained tracked after XCloseDisplay\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
