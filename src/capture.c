/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#include "capture.h"
#include "frame.h"
#include "portal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/vararg.h>
#include <spa/utils/result.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define PORTAL_SCREENCAST_SOURCE_MONITOR 1U
#define PORTAL_SCREENCAST_CURSOR_HIDDEN 1U
#define PORTAL_SCREENCAST_CURSOR_EMBEDDED 2U
#define PORTAL_SCREENCAST_PERSIST_MODE_PERSISTENT 2U

#define PORTAL_CLOSE_TIMEOUT_MS 500
#define FRAME_BYTES_PER_PIXEL 4U
#define PIPEWIRE_BUFFER_TYPES \
    ((1U << SPA_DATA_MemPtr) | (1U << SPA_DATA_MemFd))

struct capture_session;

struct capture_stream {
    struct capture_session *session;

    uint32_t node_id;

    struct pw_stream *pw_stream;
    struct spa_hook listener;
    struct spa_video_info_raw raw;

    uint8_t *frame;
    size_t frame_size;
    uint32_t frame_width;
    uint32_t frame_height;
    uint32_t frame_stride;
    enum spa_video_format frame_format;
    uint32_t crop_x;
    uint32_t crop_y;
    uint32_t crop_width;
    uint32_t crop_height;
    enum spa_meta_videotransform_value transform;
    bool frame_valid;
    bool logged_bad_buffer;
};

struct capture_session {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;

    unsigned int users;
    bool started;
    bool thread_finished;
    bool stop;
    bool failed;
    bool have_frame;
    const char *stage;

    DBusConnection *connection;
    bool response_subscribed;
    char *session_handle;
    char *restore_token;

    struct pw_thread_loop *pw_loop;
    struct pw_context *pw_context;
    struct pw_core *pw_core;

    struct capture_stream *streams;
    uint32_t n_streams;
};

static struct capture_session capture = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .stage = "idle",
};
static pthread_mutex_t lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;

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
capture_log(const char *format, ...)
{
    va_list args;

    if (!debug_enabled())
        return;

    fprintf(stderr, "[qq-preload] ");
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

static void __attribute__((format(printf, 1, 2)))
capture_error(const char *format, ...)
{
    va_list args;

    fprintf(stderr, "[qq-preload] ");
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

static bool
write_full(int fd, const void *data, size_t size)
{
    const uint8_t *position = data;

    while (size > 0) {
        ssize_t written = write(fd, position, size);

        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (written == 0)
            return false;

        position += written;
        size -= (size_t) written;
    }

    return true;
}

static bool
pread_full(int fd, void *data, size_t size, off_t offset)
{
    uint8_t *position = data;

    while (size > 0) {
        ssize_t bytes = pread(fd, position, size, offset);

        if (bytes < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (bytes == 0)
            return false;

        position += bytes;
        size -= (size_t) bytes;
        offset += bytes;
    }

    return true;
}

static bool
mkdir_p(const char *path, mode_t mode)
{
    char copy[PATH_MAX];
    size_t length;
    char *position;

    if (!path || path[0] != '/')
        return false;
    if (snprintf(copy, sizeof(copy), "%s", path) >= (int) sizeof(copy))
        return false;

    length = strlen(copy);
    if (length == 0)
        return false;
    if (copy[length - 1] == '/')
        copy[length - 1] = '\0';

    for (position = copy + 1; *position; position++) {
        if (*position != '/')
            continue;

        *position = '\0';
        if (mkdir(copy, mode) < 0 && errno != EEXIST)
            return false;
        *position = '/';
    }

    return mkdir(copy, mode) == 0 || errno == EEXIST;
}

static bool
restore_token_is_valid(const char *token)
{
    size_t i;

    if (!token || strlen(token) != 36)
        return false;

    for (i = 0; i < 36; i++) {
        const char c = token[i];

        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return false;
        } else if (!((c >= '0' && c <= '9') ||
                     (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }

    return true;
}

static bool
restore_token_path(char *path, size_t size)
{
    const char *xdg_state_home = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    char directory[PATH_MAX];
    int length;

    if (xdg_state_home && xdg_state_home[0] == '/') {
        length = snprintf(directory, sizeof(directory),
                          "%s/qq-xwayland-screencast", xdg_state_home);
    } else if (home && home[0] == '/') {
        length = snprintf(directory, sizeof(directory),
                          "%s/.local/state/qq-xwayland-screencast", home);
    } else {
        return false;
    }
    if (length < 0 || (size_t) length >= sizeof(directory))
        return false;
    if (!mkdir_p(directory, 0700))
        return false;

    length = snprintf(path, size, "%s/restore-token", directory);
    return length >= 0 && (size_t) length < size;
}

static void
delete_restore_token(void)
{
    char path[PATH_MAX];

    if (restore_token_path(path, sizeof(path)))
        unlink(path);
}

static char *
load_restore_token(void)
{
    char path[PATH_MAX];
    char buffer[128];
    ssize_t length;
    int fd;

    if (environment_enabled("QQ_PRELOAD_DISABLE_PERSISTENCE"))
        return NULL;
    if (!restore_token_path(path, sizeof(path)))
        return NULL;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    length = read(fd, buffer, sizeof(buffer) - 1U);
    close(fd);
    if (length <= 0)
        return NULL;

    while (length > 0 &&
           (buffer[length - 1] == '\n' ||
            buffer[length - 1] == '\r' ||
            buffer[length - 1] == ' ' ||
            buffer[length - 1] == '\t')) {
        length--;
    }
    buffer[length] = '\0';

    if (!restore_token_is_valid(buffer)) {
        unlink(path);
        return NULL;
    }

    return strdup(buffer);
}

static bool
save_restore_token(const char *token)
{
    char path[PATH_MAX];
    char contents[64];
    int length;
    int fd;
    bool ok;

    if (environment_enabled("QQ_PRELOAD_DISABLE_PERSISTENCE"))
        return false;
    if (!restore_token_is_valid(token))
        return false;
    if (!restore_token_path(path, sizeof(path)))
        return false;

    length = snprintf(contents, sizeof(contents), "%s\n", token);
    if (length < 0 || (size_t) length >= sizeof(contents))
        return false;

    fd = open(path,
              O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
              0600);
    if (fd < 0)
        return false;
    ok = write_full(fd, contents, (size_t) length);
    close(fd);
    if (!ok)
        unlink(path);
    return ok;
}

static bool
session_stopped(struct capture_session *session)
{
    bool stopped;

    pthread_mutex_lock(&session->mutex);
    stopped = session->stop;
    pthread_mutex_unlock(&session->mutex);
    return stopped;
}

static void
session_set_stage(struct capture_session *session, const char *stage)
{
    pthread_mutex_lock(&session->mutex);
    session->stage = stage;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mutex);
    capture_log("stage: %s", stage);
}

static void
session_set_failed(struct capture_session *session)
{
    pthread_mutex_lock(&session->mutex);
    session->failed = true;
    session->have_frame = false;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mutex);
}

static DBusMessage *
portal_wait_response(struct capture_session *session, const char *path)
{
    DBusConnection *connection = session->connection;
    DBusMessage *message;

    while (!session_stopped(session)) {
        if (!dbus_connection_read_write(connection, 250))
            break;

        while ((message = dbus_connection_pop_message(connection)) != NULL) {
            const char *message_path = dbus_message_get_path(message);

            if (dbus_message_is_signal(message,
                                       QQ_PORTAL_REQUEST_IFACE,
                                       "Response") &&
                message_path && strcmp(message_path, path) == 0) {
                return message;
            }

            dbus_message_unref(message);
        }
    }

    if (session_stopped(session))
        qq_portal_close_request(connection, path);
    return NULL;
}

static uint32_t
portal_get_cursor_modes(DBusConnection *connection)
{
    DBusMessage *message;
    DBusMessage *reply;
    DBusMessageIter iter;
    DBusMessageIter variant;
    const char *interface_name = QQ_PORTAL_SCREENCAST_IFACE;
    const char *property_name = "AvailableCursorModes";
    uint32_t modes = 0;

    message = dbus_message_new_method_call(QQ_PORTAL_BUS_NAME,
                                           QQ_PORTAL_OBJECT_PATH,
                                           QQ_PORTAL_PROPERTIES_IFACE,
                                           "Get");
    if (!message)
        return 0;

    dbus_message_iter_init_append(message, &iter);
    dbus_message_iter_append_basic(&iter,
                                   DBUS_TYPE_STRING,
                                   &interface_name);
    dbus_message_iter_append_basic(&iter,
                                   DBUS_TYPE_STRING,
                                   &property_name);

    reply = qq_portal_call(connection, message);
    if (!reply)
        return 0;

    if (dbus_message_iter_init(reply, &iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&iter, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32)
            dbus_message_iter_get_basic(&variant, &modes);
    }
    dbus_message_unref(reply);
    return modes;
}

static char *
portal_create_session(struct capture_session *session)
{
    DBusMessage *message;
    DBusMessage *reply;
    DBusMessage *response;
    DBusMessageIter iter;
    DBusMessageIter options;
    DBusMessageIter results;
    char handle_token[64];
    char session_token[64];
    char *request_path = NULL;
    char *session_handle = NULL;

    qq_portal_make_token(handle_token, sizeof(handle_token), "qq_sc_create");
    qq_portal_make_token(session_token, sizeof(session_token),
                         "qq_sc_session");

    message = dbus_message_new_method_call(QQ_PORTAL_BUS_NAME,
                                           QQ_PORTAL_OBJECT_PATH,
                                           QQ_PORTAL_SCREENCAST_IFACE,
                                           "CreateSession");
    if (!message)
        return NULL;

    dbus_message_iter_init_append(message, &iter);
    if (!dbus_message_iter_open_container(&iter,
                                          DBUS_TYPE_ARRAY,
                                          "{sv}", &options))
        goto fail;
    if (!qq_portal_append_dict_string(&options,
                                      "handle_token", handle_token) ||
        !qq_portal_append_dict_string(&options,
                                      "session_handle_token", session_token))
        goto fail;
    if (!dbus_message_iter_close_container(&iter, &options))
        goto fail;

    reply = qq_portal_call(session->connection, message);
    if (!reply)
        return NULL;
    if (!dbus_message_get_args(reply, NULL,
                               DBUS_TYPE_OBJECT_PATH, &request_path,
                               DBUS_TYPE_INVALID) ||
        !request_path) {
        dbus_message_unref(reply);
        return NULL;
    }

    response = portal_wait_response(session, request_path);
    dbus_message_unref(reply);
    if (!response)
        return NULL;

    if (qq_portal_response_success(response, &results, NULL))
        session_handle =
            qq_portal_response_lookup_string(&results, "session_handle");
    dbus_message_unref(response);
    return session_handle;

fail:
    dbus_message_unref(message);
    return NULL;
}

static bool
portal_select_sources(struct capture_session *session)
{
    DBusMessage *message;
    DBusMessage *reply;
    DBusMessage *response;
    DBusMessageIter iter;
    DBusMessageIter options;
    DBusMessageIter results;
    char handle_token[64];
    const char *session_handle = session->session_handle;
    char *request_path = NULL;
    uint32_t cursor_modes;
    bool ok;

    qq_portal_make_token(handle_token, sizeof(handle_token), "qq_sc_select");
    cursor_modes = portal_get_cursor_modes(session->connection);

    message = dbus_message_new_method_call(QQ_PORTAL_BUS_NAME,
                                           QQ_PORTAL_OBJECT_PATH,
                                           QQ_PORTAL_SCREENCAST_IFACE,
                                           "SelectSources");
    if (!message)
        return false;

    dbus_message_iter_init_append(message, &iter);
    if (!dbus_message_iter_append_basic(&iter,
                                        DBUS_TYPE_OBJECT_PATH,
                                        &session_handle))
        goto fail;
    if (!dbus_message_iter_open_container(&iter,
                                          DBUS_TYPE_ARRAY,
                                          "{sv}", &options))
        goto fail;
    if (!qq_portal_append_dict_string(&options,
                                      "handle_token", handle_token) ||
        !qq_portal_append_dict_uint32(&options, "types",
                                      PORTAL_SCREENCAST_SOURCE_MONITOR) ||
        !qq_portal_append_dict_bool(&options, "multiple", FALSE))
        goto fail;

    if (!environment_enabled("QQ_PRELOAD_DISABLE_PERSISTENCE")) {
        if (!qq_portal_append_dict_uint32(
                &options, "persist_mode",
                PORTAL_SCREENCAST_PERSIST_MODE_PERSISTENT))
            goto fail;
        if (session->restore_token &&
            restore_token_is_valid(session->restore_token) &&
            !qq_portal_append_dict_string(&options, "restore_token",
                                          session->restore_token))
            goto fail;
    }

    if (cursor_modes & PORTAL_SCREENCAST_CURSOR_EMBEDDED) {
        if (!qq_portal_append_dict_uint32(
                &options, "cursor_mode",
                PORTAL_SCREENCAST_CURSOR_EMBEDDED))
            goto fail;
    } else if (cursor_modes & PORTAL_SCREENCAST_CURSOR_HIDDEN) {
        if (!qq_portal_append_dict_uint32(
                &options, "cursor_mode",
                PORTAL_SCREENCAST_CURSOR_HIDDEN))
            goto fail;
    }
    if (!dbus_message_iter_close_container(&iter, &options))
        goto fail;

    reply = qq_portal_call(session->connection, message);
    if (!reply)
        return false;
    if (!dbus_message_get_args(reply, NULL,
                               DBUS_TYPE_OBJECT_PATH, &request_path,
                               DBUS_TYPE_INVALID) ||
        !request_path) {
        dbus_message_unref(reply);
        return false;
    }

    response = portal_wait_response(session, request_path);
    dbus_message_unref(reply);
    if (!response)
        return false;
    ok = qq_portal_response_success(response, &results, NULL);
    dbus_message_unref(response);
    return ok;

fail:
    dbus_message_unref(message);
    return false;
}

static bool
parse_streams(DBusMessageIter *results, struct capture_session *session)
{
    DBusMessageIter dictionary;

    dbus_message_iter_recurse(results, &dictionary);
    while (dbus_message_iter_get_arg_type(&dictionary) ==
           DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        DBusMessageIter variant;
        DBusMessageIter array;
        const char *key;

        dbus_message_iter_recurse(&dictionary, &entry);
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
            goto next;
        dbus_message_iter_get_basic(&entry, &key);
        if (strcmp(key, "streams") != 0)
            goto next;
        if (!dbus_message_iter_next(&entry) ||
            dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
            goto next;

        dbus_message_iter_recurse(&entry, &variant);
        if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY)
            goto next;

        dbus_message_iter_recurse(&variant, &array);
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRUCT) {
            DBusMessageIter tuple;
            struct capture_stream *streams;
            struct capture_stream *stream;
            uint32_t node_id;

            dbus_message_iter_recurse(&array, &tuple);
            if (dbus_message_iter_get_arg_type(&tuple) != DBUS_TYPE_UINT32)
                goto stream_next;
            dbus_message_iter_get_basic(&tuple, &node_id);

            streams = reallocarray(session->streams,
                                   session->n_streams + 1U,
                                   sizeof(*session->streams));
            if (!streams)
                return false;
            session->streams = streams;
            stream = &session->streams[session->n_streams++];
            memset(stream, 0, sizeof(*stream));
            stream->session = session;
            stream->node_id = node_id;
            stream->transform = SPA_META_TRANSFORMATION_None;

stream_next:
            dbus_message_iter_next(&array);
        }

        return session->n_streams > 0;

next:
        dbus_message_iter_next(&dictionary);
    }

    return false;
}

static bool
portal_start(struct capture_session *session)
{
    DBusMessage *message;
    DBusMessage *reply;
    DBusMessage *response;
    DBusMessageIter iter;
    DBusMessageIter options;
    DBusMessageIter results;
    char handle_token[64];
    const char *session_handle = session->session_handle;
    const char *parent_window = "";
    char *request_path = NULL;
    char *restore_token = NULL;
    bool ok = false;

    qq_portal_make_token(handle_token, sizeof(handle_token), "qq_sc_start");

    message = dbus_message_new_method_call(QQ_PORTAL_BUS_NAME,
                                           QQ_PORTAL_OBJECT_PATH,
                                           QQ_PORTAL_SCREENCAST_IFACE,
                                           "Start");
    if (!message)
        return false;

    dbus_message_iter_init_append(message, &iter);
    if (!dbus_message_iter_append_basic(&iter,
                                        DBUS_TYPE_OBJECT_PATH,
                                        &session_handle) ||
        !dbus_message_iter_append_basic(&iter,
                                        DBUS_TYPE_STRING,
                                        &parent_window))
        goto fail;
    if (!dbus_message_iter_open_container(&iter,
                                          DBUS_TYPE_ARRAY,
                                          "{sv}", &options))
        goto fail;
    if (!qq_portal_append_dict_string(&options,
                                      "handle_token", handle_token))
        goto fail;
    if (!dbus_message_iter_close_container(&iter, &options))
        goto fail;

    reply = qq_portal_call(session->connection, message);
    if (!reply)
        return false;
    if (!dbus_message_get_args(reply, NULL,
                               DBUS_TYPE_OBJECT_PATH, &request_path,
                               DBUS_TYPE_INVALID) ||
        !request_path) {
        dbus_message_unref(reply);
        return false;
    }

    response = portal_wait_response(session, request_path);
    dbus_message_unref(reply);
    if (!response)
        return false;

    if (qq_portal_response_success(response, &results, NULL)) {
        ok = parse_streams(&results, session);
        if (ok)
            restore_token =
                qq_portal_response_lookup_string(&results, "restore_token");
    }

    if (ok && !environment_enabled("QQ_PRELOAD_DISABLE_PERSISTENCE")) {
        if (restore_token && save_restore_token(restore_token)) {
            free(session->restore_token);
            session->restore_token = restore_token;
            restore_token = NULL;
        } else if (!restore_token && session->restore_token) {
            delete_restore_token();
            free(session->restore_token);
            session->restore_token = NULL;
        }
    }

    free(restore_token);
    dbus_message_unref(response);
    return ok;

fail:
    dbus_message_unref(message);
    return false;
}

static int
portal_open_pipewire_remote(struct capture_session *session)
{
    DBusMessage *message;
    DBusMessage *reply;
    DBusMessageIter iter;
    DBusMessageIter options;
    const char *session_handle = session->session_handle;
    int fd = -1;

    message = dbus_message_new_method_call(QQ_PORTAL_BUS_NAME,
                                           QQ_PORTAL_OBJECT_PATH,
                                           QQ_PORTAL_SCREENCAST_IFACE,
                                           "OpenPipeWireRemote");
    if (!message)
        return -1;

    dbus_message_iter_init_append(message, &iter);
    if (!dbus_message_iter_append_basic(&iter,
                                        DBUS_TYPE_OBJECT_PATH,
                                        &session_handle))
        goto fail;
    if (!dbus_message_iter_open_container(&iter,
                                          DBUS_TYPE_ARRAY,
                                          "{sv}", &options))
        goto fail;
    if (!dbus_message_iter_close_container(&iter, &options))
        goto fail;

    reply = qq_portal_call(session->connection, message);
    if (!reply)
        return -1;
    if (!dbus_message_get_args(reply, NULL,
                               DBUS_TYPE_UNIX_FD, &fd,
                               DBUS_TYPE_INVALID)) {
        fd = -1;
    }
    dbus_message_unref(reply);
    return fd;

fail:
    dbus_message_unref(message);
    return -1;
}

static const struct spa_pod *
build_buffers(struct spa_pod_builder *builder);

static const struct spa_pod *
build_meta(struct spa_pod_builder *builder, uint32_t type, uint32_t size);

static void
stream_param_changed(void *data, uint32_t id, const struct spa_pod *parameter)
{
    struct capture_stream *stream = data;
    struct spa_video_info_raw raw;
    const struct spa_pod *parameters[3];
    uint8_t buffer[2048];
    struct spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    int result;

    if (!parameter || id != SPA_PARAM_Format)
        return;

    memset(&raw, 0, sizeof(raw));
    if (spa_format_video_raw_parse(parameter, &raw) < 0 ||
        raw.size.width == 0 || raw.size.height == 0 ||
        raw.size.width > 16384U || raw.size.height > 16384U ||
        (raw.format != SPA_VIDEO_FORMAT_BGRx &&
         raw.format != SPA_VIDEO_FORMAT_BGRA &&
         raw.format != SPA_VIDEO_FORMAT_RGBx &&
         raw.format != SPA_VIDEO_FORMAT_RGBA)) {
        pw_stream_set_error(stream->pw_stream, -EINVAL,
                            "unsupported raw video format");
        return;
    }
    stream->raw = raw;

    parameters[0] = build_buffers(&builder);
    parameters[1] = build_meta(&builder,
                               SPA_META_VideoCrop,
                               sizeof(struct spa_meta_region));
    parameters[2] = build_meta(&builder,
                               SPA_META_VideoTransform,
                               sizeof(struct spa_meta_videotransform));
    result = pw_stream_update_params(stream->pw_stream,
                                     parameters,
                                     ARRAY_SIZE(parameters));
    if (result < 0) {
        capture_error("could not configure PipeWire stream %u: %s",
                      stream->node_id, spa_strerror(result));
        pw_stream_set_error(stream->pw_stream, result,
                            "could not configure video buffers");
    }
}

static void
stream_state_changed(void *data,
                     enum pw_stream_state old_state,
                     enum pw_stream_state state,
                     const char *error)
{
    struct capture_stream *stream = data;

    capture_log("PipeWire stream %u: %s -> %s%s%s",
                stream->node_id,
                pw_stream_state_as_string(old_state),
                pw_stream_state_as_string(state),
                error ? ": " : "",
                error ? error : "");

    if (state == PW_STREAM_STATE_ERROR) {
        pthread_mutex_lock(&stream->session->mutex);
        stream->frame_valid = false;
        stream->session->have_frame = false;
        stream->session->failed = true;
        pthread_cond_broadcast(&stream->session->cond);
        pthread_mutex_unlock(&stream->session->mutex);
    }
}

static bool
copy_pipewire_rows(const struct spa_data *data,
                   size_t offset,
                   size_t row_bytes,
                   size_t stride,
                   uint32_t height,
                   uint8_t *destination)
{
    uint32_t row;

    if (data->data) {
        const uint8_t *source = (const uint8_t *) data->data + offset;

        for (row = 0; row < height; row++) {
            memcpy(destination + (size_t) row * stride,
                   source + (size_t) row * stride,
                   row_bytes);
        }
        return true;
    }

    if (data->type != SPA_DATA_MemFd || data->fd < 0)
        return false;

    for (row = 0; row < height; row++) {
        uint64_t file_offset = (uint64_t) data->mapoffset + offset +
                               (uint64_t) row * stride;

        if (file_offset > INT64_MAX ||
            !pread_full(data->fd,
                        destination + (size_t) row * stride,
                        row_bytes,
                        (off_t) file_offset)) {
            return false;
        }
    }

    return true;
}

static void
stream_process(void *data)
{
    struct capture_stream *stream = data;
    struct capture_session *session = stream->session;
    struct pw_buffer *pw_buffer;
    struct spa_buffer *buffer;
    struct spa_data *spa_data;
    struct spa_chunk *chunk;
    struct spa_meta_region *crop;
    struct spa_meta_videotransform *transform;
    uint32_t width;
    uint32_t height;
    size_t stride;
    size_t row_bytes;
    size_t allocation_size;
    size_t needed;
    size_t offset;
    size_t available;
    uint8_t *frame;
    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    uint32_t crop_width = 0;
    uint32_t crop_height = 0;
    enum spa_meta_videotransform_value transform_value =
        SPA_META_TRANSFORMATION_None;

    pw_buffer = pw_stream_dequeue_buffer(stream->pw_stream);
    if (!pw_buffer)
        return;

    buffer = pw_buffer->buffer;
    if (!buffer || buffer->n_datas < 1)
        goto out;
    spa_data = &buffer->datas[0];
    chunk = spa_data->chunk;
    if (!chunk)
        goto out;
    if (chunk->size == 0)
        goto out;
    if (chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)
        goto bad_buffer;

    width = stream->raw.size.width;
    height = stream->raw.size.height;
    if (width == 0 || height == 0 ||
        width > 16384U || height > 16384U)
        goto out;

    if (chunk->stride < 0)
        goto bad_buffer;
    stride = chunk->stride == 0 ?
        (size_t) width * FRAME_BYTES_PER_PIXEL :
        (size_t) chunk->stride;
    row_bytes = (size_t) width * FRAME_BYTES_PER_PIXEL;
    if (stride < row_bytes || spa_data->maxsize == 0)
        goto bad_buffer;
    if ((size_t) height > SIZE_MAX / stride)
        goto bad_buffer;

    allocation_size = stride * (size_t) height;
    needed = stride * (size_t) (height - 1U) + row_bytes;
    if (chunk->offset >= spa_data->maxsize)
        goto bad_buffer;
    offset = chunk->offset;
    available = spa_data->maxsize - offset;
    if (chunk->size < available)
        available = chunk->size;
    if (available < needed)
        goto bad_buffer;

    frame = calloc(1, allocation_size);
    if (!frame)
        goto out;
    if (!copy_pipewire_rows(spa_data, offset, row_bytes, stride,
                            height, frame)) {
        free(frame);
        goto bad_buffer;
    }

    crop = spa_buffer_find_meta_data(buffer,
                                     SPA_META_VideoCrop,
                                     sizeof(*crop));
    if (crop && spa_meta_region_is_valid(crop) &&
        crop->region.position.x >= 0 &&
        crop->region.position.y >= 0) {
        uint64_t right =
            (uint64_t) crop->region.position.x + crop->region.size.width;
        uint64_t bottom =
            (uint64_t) crop->region.position.y + crop->region.size.height;

        if (right <= width && bottom <= height) {
            crop_x = (uint32_t) crop->region.position.x;
            crop_y = (uint32_t) crop->region.position.y;
            crop_width = crop->region.size.width;
            crop_height = crop->region.size.height;
        }
    }

    transform = spa_buffer_find_meta_data(buffer,
                                          SPA_META_VideoTransform,
                                          sizeof(*transform));
    if (transform &&
        transform->transform <= SPA_META_TRANSFORMATION_Flipped270) {
        transform_value =
            (enum spa_meta_videotransform_value) transform->transform;
    }

    pthread_mutex_lock(&session->mutex);
    free(stream->frame);
    stream->frame = frame;
    stream->frame_size = allocation_size;
    stream->frame_width = width;
    stream->frame_height = height;
    stream->frame_stride = (uint32_t) stride;
    stream->frame_format = stream->raw.format;
    stream->crop_x = crop_x;
    stream->crop_y = crop_y;
    stream->crop_width = crop_width;
    stream->crop_height = crop_height;
    stream->transform = transform_value;
    stream->frame_valid = true;
    stream->logged_bad_buffer = false;
    session->have_frame = true;
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mutex);
    goto out;

bad_buffer:
    if (!stream->logged_bad_buffer) {
        capture_error("unsupported or short PipeWire buffer for stream %u",
                      stream->node_id);
        stream->logged_bad_buffer = true;
    }

out:
    pw_stream_queue_buffer(stream->pw_stream, pw_buffer);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = stream_state_changed,
    .param_changed = stream_param_changed,
    .process = stream_process,
};

static const struct spa_pod *
build_format(struct spa_pod_builder *builder, enum spa_video_format format)
{
    struct spa_rectangle default_size = SPA_RECTANGLE(1920, 1080);
    struct spa_rectangle minimum_size = SPA_RECTANGLE(1, 1);
    struct spa_rectangle maximum_size = SPA_RECTANGLE(16384, 16384);
    struct spa_fraction default_rate = SPA_FRACTION(30, 1);
    struct spa_fraction minimum_rate = SPA_FRACTION(0, 1);
    struct spa_fraction maximum_rate = SPA_FRACTION(120, 1);

    return spa_pod_builder_add_object(
        builder,
        SPA_TYPE_OBJECT_Format,
        SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,
        SPA_POD_Id(format),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&default_size,
                                       &minimum_size,
                                       &maximum_size),
        SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(&default_rate,
                                      &minimum_rate,
                                      &maximum_rate));
}

static const struct spa_pod *
build_buffers(struct spa_pod_builder *builder)
{
    return spa_pod_builder_add_object(
        builder,
        SPA_TYPE_OBJECT_ParamBuffers,
        SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers,
        SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
        SPA_PARAM_BUFFERS_blocks,
        SPA_POD_CHOICE_RANGE_Int(0, 1, INT32_MAX),
        SPA_PARAM_BUFFERS_size,
        SPA_POD_CHOICE_RANGE_Int(0, 1, INT32_MAX),
        SPA_PARAM_BUFFERS_stride,
        SPA_POD_CHOICE_RANGE_Int(0, 0, INT32_MAX),
        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int(PIPEWIRE_BUFFER_TYPES));
}

static const struct spa_pod *
build_meta(struct spa_pod_builder *builder, uint32_t type, uint32_t size)
{
    return spa_pod_builder_add_object(builder,
                                      SPA_TYPE_OBJECT_ParamMeta,
                                      SPA_PARAM_Meta,
                                      SPA_PARAM_META_type,
                                      SPA_POD_Id(type),
                                      SPA_PARAM_META_size,
                                      SPA_POD_Int(size));
}

static bool
pipewire_connect_streams(struct capture_session *session, int fd)
{
    uint32_t index;
    bool ok = false;

    pw_init(NULL, NULL);

    session->pw_loop =
        pw_thread_loop_new("qq-xwayland-screencast", NULL);
    if (!session->pw_loop)
        goto fail_fd;

    session->pw_context =
        pw_context_new(pw_thread_loop_get_loop(session->pw_loop), NULL, 0);
    if (!session->pw_context)
        goto fail_fd;

    session->pw_core =
        pw_context_connect_fd(session->pw_context, fd, NULL, 0);
    if (!session->pw_core)
        return false;
    fd = -1;

    if (pw_thread_loop_start(session->pw_loop) < 0)
        return false;

    pw_thread_loop_lock(session->pw_loop);
    for (index = 0; index < session->n_streams; index++) {
        struct capture_stream *stream = &session->streams[index];
        struct pw_properties *properties;
        const struct spa_pod *parameters[4];
        uint8_t buffer[4096];
        struct spa_pod_builder builder =
            SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        int result;

        properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                       PW_KEY_MEDIA_CATEGORY, "Capture",
                                       PW_KEY_MEDIA_ROLE, "Screen",
                                       NULL);
        if (!properties)
            goto unlock;

        stream->pw_stream =
            pw_stream_new(session->pw_core,
                          "qq-xwayland-screencast",
                          properties);
        if (!stream->pw_stream)
            goto unlock;
        pw_stream_add_listener(stream->pw_stream,
                               &stream->listener,
                               &stream_events,
                               stream);

        parameters[0] = build_format(&builder, SPA_VIDEO_FORMAT_BGRx);
        parameters[1] = build_format(&builder, SPA_VIDEO_FORMAT_BGRA);
        parameters[2] = build_format(&builder, SPA_VIDEO_FORMAT_RGBx);
        parameters[3] = build_format(&builder, SPA_VIDEO_FORMAT_RGBA);

        result = pw_stream_connect(
            stream->pw_stream,
            PW_DIRECTION_INPUT,
            stream->node_id,
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
            parameters,
            ARRAY_SIZE(parameters));
        if (result < 0) {
            capture_error("could not connect PipeWire stream %u: %s",
                          stream->node_id, spa_strerror(result));
            goto unlock;
        }
        pw_stream_set_active(stream->pw_stream, true);
    }
    ok = true;

unlock:
    pw_thread_loop_unlock(session->pw_loop);
    return ok;

fail_fd:
    if (fd >= 0)
        close(fd);
    return false;
}

static void
pipewire_cleanup(struct capture_session *session)
{
    uint32_t index;

    if (session->pw_loop)
        pw_thread_loop_stop(session->pw_loop);

    for (index = 0; index < session->n_streams; index++) {
        if (session->streams[index].pw_stream)
            pw_stream_destroy(session->streams[index].pw_stream);
    }
    if (session->pw_core)
        pw_core_disconnect(session->pw_core);
    if (session->pw_context)
        pw_context_destroy(session->pw_context);
    if (session->pw_loop)
        pw_thread_loop_destroy(session->pw_loop);

    session->pw_core = NULL;
    session->pw_context = NULL;
    session->pw_loop = NULL;

    pthread_mutex_lock(&session->mutex);
    for (index = 0; index < session->n_streams; index++)
        free(session->streams[index].frame);
    free(session->streams);
    session->streams = NULL;
    session->n_streams = 0;
    session->have_frame = false;
    pthread_mutex_unlock(&session->mutex);
}

static bool
portal_connect(struct capture_session *session)
{
    int fd;

    session_set_stage(session, "connecting to session bus");
    session->connection = qq_portal_connect_session_bus();
    if (!session->connection)
        return false;
    if (session_stopped(session))
        return false;

    session_set_stage(session, "registering QQ portal identity");
    if (!qq_portal_register_app_id(session->connection) ||
        session_stopped(session))
        return false;

    session_set_stage(session, "subscribing to portal responses");
    session->response_subscribed =
        qq_portal_add_response_match(session->connection);
    if (!session->response_subscribed || session_stopped(session))
        return false;

    session_set_stage(session, "creating portal session");
    session->session_handle = portal_create_session(session);
    if (!session->session_handle || session_stopped(session))
        return false;

    session_set_stage(session, "selecting portal source");
    if (!portal_select_sources(session) || session_stopped(session))
        return false;

    session_set_stage(session, "starting portal session");
    if (!portal_start(session) || session_stopped(session))
        return false;

    session_set_stage(session, "opening PipeWire remote");
    fd = portal_open_pipewire_remote(session);
    if (fd < 0)
        return false;
    if (session_stopped(session)) {
        close(fd);
        return false;
    }

    session_set_stage(session, "connecting PipeWire stream");
    if (!pipewire_connect_streams(session, fd))
        return false;

    session_set_stage(session, "streaming");
    return true;
}

static void
portal_cleanup(struct capture_session *session)
{
    if (session->connection && session->session_handle) {
        DBusMessage *message;
        DBusMessage *reply;
        const char *session_handle = session->session_handle;

        message = dbus_message_new_method_call(QQ_PORTAL_BUS_NAME,
                                               session_handle,
                                               QQ_PORTAL_SESSION_IFACE,
                                               "Close");
        if (message) {
            reply = qq_portal_call_with_timeout(session->connection,
                                                message,
                                                PORTAL_CLOSE_TIMEOUT_MS);
            if (reply)
                dbus_message_unref(reply);
        }
    }

    free(session->session_handle);
    session->session_handle = NULL;

    session->response_subscribed = false;
    qq_portal_disconnect(session->connection);
    session->connection = NULL;
}

static void *
capture_thread(void *data)
{
    struct capture_session *session = data;
    bool connected;

    connected = portal_connect(session);
    if (!connected && !session_stopped(session))
        session_set_failed(session);

    if (connected) {
        pthread_mutex_lock(&session->mutex);
        while (!session->stop)
            pthread_cond_wait(&session->cond, &session->mutex);
        pthread_mutex_unlock(&session->mutex);
    }

    portal_cleanup(session);
    pipewire_cleanup(session);

    pthread_mutex_lock(&session->mutex);
    session->thread_finished = true;
    session->stage = "stopped";
    pthread_cond_broadcast(&session->cond);
    pthread_mutex_unlock(&session->mutex);
    return NULL;
}

bool
qq_capture_acquire(void)
{
    bool first_user;
    bool ok = true;
    unsigned int users;

    pthread_mutex_lock(&lifecycle_mutex);
    pthread_mutex_lock(&capture.mutex);
    first_user = capture.users == 0;
    capture.users++;
    users = capture.users;

    if (first_user && !environment_enabled("QQ_PRELOAD_DISABLE_PORTAL")) {
        capture.stop = false;
        capture.failed = false;
        capture.have_frame = false;
        capture.thread_finished = false;
        capture.stage = "starting";
        free(capture.restore_token);
        capture.restore_token = load_restore_token();

        if (!qq_portal_initialize_threads()) {
            capture.failed = true;
            capture.stage = "D-Bus thread initialization failed";
            ok = false;
        } else if (pthread_create(&capture.thread,
                                  NULL,
                                  capture_thread,
                                  &capture) != 0) {
            capture.failed = true;
            capture.stage = "thread creation failed";
            ok = false;
        } else {
            capture.started = true;
        }
    }

    pthread_mutex_unlock(&capture.mutex);
    pthread_mutex_unlock(&lifecycle_mutex);

    capture_log("capture acquire: users=%u", users);
    return ok;
}

static void
stop_last_capture_user(bool force)
{
    pthread_t thread;
    bool join = false;

    pthread_mutex_lock(&lifecycle_mutex);
    pthread_mutex_lock(&capture.mutex);

    if (force) {
        capture.users = 0;
    } else if (capture.users > 0) {
        capture.users--;
    }

    if (capture.users == 0 && capture.started) {
        capture.stop = true;
        pthread_cond_broadcast(&capture.cond);
        thread = capture.thread;
        join = true;
    }
    pthread_mutex_unlock(&capture.mutex);

    if (join)
        pthread_join(thread, NULL);

    pthread_mutex_lock(&capture.mutex);
    if (capture.users == 0) {
        capture.started = false;
        capture.thread_finished = false;
        capture.stop = false;
        capture.failed = false;
        capture.have_frame = false;
        capture.stage = "idle";
        free(capture.restore_token);
        capture.restore_token = NULL;
    }
    pthread_mutex_unlock(&capture.mutex);
    pthread_mutex_unlock(&lifecycle_mutex);
}

void
qq_capture_release(void)
{
    stop_last_capture_user(false);
    capture_log("capture release");
}

void
qq_capture_force_stop(void)
{
    stop_last_capture_user(true);
}

void
qq_capture_copy_to_ximage(XImage *image)
{
    uint32_t index;
    bool copied = false;

    pthread_mutex_lock(&capture.mutex);
    if (!capture.failed && capture.have_frame) {
        for (index = 0; index < capture.n_streams; index++) {
            struct capture_stream *stream = &capture.streams[index];
            struct qq_capture_frame frame;

            if (!stream->frame_valid || !stream->frame)
                continue;

            memset(&frame, 0, sizeof(frame));
            frame.data = stream->frame;
            frame.size = stream->frame_size;
            frame.width = stream->frame_width;
            frame.height = stream->frame_height;
            frame.stride = stream->frame_stride;
            frame.format = stream->frame_format;
            frame.crop_x = stream->crop_x;
            frame.crop_y = stream->crop_y;
            frame.crop_width = stream->crop_width;
            frame.crop_height = stream->crop_height;
            frame.transform = stream->transform;

            copied = qq_frame_copy_to_ximage(&frame, image);
            if (copied)
                break;
        }
    }

    if (!copied)
        qq_frame_black_ximage(image);
    pthread_mutex_unlock(&capture.mutex);
}
