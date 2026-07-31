/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#ifndef QQ_PRELOAD_FRAME_H
#define QQ_PRELOAD_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <X11/Xlib.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/raw.h>

struct qq_capture_frame {
    const uint8_t *data;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    enum spa_video_format format;

    uint32_t crop_x;
    uint32_t crop_y;
    uint32_t crop_width;
    uint32_t crop_height;
    enum spa_meta_videotransform_value transform;
};

void qq_frame_black_ximage(XImage *image);

bool qq_frame_copy_to_ximage(const struct qq_capture_frame *frame,
                             XImage *image);

#endif
