/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#ifndef QQ_PRELOAD_SCREENSHOT_H
#define QQ_PRELOAD_SCREENSHOT_H

#include <stdbool.h>

#include <X11/Xlib.h>

bool qq_screenshot_copy_to_ximage(XImage *image);

bool qq_screenshot_copy_uri_to_ximage(const char *uri, XImage *image);

void qq_screenshot_shutdown(void);

#endif
