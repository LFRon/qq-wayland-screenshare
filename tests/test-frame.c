#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    return value[0] == blue &&
           value[1] == green &&
           value[2] == red &&
           value[3] == 0;
}

static bool
test_bgrx_copy(void)
{
    static const uint8_t source[] = {
        0, 0, 255, 0, 0, 255, 0, 0,
        255, 0, 0, 0, 255, 255, 255, 0,
    };
    uint8_t destination[sizeof(source)];
    struct qq_capture_frame frame = {
        .data = source,
        .size = sizeof(source),
        .width = 2,
        .height = 2,
        .stride = 8,
        .format = SPA_VIDEO_FORMAT_BGRx,
        .transform = SPA_META_TRANSFORMATION_None,
    };
    XImage image;

    memset(destination, 0x7f, sizeof(destination));
    initialize_ximage(&image, destination, 2, 2);
    if (!qq_frame_copy_to_ximage(&frame, &image))
        return false;

    return pixel_equals(destination, 0, 0, 0, 255) &&
           pixel_equals(destination, 1, 0, 255, 0) &&
           pixel_equals(destination, 2, 255, 0, 0) &&
           pixel_equals(destination, 3, 255, 255, 255);
}

static bool
test_supported_formats(void)
{
    static const struct {
        enum spa_video_format format;
        uint8_t source[4];
    } cases[] = {
        { SPA_VIDEO_FORMAT_BGRx, { 0x11, 0x22, 0x33, 0x00 } },
        { SPA_VIDEO_FORMAT_BGRA, { 0x11, 0x22, 0x33, 0x7f } },
        { SPA_VIDEO_FORMAT_RGBx, { 0x33, 0x22, 0x11, 0x00 } },
        { SPA_VIDEO_FORMAT_RGBA, { 0x33, 0x22, 0x11, 0x7f } },
    };
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        uint8_t destination[4];
        struct qq_capture_frame frame = {
            .data = cases[index].source,
            .size = sizeof(cases[index].source),
            .width = 1,
            .height = 1,
            .stride = 4,
            .format = cases[index].format,
            .transform = SPA_META_TRANSFORMATION_None,
        };
        XImage image;

        initialize_ximage(&image, destination, 1, 1);
        if (!qq_frame_copy_to_ximage(&frame, &image) ||
            !pixel_equals(destination, 0, 0x11, 0x22, 0x33)) {
            return false;
        }
    }

    return true;
}

static bool
test_aspect_fit_blacks_unused_pixels(void)
{
    static const uint8_t source[] = {
        0, 0, 255, 0, 0, 255, 0, 0,
    };
    uint8_t destination[16];
    struct qq_capture_frame frame = {
        .data = source,
        .size = sizeof(source),
        .width = 2,
        .height = 1,
        .stride = 8,
        .format = SPA_VIDEO_FORMAT_BGRx,
        .transform = SPA_META_TRANSFORMATION_None,
    };
    XImage image;

    memset(destination, 0x7f, sizeof(destination));
    initialize_ximage(&image, destination, 2, 2);
    if (!qq_frame_copy_to_ximage(&frame, &image))
        return false;

    return pixel_equals(destination, 0, 0, 0, 255) &&
           pixel_equals(destination, 1, 0, 255, 0) &&
           pixel_equals(destination, 2, 0, 0, 0) &&
           pixel_equals(destination, 3, 0, 0, 0);
}

static bool
test_crop(void)
{
    static const uint8_t source[] = {
        0, 0, 10, 0, 0, 0, 20, 0, 0, 0, 30, 0,
        0, 0, 40, 0, 0, 0, 50, 0, 0, 0, 60, 0,
    };
    uint8_t destination[16];
    struct qq_capture_frame frame = {
        .data = source,
        .size = sizeof(source),
        .width = 3,
        .height = 2,
        .stride = 12,
        .format = SPA_VIDEO_FORMAT_BGRx,
        .crop_x = 1,
        .crop_y = 0,
        .crop_width = 2,
        .crop_height = 2,
        .transform = SPA_META_TRANSFORMATION_None,
    };
    XImage image;

    initialize_ximage(&image, destination, 2, 2);
    if (!qq_frame_copy_to_ximage(&frame, &image))
        return false;

    return pixel_equals(destination, 0, 0, 0, 20) &&
           pixel_equals(destination, 1, 0, 0, 30) &&
           pixel_equals(destination, 2, 0, 0, 50) &&
           pixel_equals(destination, 3, 0, 0, 60);
}

static bool
test_transforms(void)
{
    static const uint8_t source[] = {
        0, 0, 10, 0, 0, 0, 20, 0,
        0, 0, 30, 0, 0, 0, 40, 0,
        0, 0, 50, 0, 0, 0, 60, 0,
    };
    static const enum spa_meta_videotransform_value transforms[] = {
        SPA_META_TRANSFORMATION_None,
        SPA_META_TRANSFORMATION_90,
        SPA_META_TRANSFORMATION_180,
        SPA_META_TRANSFORMATION_270,
        SPA_META_TRANSFORMATION_Flipped,
        SPA_META_TRANSFORMATION_Flipped90,
        SPA_META_TRANSFORMATION_Flipped180,
        SPA_META_TRANSFORMATION_Flipped270,
    };
    static const uint8_t expected[][6] = {
        { 10, 20, 30, 40, 50, 60 },
        { 50, 30, 10, 60, 40, 20 },
        { 60, 50, 40, 30, 20, 10 },
        { 20, 40, 60, 10, 30, 50 },
        { 20, 10, 40, 30, 60, 50 },
        { 10, 30, 50, 20, 40, 60 },
        { 50, 60, 30, 40, 10, 20 },
        { 60, 40, 20, 50, 30, 10 },
    };
    size_t transform_index;

    for (transform_index = 0;
         transform_index < sizeof(transforms) / sizeof(transforms[0]);
         transform_index++) {
        bool swaps_axes =
            transforms[transform_index] == SPA_META_TRANSFORMATION_90 ||
            transforms[transform_index] == SPA_META_TRANSFORMATION_270 ||
            transforms[transform_index] ==
                SPA_META_TRANSFORMATION_Flipped90 ||
            transforms[transform_index] ==
                SPA_META_TRANSFORMATION_Flipped270;
        int output_width = swaps_axes ? 3 : 2;
        int output_height = swaps_axes ? 2 : 3;
        uint8_t destination[sizeof(source)];
        struct qq_capture_frame frame = {
            .data = source,
            .size = sizeof(source),
            .width = 2,
            .height = 3,
            .stride = 8,
            .format = SPA_VIDEO_FORMAT_BGRx,
            .transform = transforms[transform_index],
        };
        XImage image;
        size_t pixel;

        initialize_ximage(&image, destination, output_width, output_height);
        if (!qq_frame_copy_to_ximage(&frame, &image))
            return false;
        for (pixel = 0; pixel < 6; pixel++) {
            if (!pixel_equals(destination, pixel, 0, 0,
                              expected[transform_index][pixel])) {
                return false;
            }
        }
    }

    return true;
}

static bool
test_invalid_frame_returns_black(void)
{
    static const uint8_t source[] = { 0xff, 0xff, 0xff, 0xff };
    uint8_t destination[4];
    struct qq_capture_frame frame = {
        .data = source,
        .size = sizeof(source),
        .width = UINT32_MAX,
        .height = 1,
        .stride = UINT32_MAX,
        .format = SPA_VIDEO_FORMAT_BGRx,
        .transform = SPA_META_TRANSFORMATION_90,
    };
    XImage image;

    memset(destination, 0xff, sizeof(destination));
    initialize_ximage(&image, destination, 1, 1);
    if (qq_frame_copy_to_ximage(&frame, &image))
        return false;
    return pixel_equals(destination, 0, 0, 0, 0);
}

static bool
test_black_frame(void)
{
    uint8_t destination[32];
    XImage image;
    size_t index;

    memset(destination, 0xff, sizeof(destination));
    initialize_ximage(&image, destination, 4, 2);
    qq_frame_black_ximage(&image);

    for (index = 0; index < sizeof(destination); index++) {
        if (destination[index] != 0)
            return false;
    }
    return true;
}

int
main(void)
{
    if (!test_bgrx_copy()) {
        fprintf(stderr, "BGRx conversion failed\n");
        return EXIT_FAILURE;
    }
    if (!test_supported_formats()) {
        fprintf(stderr, "supported-format conversion failed\n");
        return EXIT_FAILURE;
    }
    if (!test_aspect_fit_blacks_unused_pixels()) {
        fprintf(stderr, "aspect-fit conversion failed\n");
        return EXIT_FAILURE;
    }
    if (!test_crop()) {
        fprintf(stderr, "crop conversion failed\n");
        return EXIT_FAILURE;
    }
    if (!test_transforms()) {
        fprintf(stderr, "transform conversion failed\n");
        return EXIT_FAILURE;
    }
    if (!test_invalid_frame_returns_black()) {
        fprintf(stderr, "invalid-frame fallback failed\n");
        return EXIT_FAILURE;
    }
    if (!test_black_frame()) {
        fprintf(stderr, "black-frame fallback failed\n");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
