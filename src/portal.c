/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#include "portal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DBUS_SERVICE_DBUS "org.freedesktop.DBus"
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"
#define DBUS_INTERFACE_DBUS "org.freedesktop.DBus"

static pthread_once_t dbus_threads_once = PTHREAD_ONCE_INIT;
static bool dbus_threads_ready;
static atomic_uint token_serial;

static bool
debug_enabled(void)
{
    const char *value = getenv("QQ_PRELOAD_DEBUG");

    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void __attribute__((format(printf, 1, 2)))
portal_log(const char *format, ...)
{
    va_list args;

    if (!debug_enabled())
        return;

    fprintf(stderr, "[qq-preload:portal] ");
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

static void __attribute__((format(printf, 1, 2)))
portal_error(const char *format, ...)
{
    va_list args;

    fprintf(stderr, "[qq-preload:portal] ");
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
mkdir_p(const char *path, mode_t mode)
{
    char copy[PATH_MAX];
    char *position;
    size_t length;

    if (!path || path[0] != '/')
        return false;
    length = strlen(path);
    if (length == 0 || length >= sizeof(copy))
        return false;

    memcpy(copy, path, length + 1U);
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
ensure_desktop_file(void)
{
    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    char applications_dir[PATH_MAX];
    char desktop_path[PATH_MAX];
    static const char contents[] =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=QQ\n"
        "Exec=true\n"
        "NoDisplay=true\n"
        "X-QQ-Xwayland-Portal-Preload=true\n";
    int length;
    int fd;
    bool ok;

    if (xdg_data_home && xdg_data_home[0] == '/') {
        length = snprintf(applications_dir, sizeof(applications_dir),
                          "%s/applications", xdg_data_home);
    } else if (home && home[0] == '/') {
        length = snprintf(applications_dir, sizeof(applications_dir),
                          "%s/.local/share/applications", home);
    } else {
        return false;
    }
    if (length < 0 || (size_t) length >= sizeof(applications_dir))
        return false;
    if (!mkdir_p(applications_dir, 0700))
        return false;

    length = snprintf(desktop_path, sizeof(desktop_path),
                      "%s/%s.desktop", applications_dir, QQ_PORTAL_APP_ID);
    if (length < 0 || (size_t) length >= sizeof(desktop_path))
        return false;
    if (access(desktop_path, F_OK) == 0)
        return true;

    fd = open(desktop_path,
              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
              0644);
    if (fd < 0)
        return errno == EEXIST;

    ok = write_full(fd, contents, sizeof(contents) - 1U);
    close(fd);
    if (!ok)
        unlink(desktop_path);
    return ok;
}

static void
initialize_dbus_threads_once(void)
{
    dbus_threads_ready = dbus_threads_init_default();
}

bool
qq_portal_initialize_threads(void)
{
    pthread_once(&dbus_threads_once, initialize_dbus_threads_once);
    return dbus_threads_ready;
}

DBusConnection *
qq_portal_connect_session_bus(void)
{
    DBusConnection *connection;
    DBusError error;

    if (!qq_portal_initialize_threads()) {
        portal_error("could not initialize libdbus threading");
        return NULL;
    }

    dbus_error_init(&error);
    connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!connection) {
        if (dbus_error_is_set(&error)) {
            portal_error("could not connect to session bus: %s",
                         error.message);
            dbus_error_free(&error);
        }
        return NULL;
    }

    dbus_connection_set_exit_on_disconnect(connection, false);
    return connection;
}

void
qq_portal_disconnect(DBusConnection *connection)
{
    if (!connection)
        return;

    dbus_connection_close(connection);
    dbus_connection_unref(connection);
}

DBusMessage *
qq_portal_call_with_timeout(DBusConnection *connection,
                            DBusMessage *message,
                            int timeout_ms)
{
    DBusError error;
    DBusMessage *reply;

    if (!connection || !message) {
        if (message)
            dbus_message_unref(message);
        return NULL;
    }

    dbus_error_init(&error);
    reply = dbus_connection_send_with_reply_and_block(connection,
                                                      message,
                                                      timeout_ms,
                                                      &error);
    dbus_message_unref(message);
    if (dbus_error_is_set(&error)) {
        portal_error("portal D-Bus call failed: %s", error.message);
        dbus_error_free(&error);
    }

    return reply;
}

DBusMessage *
qq_portal_call(DBusConnection *connection, DBusMessage *message)
{
    return qq_portal_call_with_timeout(connection, message,
                                       QQ_PORTAL_CALL_TIMEOUT_MS);
}

bool
qq_portal_register_app_id(DBusConnection *connection)
{
    DBusMessage *message;
    DBusMessage *reply;
    DBusMessageIter iter;
    DBusMessageIter options;
    const char *app_id = QQ_PORTAL_APP_ID;

    if (!ensure_desktop_file())
        portal_error("could not prepare %s.desktop", QQ_PORTAL_APP_ID);

    message = dbus_message_new_method_call(QQ_PORTAL_BUS_NAME,
                                           QQ_PORTAL_OBJECT_PATH,
                                           QQ_PORTAL_HOST_REGISTRY_IFACE,
                                           "Register");
    if (!message)
        return false;

    dbus_message_iter_init_append(message, &iter);
    if (!dbus_message_iter_append_basic(&iter,
                                        DBUS_TYPE_STRING,
                                        &app_id))
        goto fail;
    if (!dbus_message_iter_open_container(&iter,
                                          DBUS_TYPE_ARRAY,
                                          "{sv}", &options))
        goto fail;
    if (!dbus_message_iter_close_container(&iter, &options))
        goto fail;

    reply = qq_portal_call(connection, message);
    if (!reply)
        return false;
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *name = dbus_message_get_error_name(reply);

        portal_error("portal rejected app id %s: %s",
                     QQ_PORTAL_APP_ID,
                     name ? name : "unknown D-Bus error");
        dbus_message_unref(reply);
        return false;
    }

    dbus_message_unref(reply);
    return true;

fail:
    dbus_message_unref(message);
    return false;
}

bool
qq_portal_add_response_match(DBusConnection *connection)
{
    static const char rule[] =
        "type='signal',interface='" QQ_PORTAL_REQUEST_IFACE
        "',member='Response'";
    DBusMessage *message;
    DBusMessage *reply;
    const char *rule_value = rule;

    message = dbus_message_new_method_call(DBUS_SERVICE_DBUS,
                                           DBUS_PATH_DBUS,
                                           DBUS_INTERFACE_DBUS,
                                           "AddMatch");
    if (!message)
        return false;
    if (!dbus_message_append_args(message,
                                  DBUS_TYPE_STRING, &rule_value,
                                  DBUS_TYPE_INVALID)) {
        dbus_message_unref(message);
        return false;
    }

    reply = qq_portal_call(connection, message);
    if (!reply)
        return false;
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        const char *name = dbus_message_get_error_name(reply);

        portal_error("could not subscribe to portal responses: %s",
                     name ? name : "unknown D-Bus error");
        dbus_message_unref(reply);
        return false;
    }

    dbus_message_unref(reply);
    return true;
}

void
qq_portal_close_request(DBusConnection *connection, const char *path)
{
    DBusMessage *message;
    dbus_uint32_t serial = 0;

    if (!connection || !path)
        return;

    message = dbus_message_new_method_call(QQ_PORTAL_BUS_NAME,
                                           path,
                                           QQ_PORTAL_REQUEST_IFACE,
                                           "Close");
    if (!message)
        return;

    dbus_connection_send(connection, message, &serial);
    dbus_message_unref(message);
}

void
qq_portal_make_token(char *buffer, size_t size, const char *prefix)
{
    unsigned int serial =
        atomic_fetch_add_explicit(&token_serial, 1U,
                                  memory_order_relaxed) + 1U;

    if (!buffer || size == 0 || !prefix)
        return;
    snprintf(buffer, size, "%s_%u_%u",
             prefix, (unsigned int) getpid(), serial);
}

bool
qq_portal_append_dict_uint32(DBusMessageIter *dictionary,
                             const char *key, uint32_t value)
{
    DBusMessageIter entry;
    DBusMessageIter variant;
    const char *key_value = key;

    if (!dbus_message_iter_open_container(dictionary,
                                          DBUS_TYPE_DICT_ENTRY,
                                          NULL, &entry))
        return false;
    if (!dbus_message_iter_append_basic(&entry,
                                        DBUS_TYPE_STRING,
                                        &key_value))
        return false;
    if (!dbus_message_iter_open_container(&entry,
                                          DBUS_TYPE_VARIANT,
                                          "u", &variant))
        return false;
    if (!dbus_message_iter_append_basic(&variant,
                                        DBUS_TYPE_UINT32,
                                        &value))
        return false;
    if (!dbus_message_iter_close_container(&entry, &variant))
        return false;
    return dbus_message_iter_close_container(dictionary, &entry);
}

bool
qq_portal_append_dict_bool(DBusMessageIter *dictionary,
                           const char *key, dbus_bool_t value)
{
    DBusMessageIter entry;
    DBusMessageIter variant;
    const char *key_value = key;

    if (!dbus_message_iter_open_container(dictionary,
                                          DBUS_TYPE_DICT_ENTRY,
                                          NULL, &entry))
        return false;
    if (!dbus_message_iter_append_basic(&entry,
                                        DBUS_TYPE_STRING,
                                        &key_value))
        return false;
    if (!dbus_message_iter_open_container(&entry,
                                          DBUS_TYPE_VARIANT,
                                          "b", &variant))
        return false;
    if (!dbus_message_iter_append_basic(&variant,
                                        DBUS_TYPE_BOOLEAN,
                                        &value))
        return false;
    if (!dbus_message_iter_close_container(&entry, &variant))
        return false;
    return dbus_message_iter_close_container(dictionary, &entry);
}

bool
qq_portal_append_dict_string(DBusMessageIter *dictionary,
                             const char *key, const char *value)
{
    DBusMessageIter entry;
    DBusMessageIter variant;
    const char *key_value = key;
    const char *string_value = value;

    if (!dbus_message_iter_open_container(dictionary,
                                          DBUS_TYPE_DICT_ENTRY,
                                          NULL, &entry))
        return false;
    if (!dbus_message_iter_append_basic(&entry,
                                        DBUS_TYPE_STRING,
                                        &key_value))
        return false;
    if (!dbus_message_iter_open_container(&entry,
                                          DBUS_TYPE_VARIANT,
                                          "s", &variant))
        return false;
    if (!dbus_message_iter_append_basic(&variant,
                                        DBUS_TYPE_STRING,
                                        &string_value))
        return false;
    if (!dbus_message_iter_close_container(&entry, &variant))
        return false;
    return dbus_message_iter_close_container(dictionary, &entry);
}

bool
qq_portal_response_success(DBusMessage *message,
                           DBusMessageIter *results,
                           uint32_t *response_out)
{
    DBusMessageIter iter;
    uint32_t response;

    if (response_out)
        *response_out = UINT32_MAX;
    if (!message || !results ||
        !dbus_message_iter_init(message, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_UINT32)
        return false;

    dbus_message_iter_get_basic(&iter, &response);
    if (response_out)
        *response_out = response;
    if (response != 0) {
        portal_log("portal request returned response %u", response);
        return false;
    }

    if (!dbus_message_iter_next(&iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
        return false;

    *results = iter;
    return true;
}

char *
qq_portal_response_lookup_string(DBusMessageIter *results,
                                 const char *wanted_key)
{
    DBusMessageIter dictionary;

    if (!results || !wanted_key)
        return NULL;

    dbus_message_iter_recurse(results, &dictionary);
    while (dbus_message_iter_get_arg_type(&dictionary) ==
           DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        DBusMessageIter variant;
        const char *key;
        int value_type;

        dbus_message_iter_recurse(&dictionary, &entry);
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_STRING)
            goto next;
        dbus_message_iter_get_basic(&entry, &key);
        if (strcmp(key, wanted_key) != 0)
            goto next;
        if (!dbus_message_iter_next(&entry) ||
            dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT)
            goto next;

        dbus_message_iter_recurse(&entry, &variant);
        value_type = dbus_message_iter_get_arg_type(&variant);
        if (value_type == DBUS_TYPE_STRING ||
            value_type == DBUS_TYPE_OBJECT_PATH) {
            const char *value;

            dbus_message_iter_get_basic(&variant, &value);
            return strdup(value);
        }

next:
        dbus_message_iter_next(&dictionary);
    }

    return NULL;
}
