#include "portal.h"
#include "screenshot.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib.h>

#define DBUS_BUS_NAME "org.freedesktop.DBus"
#define DBUS_BUS_PATH "/org/freedesktop/DBus"
#define DBUS_BUS_IFACE "org.freedesktop.DBus"

static const char portal_xml[] =
    "<node>"
    " <interface name='org.freedesktop.host.portal.Registry'>"
    "  <method name='Register'>"
    "   <arg type='s' direction='in'/>"
    "   <arg type='a{sv}' direction='in'/>"
    "  </method>"
    " </interface>"
    " <interface name='org.freedesktop.portal.Screenshot'>"
    "  <method name='Screenshot'>"
    "   <arg type='s' direction='in'/>"
    "   <arg type='a{sv}' direction='in'/>"
    "   <arg type='o' direction='out'/>"
    "  </method>"
    " </interface>"
    "</node>";

struct fake_portal {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    bool ready;
    bool failed;

    GMainContext *context;
    GMainLoop *loop;
    const char *uri;

    atomic_uint response;
    atomic_uint screenshot_calls;
    atomic_bool request_valid;
};

static void
mark_server_ready(struct fake_portal *portal, bool failed)
{
    pthread_mutex_lock(&portal->mutex);
    portal->failed = failed;
    portal->ready = true;
    pthread_cond_broadcast(&portal->cond);
    pthread_mutex_unlock(&portal->mutex);
}

static void
handle_method_call(GDBusConnection *connection,
                   const gchar *sender,
                   const gchar *object_path,
                   const gchar *interface_name,
                   const gchar *method_name,
                   GVariant *parameters,
                   GDBusMethodInvocation *invocation,
                   gpointer user_data)
{
    struct fake_portal *portal = user_data;

    (void) sender;
    (void) object_path;

    if (strcmp(interface_name, QQ_PORTAL_HOST_REGISTRY_IFACE) == 0 &&
        strcmp(method_name, "Register") == 0) {
        const char *app_id;
        GVariant *options;

        g_variant_get(parameters, "(&s@a{sv})", &app_id, &options);
        if (strcmp(app_id, QQ_PORTAL_APP_ID) != 0)
            atomic_store(&portal->request_valid, false);
        g_variant_unref(options);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (strcmp(interface_name, QQ_PORTAL_SCREENSHOT_IFACE) == 0 &&
        strcmp(method_name, "Screenshot") == 0) {
        GVariantBuilder results;
        GVariant *options;
        GError *error = NULL;
        const char *parent_window;
        const char *handle_token = NULL;
        char request_path[128];
        gboolean interactive = TRUE;
        gboolean modal = TRUE;
        unsigned int call;
        unsigned int response;

        g_variant_get(parameters, "(&s@a{sv})", &parent_window, &options);
        if (parent_window[0] != '\0' ||
            !g_variant_lookup(options, "handle_token", "&s",
                              &handle_token) ||
            !handle_token || handle_token[0] == '\0' ||
            !g_variant_lookup(options, "interactive", "b",
                              &interactive) ||
            !g_variant_lookup(options, "modal", "b", &modal) ||
            interactive || modal) {
            atomic_store(&portal->request_valid, false);
        }
        g_variant_unref(options);

        call = atomic_fetch_add(&portal->screenshot_calls, 1U) + 1U;
        snprintf(request_path, sizeof(request_path),
                 "/org/freedesktop/portal/desktop/request/fake/%u", call);
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(o)", request_path));

        response = atomic_load(&portal->response);
        g_variant_builder_init(&results, G_VARIANT_TYPE_VARDICT);
        if (response == 0)
            g_variant_builder_add(&results, "{sv}", "uri",
                                  g_variant_new_string(portal->uri));
        if (!g_dbus_connection_emit_signal(
                connection, NULL, request_path,
                QQ_PORTAL_REQUEST_IFACE, "Response",
                g_variant_new("(u@a{sv})", response,
                              g_variant_builder_end(&results)),
                &error)) {
            atomic_store(&portal->request_valid, false);
            g_clear_error(&error);
        }
        return;
    }

    atomic_store(&portal->request_valid, false);
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod",
        "unexpected method");
}

static const GDBusInterfaceVTable portal_vtable = {
    .method_call = handle_method_call,
};

static void *
fake_portal_thread(void *data)
{
    struct fake_portal *portal = data;
    GDBusConnection *connection = NULL;
    GDBusNodeInfo *node_info = NULL;
    GVariant *reply = NULL;
    GError *error = NULL;
    guint registry_id = 0;
    guint screenshot_id = 0;
    guint32 request_name_result = 0;
    bool failed = true;

    portal->context = g_main_context_new();
    g_main_context_push_thread_default(portal->context);
    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!connection)
        goto ready;

    reply = g_dbus_connection_call_sync(
        connection,
        DBUS_BUS_NAME, DBUS_BUS_PATH, DBUS_BUS_IFACE, "RequestName",
        g_variant_new("(su)", QQ_PORTAL_BUS_NAME, 0U),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000,
        NULL, &error);
    if (!reply)
        goto ready;
    g_variant_get(reply, "(u)", &request_name_result);
    g_variant_unref(reply);
    reply = NULL;
    if (request_name_result != 1U && request_name_result != 4U)
        goto ready;

    node_info = g_dbus_node_info_new_for_xml(portal_xml, &error);
    if (!node_info)
        goto ready;
    registry_id = g_dbus_connection_register_object(
        connection, QQ_PORTAL_OBJECT_PATH, node_info->interfaces[0],
        &portal_vtable, portal, NULL, &error);
    if (registry_id == 0)
        goto ready;
    screenshot_id = g_dbus_connection_register_object(
        connection, QQ_PORTAL_OBJECT_PATH, node_info->interfaces[1],
        &portal_vtable, portal, NULL, &error);
    if (screenshot_id == 0)
        goto ready;

    portal->loop = g_main_loop_new(portal->context, FALSE);
    failed = false;

ready:
    if (failed && error) {
        fprintf(stderr, "fake portal setup failed: %s\n", error->message);
        g_clear_error(&error);
    }
    mark_server_ready(portal, failed);
    if (!failed)
        g_main_loop_run(portal->loop);

    if (screenshot_id != 0)
        g_dbus_connection_unregister_object(connection, screenshot_id);
    if (registry_id != 0)
        g_dbus_connection_unregister_object(connection, registry_id);
    if (portal->loop)
        g_main_loop_unref(portal->loop);
    if (node_info)
        g_dbus_node_info_unref(node_info);
    if (reply)
        g_variant_unref(reply);
    if (connection)
        g_object_unref(connection);
    g_clear_error(&error);
    g_main_context_pop_thread_default(portal->context);
    g_main_context_unref(portal->context);
    return NULL;
}

static gboolean
quit_fake_portal(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

static bool
start_fake_portal(struct fake_portal *portal, const char *uri)
{
    memset(portal, 0, sizeof(*portal));
    pthread_mutex_init(&portal->mutex, NULL);
    pthread_cond_init(&portal->cond, NULL);
    portal->uri = uri;
    atomic_init(&portal->response, 0U);
    atomic_init(&portal->screenshot_calls, 0U);
    atomic_init(&portal->request_valid, true);

    if (pthread_create(&portal->thread, NULL,
                       fake_portal_thread, portal) != 0) {
        pthread_cond_destroy(&portal->cond);
        pthread_mutex_destroy(&portal->mutex);
        return false;
    }

    pthread_mutex_lock(&portal->mutex);
    while (!portal->ready)
        pthread_cond_wait(&portal->cond, &portal->mutex);
    pthread_mutex_unlock(&portal->mutex);
    if (portal->failed) {
        pthread_join(portal->thread, NULL);
        pthread_cond_destroy(&portal->cond);
        pthread_mutex_destroy(&portal->mutex);
        return false;
    }
    return true;
}

static void
stop_fake_portal(struct fake_portal *portal)
{
    g_main_context_invoke(portal->context,
                          quit_fake_portal, portal->loop);
    pthread_join(portal->thread, NULL);
    pthread_cond_destroy(&portal->cond);
    pthread_mutex_destroy(&portal->mutex);
}

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

static char *
create_png_fixture(char **path_out)
{
    static const uint8_t colors[][3] = {
        { 255, 0, 0 }, { 0, 255, 0 },
        { 0, 0, 255 }, { 255, 255, 255 },
    };
    GdkPixbuf *pixbuf = NULL;
    GError *error = NULL;
    char *path = NULL;
    char *uri = NULL;
    uint8_t *pixels;
    int rowstride;
    int fd;
    size_t index;

    *path_out = NULL;
    fd = g_file_open_tmp("qq-preload-portal-XXXXXX", &path, &error);
    if (fd < 0)
        goto out;
    close(fd);

    pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 2, 2);
    if (!pixbuf)
        goto out;
    pixels = gdk_pixbuf_get_pixels(pixbuf);
    rowstride = gdk_pixbuf_get_rowstride(pixbuf);
    for (index = 0; index < 4; index++) {
        int x = (int) (index % 2U);
        int y = (int) (index / 2U);

        memcpy(pixels + y * rowstride + x * 3, colors[index], 3);
    }
    if (!gdk_pixbuf_save(pixbuf, path, "png", &error, NULL))
        goto out;
    uri = g_filename_to_uri(path, NULL, &error);
    if (uri) {
        *path_out = path;
        path = NULL;
    }

out:
    if (error) {
        fprintf(stderr, "portal fixture failed: %s\n", error->message);
        g_error_free(error);
    }
    if (pixbuf)
        g_object_unref(pixbuf);
    if (path) {
        unlink(path);
        g_free(path);
    }
    return uri;
}

static bool
image_is_black(const uint8_t *data, size_t size)
{
    size_t index;

    for (index = 0; index < size; index++) {
        if (data[index] != 0)
            return false;
    }
    return true;
}

int
main(void)
{
    struct fake_portal portal;
    char *fixture_path = NULL;
    char *fixture_uri = NULL;
    char *data_home = NULL;
    char *desktop_path = NULL;
    char *applications_path = NULL;
    GError *error = NULL;
    uint8_t destination[16];
    XImage image;
    bool server_started = false;
    bool ok = false;

    fixture_uri = create_png_fixture(&fixture_path);
    data_home = g_dir_make_tmp("qq-preload-data-XXXXXX", &error);
    if (!fixture_uri || !data_home) {
        if (error) {
            fprintf(stderr, "temporary data directory failed: %s\n",
                    error->message);
            g_error_free(error);
        }
        goto out;
    }
    setenv("XDG_DATA_HOME", data_home, 1);

    if (!start_fake_portal(&portal, fixture_uri))
        goto out;
    server_started = true;

    memset(destination, 0x7f, sizeof(destination));
    initialize_ximage(&image, destination, 2, 2);
    if (!qq_screenshot_copy_to_ximage(&image) ||
        !pixel_equals(destination, 0, 0, 0, 255) ||
        !pixel_equals(destination, 1, 0, 255, 0) ||
        !pixel_equals(destination, 2, 255, 0, 0) ||
        !pixel_equals(destination, 3, 255, 255, 255)) {
        fprintf(stderr, "successful portal screenshot was not copied\n");
        goto out;
    }

    atomic_store(&portal.response, 2U);
    memset(destination, 0x7f, sizeof(destination));
    if (qq_screenshot_copy_to_ximage(&image) ||
        !image_is_black(destination, sizeof(destination))) {
        fprintf(stderr, "rejected portal screenshot was not black\n");
        goto out;
    }

    if (atomic_load(&portal.screenshot_calls) != 2U ||
        !atomic_load(&portal.request_valid)) {
        fprintf(stderr, "portal screenshot request did not match the API\n");
        goto out;
    }
    ok = true;

out:
    if (server_started)
        stop_fake_portal(&portal);
    if (fixture_path) {
        unlink(fixture_path);
        g_free(fixture_path);
    }
    g_free(fixture_uri);
    if (data_home) {
        applications_path = g_build_filename(data_home,
                                             "applications", NULL);
        desktop_path = g_build_filename(applications_path,
                                        QQ_PORTAL_APP_ID ".desktop", NULL);
        unlink(desktop_path);
        rmdir(applications_path);
        rmdir(data_home);
    }
    g_free(desktop_path);
    g_free(applications_path);
    g_free(data_home);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
