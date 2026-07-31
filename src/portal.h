/*
 * Copyright (C) 2026
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation.
 */

#ifndef QQ_PRELOAD_PORTAL_H
#define QQ_PRELOAD_PORTAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dbus/dbus.h>

#define QQ_PORTAL_APP_ID "qq.xwayland"
#define QQ_PORTAL_APP_NAME "QQ"

#define QQ_PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define QQ_PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define QQ_PORTAL_SCREENCAST_IFACE "org.freedesktop.portal.ScreenCast"
#define QQ_PORTAL_SCREENSHOT_IFACE "org.freedesktop.portal.Screenshot"
#define QQ_PORTAL_REQUEST_IFACE "org.freedesktop.portal.Request"
#define QQ_PORTAL_SESSION_IFACE "org.freedesktop.portal.Session"
#define QQ_PORTAL_PROPERTIES_IFACE "org.freedesktop.DBus.Properties"
#define QQ_PORTAL_HOST_REGISTRY_IFACE \
    "org.freedesktop.host.portal.Registry"

#define QQ_PORTAL_CALL_TIMEOUT_MS 3000

bool qq_portal_initialize_threads(void);

DBusConnection *qq_portal_connect_session_bus(void);
void qq_portal_disconnect(DBusConnection *connection);

bool qq_portal_register_app_id(DBusConnection *connection);
bool qq_portal_add_response_match(DBusConnection *connection);

DBusMessage *qq_portal_call_with_timeout(DBusConnection *connection,
                                         DBusMessage *message,
                                         int timeout_ms);
DBusMessage *qq_portal_call(DBusConnection *connection,
                            DBusMessage *message);

void qq_portal_close_request(DBusConnection *connection, const char *path);
void qq_portal_make_token(char *buffer, size_t size, const char *prefix);

bool qq_portal_append_dict_uint32(DBusMessageIter *dictionary,
                                  const char *key, uint32_t value);
bool qq_portal_append_dict_bool(DBusMessageIter *dictionary,
                                const char *key, dbus_bool_t value);
bool qq_portal_append_dict_string(DBusMessageIter *dictionary,
                                  const char *key, const char *value);

bool qq_portal_response_success(DBusMessage *message,
                                DBusMessageIter *results,
                                uint32_t *response_out);
char *qq_portal_response_lookup_string(DBusMessageIter *results,
                                       const char *wanted_key);

#endif
