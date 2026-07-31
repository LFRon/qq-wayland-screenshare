/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#ifndef QQ_PRELOAD_CAPTURE_H
#define QQ_PRELOAD_CAPTURE_H

#include <stdbool.h>

#include <X11/Xlib.h>

bool qq_capture_acquire(void);
void qq_capture_release(void);
void qq_capture_force_stop(void);
void qq_capture_copy_to_ximage(XImage *image);

#endif
