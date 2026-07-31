/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#include "screenshot.h"

#include "frame.h"
#include "portal.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dbus/dbus.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/raw.h>

static pthread_mutex_t screenshot_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool shutting_down;

static bool
environment_enabled(const char *name)
{
    const char *value = getenv(name);

    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static bool
debug_enabled(void)
{
    return environment_enabled("QQ_PRELOAD_DEBUG");
}

static void __attribute__((format(printf, 1, 2)))
screenshot_log(const char *format, ...)
{
    va_list args;

    if (!debug_enabled())
        return;

    fprintf(stderr, "[qq-preload:screenshot] ");
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

static DBusMessage *
wait_for_response(DBusConnection *connection, const char *request_path)
{
    DBusMessage *message;

    while (!atomic_load_explicit(&shutting_down, memory_order_acquire)) {
        if (!dbus_connection_read_write(connection, 250))
            break;

        while ((message = dbus_connection_pop_message(connection)) != NULL) {
            const char *message_path = dbus_message_get_path(message);

            if (dbus_message_is_signal(message,
                                       QQ_PORTAL_REQUEST_IFACE,
                                       "Response") &&
                message_path && strcmp(message_path, request_path) == 0) {
                return message;
            }

            dbus_message_unref(message);
        }
    }

    if (atomic_load_explicit(&shutting_down, memory_order_acquire))
        qq_portal_close_request(connection, request_path);
    return NULL;
}

static char *
request_screenshot_uri(void)
{
    DBusConnection *connection = NULL;
    DBusMessage *message = NULL;
    DBusMessage *reply = NULL;
    DBusMessage *response = NULL;
    DBusMessageIter iter;
    DBusMessageIter options;
    DBusMessageIter results;
    char handle_token[64];
    const char *parent_window = "";
    char *request_path = NULL;
    char *uri = NULL;

    connection = qq_portal_connect_session_bus();
    if (!connection)
        goto out;
    if (!qq_portal_register_app_id(connection) ||
        !qq_portal_add_response_match(connection))
        goto out;

    qq_portal_make_token(handle_token, sizeof(handle_token),
                         "qq_screenshot");
    message = dbus_message_new_method_call(QQ_PORTAL_BUS_NAME,
                                           QQ_PORTAL_OBJECT_PATH,
                                           QQ_PORTAL_SCREENSHOT_IFACE,
                                           "Screenshot");
    if (!message)
        goto out;

    dbus_message_iter_init_append(message, &iter);
    if (!dbus_message_iter_append_basic(&iter,
                                        DBUS_TYPE_STRING,
                                        &parent_window) ||
        !dbus_message_iter_open_container(&iter,
                                          DBUS_TYPE_ARRAY,
                                          "{sv}", &options) ||
        !qq_portal_append_dict_string(&options,
                                      "handle_token", handle_token) ||
        !qq_portal_append_dict_bool(&options, "modal", FALSE) ||
        !qq_portal_append_dict_bool(&options, "interactive", FALSE) ||
        !dbus_message_iter_close_container(&iter, &options)) {
        goto out;
    }

    reply = qq_portal_call(connection, message);
    message = NULL;
    if (!reply ||
        !dbus_message_get_args(reply, NULL,
                               DBUS_TYPE_OBJECT_PATH, &request_path,
                               DBUS_TYPE_INVALID) ||
        !request_path) {
        goto out;
    }

    response = wait_for_response(connection, request_path);
    if (!response ||
        atomic_load_explicit(&shutting_down, memory_order_acquire))
        goto out;
    if (qq_portal_response_success(response, &results, NULL))
        uri = qq_portal_response_lookup_string(&results, "uri");

out:
    if (message)
        dbus_message_unref(message);
    if (response)
        dbus_message_unref(response);
    if (reply)
        dbus_message_unref(reply);
    qq_portal_disconnect(connection);
    return uri;
}

bool
qq_screenshot_copy_uri_to_ximage(const char *uri, XImage *image)
{
    GFile *file = NULL;
    GFileInputStream *stream = NULL;
    GdkPixbuf *pixbuf = NULL;
    GdkPixbuf *rgba = NULL;
    GError *error = NULL;
    struct qq_capture_frame frame;
    bool copied = false;

    qq_frame_black_ximage(image);
    if (!uri || !image || image->width <= 0 || image->height <= 0 ||
        image->width > 16384 || image->height > 16384)
        return false;

    file = g_file_new_for_uri(uri);
    if (!file)
        goto out;
    stream = g_file_read(file, NULL, &error);
    if (!stream)
        goto out;

    pixbuf = gdk_pixbuf_new_from_stream_at_scale(
        G_INPUT_STREAM(stream), image->width, image->height,
        FALSE, NULL, &error);
    if (!pixbuf)
        goto out;

    if (gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB ||
        gdk_pixbuf_get_bits_per_sample(pixbuf) != 8 ||
        gdk_pixbuf_get_width(pixbuf) != image->width ||
        gdk_pixbuf_get_height(pixbuf) != image->height)
        goto out;

    if (gdk_pixbuf_get_has_alpha(pixbuf)) {
        rgba = g_object_ref(pixbuf);
    } else {
        rgba = gdk_pixbuf_add_alpha(pixbuf, FALSE, 0, 0, 0);
    }
    if (!rgba || gdk_pixbuf_get_n_channels(rgba) != 4)
        goto out;

    memset(&frame, 0, sizeof(frame));
    frame.data = gdk_pixbuf_get_pixels(rgba);
    frame.size = gdk_pixbuf_get_byte_length(rgba);
    frame.width = (uint32_t) gdk_pixbuf_get_width(rgba);
    frame.height = (uint32_t) gdk_pixbuf_get_height(rgba);
    frame.stride = (uint32_t) gdk_pixbuf_get_rowstride(rgba);
    frame.format = SPA_VIDEO_FORMAT_RGBA;
    frame.transform = SPA_META_TRANSFORMATION_None;
    copied = qq_frame_copy_to_ximage(&frame, image);

out:
    if (!copied)
        qq_frame_black_ximage(image);
    if (!copied && error)
        screenshot_log("could not decode portal screenshot: %s",
                       error->message);
    g_clear_error(&error);
    if (rgba)
        g_object_unref(rgba);
    if (pixbuf)
        g_object_unref(pixbuf);
    if (stream)
        g_object_unref(stream);
    if (file)
        g_object_unref(file);
    return copied;
}

bool
qq_screenshot_copy_to_ximage(XImage *image)
{
    char *uri = NULL;
    bool copied = false;

    qq_frame_black_ximage(image);
    if (environment_enabled("QQ_PRELOAD_DISABLE_PORTAL") ||
        atomic_load_explicit(&shutting_down, memory_order_acquire))
        return false;

    pthread_mutex_lock(&screenshot_mutex);
    if (!atomic_load_explicit(&shutting_down, memory_order_acquire)) {
        uri = request_screenshot_uri();
        if (uri)
            copied = qq_screenshot_copy_uri_to_ximage(uri, image);
    }
    pthread_mutex_unlock(&screenshot_mutex);

    screenshot_log("portal screenshot %s",
                   copied ? "completed" : "returned black");
    free(uri);
    return copied;
}

void
qq_screenshot_shutdown(void)
{
    atomic_store_explicit(&shutting_down, true, memory_order_release);

    pthread_mutex_lock(&screenshot_mutex);
    pthread_mutex_unlock(&screenshot_mutex);
}
