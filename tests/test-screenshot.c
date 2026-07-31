#include "screenshot.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>

static void
initialize_ximage(XImage *image, uint8_t *data, int width, int height)
{
    memset(image, 0, sizeof(*image));
    image->width = width;
    image->height = height;
    image->format = ZPixmap;
    image->data = (char *) data;
    image->byte_order = LSBFirst;
    image->bitmap_unit = 32;
    image->bitmap_bit_order = LSBFirst;
    image->bitmap_pad = 32;
    image->depth = 24;
    image->bytes_per_line = width * 4;
    image->bits_per_pixel = 32;
    image->red_mask = 0x00ff0000UL;
    image->green_mask = 0x0000ff00UL;
    image->blue_mask = 0x000000ffUL;
}

static bool
pixel_equals(const uint8_t *data, size_t pixel,
             uint8_t blue, uint8_t green, uint8_t red)
{
    const uint8_t *value = data + pixel * 4U;

    return value[0] == blue && value[1] == green &&
           value[2] == red && value[3] == 0;
}

static bool
test_png_uri(void)
{
    static const uint8_t colors[][3] = {
        { 255, 0, 0 },
        { 0, 255, 0 },
        { 0, 0, 255 },
        { 255, 255, 255 },
    };
    GdkPixbuf *pixbuf = NULL;
    GError *error = NULL;
    char *path = NULL;
    char *uri = NULL;
    uint8_t destination[16];
    XImage image;
    int fd = -1;
    int rowstride;
    uint8_t *pixels;
    size_t index;
    bool ok = false;

    fd = g_file_open_tmp("qq-preload-screenshot-XXXXXX.png",
                         &path, &error);
    if (fd < 0)
        goto out;
    close(fd);
    fd = -1;

    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 2, 2);
    if (!pixbuf)
        goto out;
    pixels = gdk_pixbuf_get_pixels(pixbuf);
    rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    for (index = 0; index < 4; index++) {
        int x = (int) (index % 2U);
        int y = (int) (index / 2U);
        uint8_t *pixel = pixels + y * rowstride + x * 3;

        memcpy(pixel, colors[index], 3);
    }
    if (!gdk_pixbuf_save(pixbuf, path, "png", &error, NULL))
        goto out;

    uri = g_filename_to_uri(path, NULL, &error);
    if (!uri)
        goto out;

    memset(destination, 0x7f, sizeof(destination));
    initialize_ximage(&image, destination, 2, 2);
    ok = qq_screenshot_copy_uri_to_ximage(uri, &image) &&
         pixel_equals(destination, 0, 0, 0, 255) &&
         pixel_equals(destination, 1, 0, 255, 0) &&
         pixel_equals(destination, 2, 255, 0, 0) &&
         pixel_equals(destination, 3, 255, 255, 255);

out:
    if (error) {
        fprintf(stderr, "screenshot fixture failed: %s\n", error->message);
        g_error_free(error);
    }
    if (pixbuf)
        g_object_unref(pixbuf);
    if (fd >= 0)
        close(fd);
    if (path)
        unlink(path);
    g_free(uri);
    g_free(path);
    return ok;
}

static bool
test_invalid_uri_returns_black(void)
{
    uint8_t destination[16];
    XImage image;
    size_t index;

    memset(destination, 0x7f, sizeof(destination));
    initialize_ximage(&image, destination, 2, 2);
    if (qq_screenshot_copy_uri_to_ximage(
            "file:///definitely-not-a-qq-screenshot", &image))
        return false;
    for (index = 0; index < sizeof(destination); index++) {
        if (destination[index] != 0)
            return false;
    }
    return true;
}

int
main(void)
{
    if (!test_png_uri()) {
        fprintf(stderr, "portal screenshot URI conversion failed\n");
        return EXIT_FAILURE;
    }
    if (!test_invalid_uri_returns_black()) {
        fprintf(stderr, "invalid screenshot URI did not return black\n");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
