/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#include "frame.h"

#include <limits.h>
#include <string.h>

static bool
ximage_data_size(const XImage *image, size_t *size_out)
{
    size_t stride;
    size_t height;

    if (!image || !image->data || image->bytes_per_line <= 0 ||
        image->height <= 0)
        return false;

    stride = (size_t) image->bytes_per_line;
    height = (size_t) image->height;
    if (height > SIZE_MAX / stride)
        return false;

    *size_out = stride * height;
    return true;
}

void
qq_frame_black_ximage(XImage *image)
{
    size_t size;

    if (ximage_data_size(image, &size))
        memset(image->data, 0, size);
}

static unsigned int
mask_shift(unsigned long mask)
{
    unsigned int shift = 0;

    if (mask == 0)
        return 0;

    while ((mask & 1UL) == 0) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

static unsigned long
pack_channel(unsigned long mask, uint8_t value)
{
    unsigned int shift;
    unsigned long maximum;
    uint64_t scaled;

    if (mask == 0)
        return 0;

    shift = mask_shift(mask);
    maximum = mask >> shift;
    scaled = ((uint64_t) value * maximum + 127U) / 255U;
    return ((unsigned long) scaled << shift) & mask;
}

static bool
ximage_write_pixel(XImage *image, int x, int y,
                   uint8_t red, uint8_t green, uint8_t blue)
{
    unsigned long pixel;
    size_t bit_offset;
    size_t byte_offset;
    uint8_t *dst;
    int bytes_per_pixel;
    int i;

    if (x < 0 || y < 0 || x >= image->width || y >= image->height)
        return false;
    if (image->bits_per_pixel != 16 &&
        image->bits_per_pixel != 24 &&
        image->bits_per_pixel != 32)
        return false;
    if (image->xoffset < 0)
        return false;

    bit_offset = (size_t) (image->xoffset + x) *
                 (size_t) image->bits_per_pixel;
    if ((bit_offset & 7U) != 0)
        return false;

    byte_offset = bit_offset / 8U;
    bytes_per_pixel = image->bits_per_pixel / 8;
    if (byte_offset + (size_t) bytes_per_pixel >
        (size_t) image->bytes_per_line)
        return false;

    pixel = pack_channel(image->red_mask, red) |
            pack_channel(image->green_mask, green) |
            pack_channel(image->blue_mask, blue);
    dst = (uint8_t *) image->data +
          (size_t) y * (size_t) image->bytes_per_line + byte_offset;

    if (image->byte_order == LSBFirst) {
        for (i = 0; i < bytes_per_pixel; i++)
            dst[i] = (uint8_t) (pixel >> (i * CHAR_BIT));
    } else {
        for (i = 0; i < bytes_per_pixel; i++)
            dst[i] = (uint8_t)
                (pixel >> ((bytes_per_pixel - i - 1) * CHAR_BIT));
    }

    return true;
}

static bool
frame_get_rgb(const struct qq_capture_frame *frame,
              uint32_t x, uint32_t y,
              uint8_t *red, uint8_t *green, uint8_t *blue)
{
    const uint8_t *pixel;
    size_t offset;

    if (x >= frame->width || y >= frame->height)
        return false;

    offset = (size_t) y * frame->stride + (size_t) x * 4U;
    if (offset > frame->size || frame->size - offset < 4U)
        return false;
    pixel = frame->data + offset;

    switch (frame->format) {
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA:
        *blue = pixel[0];
        *green = pixel[1];
        *red = pixel[2];
        return true;
    case SPA_VIDEO_FORMAT_RGBx:
    case SPA_VIDEO_FORMAT_RGBA:
        *red = pixel[0];
        *green = pixel[1];
        *blue = pixel[2];
        return true;
    default:
        return false;
    }
}

static void
transformed_size(enum spa_meta_videotransform_value transform,
                 uint32_t width, uint32_t height,
                 uint32_t *out_width, uint32_t *out_height)
{
    switch (transform) {
    case SPA_META_TRANSFORMATION_90:
    case SPA_META_TRANSFORMATION_270:
    case SPA_META_TRANSFORMATION_Flipped90:
    case SPA_META_TRANSFORMATION_Flipped270:
        *out_width = height;
        *out_height = width;
        break;
    default:
        *out_width = width;
        *out_height = height;
        break;
    }
}

static bool
map_transformed_pixel(enum spa_meta_videotransform_value transform,
                      uint32_t width, uint32_t height,
                      uint32_t x, uint32_t y,
                      uint32_t *source_x, uint32_t *source_y)
{
    switch (transform) {
    case SPA_META_TRANSFORMATION_None:
        *source_x = x;
        *source_y = y;
        break;
    case SPA_META_TRANSFORMATION_90:
        *source_x = y;
        *source_y = height - x - 1U;
        break;
    case SPA_META_TRANSFORMATION_180:
        *source_x = width - x - 1U;
        *source_y = height - y - 1U;
        break;
    case SPA_META_TRANSFORMATION_270:
        *source_x = width - y - 1U;
        *source_y = x;
        break;
    case SPA_META_TRANSFORMATION_Flipped:
        *source_x = width - x - 1U;
        *source_y = y;
        break;
    case SPA_META_TRANSFORMATION_Flipped90:
        *source_x = y;
        *source_y = x;
        break;
    case SPA_META_TRANSFORMATION_Flipped180:
        *source_x = x;
        *source_y = height - y - 1U;
        break;
    case SPA_META_TRANSFORMATION_Flipped270:
        *source_x = width - y - 1U;
        *source_y = height - x - 1U;
        break;
    default:
        return false;
    }

    return *source_x < width && *source_y < height;
}

static bool
normalize_crop(const struct qq_capture_frame *frame,
               uint32_t *crop_x, uint32_t *crop_y,
               uint32_t *crop_width, uint32_t *crop_height)
{
    uint64_t right;
    uint64_t bottom;

    *crop_x = frame->crop_x;
    *crop_y = frame->crop_y;
    *crop_width = frame->crop_width;
    *crop_height = frame->crop_height;

    if (*crop_width == 0 || *crop_height == 0) {
        *crop_x = 0;
        *crop_y = 0;
        *crop_width = frame->width;
        *crop_height = frame->height;
    }

    right = (uint64_t) *crop_x + *crop_width;
    bottom = (uint64_t) *crop_y + *crop_height;
    return *crop_x < frame->width &&
           *crop_y < frame->height &&
           right <= frame->width &&
           bottom <= frame->height;
}

bool
qq_frame_copy_to_ximage(const struct qq_capture_frame *frame,
                        XImage *image)
{
    uint32_t crop_x;
    uint32_t crop_y;
    uint32_t crop_width;
    uint32_t crop_height;
    uint32_t visual_width;
    uint32_t visual_height;
    uint32_t fit_width;
    uint32_t fit_height;
    uint32_t offset_x;
    uint32_t offset_y;
    uint32_t dy;
    uint64_t required;

    qq_frame_black_ximage(image);

    if (!frame || !frame->data || !image || !image->data ||
        frame->width == 0 || frame->height == 0 ||
        frame->width > 16384U || frame->height > 16384U ||
        (uint64_t) frame->stride < (uint64_t) frame->width * 4U ||
        image->width <= 0 || image->height <= 0 ||
        image->bytes_per_line <= 0)
        return false;

    required = (uint64_t) (frame->height - 1U) * frame->stride +
               (uint64_t) frame->width * 4U;
    if (required > frame->size)
        return false;

    if (!normalize_crop(frame, &crop_x, &crop_y,
                        &crop_width, &crop_height))
        return false;

    transformed_size(frame->transform, crop_width, crop_height,
                     &visual_width, &visual_height);
    if (visual_width == 0 || visual_height == 0)
        return false;

    if ((uint64_t) image->width * visual_height <=
        (uint64_t) image->height * visual_width) {
        fit_width = (uint32_t) image->width;
        fit_height = (uint32_t)
            ((uint64_t) fit_width * visual_height / visual_width);
    } else {
        fit_height = (uint32_t) image->height;
        fit_width = (uint32_t)
            ((uint64_t) fit_height * visual_width / visual_height);
    }
    if (fit_width == 0 || fit_height == 0)
        return false;

    offset_x = ((uint32_t) image->width - fit_width) / 2U;
    offset_y = ((uint32_t) image->height - fit_height) / 2U;

    for (dy = 0; dy < fit_height; dy++) {
        uint32_t visual_y =
            (uint32_t) ((uint64_t) dy * visual_height / fit_height);
        uint32_t dx;

        for (dx = 0; dx < fit_width; dx++) {
            uint32_t visual_x =
                (uint32_t) ((uint64_t) dx * visual_width / fit_width);
            uint32_t source_x;
            uint32_t source_y;
            uint8_t red;
            uint8_t green;
            uint8_t blue;

            if (!map_transformed_pixel(frame->transform,
                                       crop_width, crop_height,
                                       visual_x, visual_y,
                                       &source_x, &source_y))
                return false;
            source_x += crop_x;
            source_y += crop_y;

            if (!frame_get_rgb(frame, source_x, source_y,
                               &red, &green, &blue))
                return false;
            if (!ximage_write_pixel(image,
                                    (int) (offset_x + dx),
                                    (int) (offset_y + dy),
                                    red, green, blue))
                return false;
        }
    }

    return true;
}
