#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>

static void
wait_for_capture_thread_if_requested(void)
{
    const char *value = getenv("QQ_PRELOAD_TEST_DELAY_MS");
    char *end = NULL;
    unsigned long milliseconds;
    struct timespec delay;

    if (!value || value[0] == '\0')
        return;
    milliseconds = strtoul(value, &end, 10);
    if (!end || *end != '\0' || milliseconds > 5000)
        return;

    delay.tv_sec = (time_t) (milliseconds / 1000);
    delay.tv_nsec = (long) (milliseconds % 1000) * 1000 * 1000;
    nanosleep(&delay, NULL);
}

bool
fake_broadcast_core_untracked_xgetimage_untouched(void)
{
    Display *display;
    int screen;
    Pixmap pixmap = None;
    GC gc = NULL;
    XImage *image = NULL;
    bool untouched = false;

    display = XOpenDisplay(NULL);
    if (!display)
        return false;
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
    untouched = image &&
        XGetPixel(image, 0, 0) == WhitePixel(display, screen);

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

bool
fake_broadcast_core_captures_black(void)
{
    Display *display;
    int screen;
    XImage *image = NULL;
    XImage *fallback = NULL;
    XShmSegmentInfo segment;
    size_t image_size;
    size_t index;
    bool attached = false;
    bool black = false;

    memset(&segment, 0, sizeof(segment));
    segment.shmid = -1;
    segment.shmaddr = (char *) -1;

    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "fake capture: XOpenDisplay failed\n");
        return false;
    }
    screen = DefaultScreen(display);

    image = XShmCreateImage(display,
                            DefaultVisual(display, screen),
                            (unsigned int) DefaultDepth(display, screen),
                            ZPixmap,
                            NULL,
                            &segment,
                            8,
                            8);
    if (!image || image->bytes_per_line <= 0 || image->height <= 0) {
        fprintf(stderr, "fake capture: XShmCreateImage failed\n");
        goto out;
    }

    image_size = (size_t) image->bytes_per_line * (size_t) image->height;
    segment.shmid = shmget(IPC_PRIVATE, image_size, IPC_CREAT | 0600);
    if (segment.shmid < 0) {
        perror("fake capture: shmget");
        goto out;
    }
    segment.shmaddr = shmat(segment.shmid, NULL, 0);
    if (segment.shmaddr == (char *) -1) {
        perror("fake capture: shmat");
        goto out;
    }
    segment.readOnly = False;
    image->data = segment.shmaddr;

    attached = XShmAttach(display, &segment);
    XSync(display, False);
    wait_for_capture_thread_if_requested();

    memset(image->data, 0x7f, image_size);
    if (!XShmGetImage(display,
                      RootWindow(display, screen),
                      image,
                      0,
                      0,
                      AllPlanes)) {
        fprintf(stderr, "fake capture: XShmGetImage returned False\n");
        goto out;
    }

    black = true;
    for (index = 0; index < image_size; index++) {
        if ((uint8_t) image->data[index] != 0) {
            fprintf(stderr,
                    "fake capture: byte %zu is 0x%02x "
                    "(attached=%d, stride=%d, height=%d)\n",
                    index, (unsigned int) (uint8_t) image->data[index],
                    attached, image->bytes_per_line, image->height);
            black = false;
            break;
        }
    }
    if (!black)
        goto out;

    fallback = XGetImage(display,
                         RootWindow(display, screen),
                         0,
                         0,
                         8,
                         8,
                         AllPlanes,
                         ZPixmap);
    if (!fallback || !fallback->data ||
        fallback->bytes_per_line <= 0 || fallback->height <= 0) {
        fprintf(stderr, "fake capture: XGetImage fallback failed\n");
        black = false;
        goto out;
    }
    image_size = (size_t) fallback->bytes_per_line *
                 (size_t) fallback->height;
    for (index = 0; index < image_size; index++) {
        if ((uint8_t) fallback->data[index] != 0) {
            fprintf(stderr,
                    "fake capture: XGetImage fallback byte %zu is 0x%02x\n",
                    index, (unsigned int) (uint8_t) fallback->data[index]);
            black = false;
            break;
        }
    }

out:
    if (fallback)
        XDestroyImage(fallback);
    if (attached) {
        XShmDetach(display, &segment);
        XSync(display, False);
    }
    if (image) {
        image->data = NULL;
        XDestroyImage(image);
    }
    if (segment.shmaddr != (char *) -1)
        shmdt(segment.shmaddr);
    if (segment.shmid >= 0)
        shmctl(segment.shmid, IPC_RMID, NULL);
    XCloseDisplay(display);
    return black;
}
