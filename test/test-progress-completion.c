/* Exercise the real handler on a private desktop; intercept only app lifecycle
 * and desktop notification/status-icon backends. No live Nemo is involved. */
#include <config.h>
#include <gtk/gtk.h>
#include "../src/nemo-application.h"
#include <libnemo-private/nemo-icon-fallback.h>
#include <libxapp/xapp-status-icon.h>
#include <eel/eel-debug.h>

static GApplication *application;
static guint holds;
static guint notifications;
static GNotification *last_notification;
static char *notification_body;
static char *notification_target;
static gboolean real_backends;
static gboolean pending_backend_exit;
static guint delivered_notifications;

typedef GObject TestStatusIcon;
typedef GObjectClass TestStatusIconClass;
G_DEFINE_TYPE (TestStatusIcon, test_status_icon, G_TYPE_OBJECT)

static void
test_status_icon_class_init (TestStatusIconClass *klass)
{
    g_signal_new ("activate", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);
}

static void test_status_icon_init (TestStatusIcon *icon) {}
static XAppStatusIcon *test_status_new (void)
{
    if (real_backends)
        return xapp_status_icon_new ();
    return (XAppStatusIcon *) g_object_new (test_status_icon_get_type (), NULL);
}
static void test_status_name (XAppStatusIcon *icon, const char *text)
{
    if (real_backends)
        xapp_status_icon_set_icon_name (icon, text);
}
static void test_status_tooltip (XAppStatusIcon *icon, const char *text)
{
    if (real_backends)
        xapp_status_icon_set_tooltip_text (icon, text);
}
static void test_status_visible (XAppStatusIcon *icon, gboolean visible)
{
    if (real_backends)
        xapp_status_icon_set_visible (icon, visible);
}

static void
test_notification_body (GNotification *notification, const char *text)
{
    g_free (notification_body);
    notification_body = g_strdup (text);
    g_notification_set_body (notification, text);
}

static void
test_notification_action (GNotification *notification, const char *action,
                          const char *format, const char *text)
{
    g_assert_cmpstr (action, ==, "app.show-file-operation-results");
    g_assert_cmpstr (format, ==, "s");
    g_free (notification_target);
    notification_target = g_strdup (text);
    g_notification_set_default_action_and_target (notification, action, format, text);
}

static void
test_notification_icon (GNotification *notification, GIcon *icon)
{
    g_assert_true (G_IS_THEMED_ICON (icon));
    g_assert_cmpstr (g_themed_icon_get_names (G_THEMED_ICON (icon))[0], ==, "system-file-manager");
    g_notification_set_icon (notification, icon);
}

static NemoApplication *
test_application (void)
{
    return (NemoApplication *) application;
}

static void
test_hold (GApplication *app)
{
    g_assert_true (app == application);
    holds++;
    g_application_hold (app);
}

static void
test_release (GApplication *app)
{
    g_assert_true (app == application);
    g_assert_cmpuint (holds, >, 0);
    holds--;
    g_application_release (app);
}

static void
test_send_notification (GApplication *app, const char *id, GNotification *notification)
{
    g_assert_true (app == application);
    g_assert_nonnull (id);
    notifications++;
    g_set_object (&last_notification, notification);
    if (real_backends)
        g_application_send_notification (app, id, notification);
}

static void
test_withdraw_notification (GApplication *app, const char *id)
{
    g_assert_true (app == application);
    g_clear_object (&last_notification);
    if (real_backends)
        g_application_withdraw_notification (app, id);
}

#define nemo_application_get_singleton test_application
#define g_application_hold test_hold
#define g_application_release test_release
#define g_application_send_notification test_send_notification
#define g_application_withdraw_notification test_withdraw_notification
#define xapp_status_icon_new test_status_new
#define xapp_status_icon_set_icon_name test_status_name
#define xapp_status_icon_set_tooltip_text test_status_tooltip
#define xapp_status_icon_set_visible test_status_visible
#define g_notification_set_body test_notification_body
#define g_notification_set_default_action_and_target test_notification_action
#define g_notification_set_icon test_notification_icon
#include "../src/nemo-progress-ui-handler.c"
#undef nemo_application_get_singleton
#undef g_application_hold
#undef g_application_release
#undef g_application_send_notification
#undef g_application_withdraw_notification
#undef xapp_status_icon_new
#undef xapp_status_icon_set_icon_name
#undef xapp_status_icon_set_tooltip_text
#undef xapp_status_icon_set_visible
#undef g_notification_set_body
#undef g_notification_set_default_action_and_target
#undef g_notification_set_icon

static void
drain (void)
{
    while (g_main_context_iteration (NULL, FALSE));
}

static NemoProgressUIHandler *
new_handler (void)
{
    g_assert_cmpuint (holds, ==, 0);
    notifications = 0;
    g_clear_object (&last_notification);
    return nemo_progress_ui_handler_new ();
}

static NemoProgressInfo *
new_operation (void)
{
    NemoProgressInfo *info = nemo_progress_info_new ();
    nemo_progress_info_take_initial_details (info, g_strdup ("Copying test data"));
    nemo_progress_info_queue (info);
    nemo_progress_info_start (info);
    return info;
}

static void
finish_operation (NemoProgressInfo *info, NemoProgressOutcome outcome)
{
    NemoProgressResult result = { 0 };
    result.operation = NEMO_PROGRESS_OPERATION_COPY;
    result.outcome = outcome;
    nemo_progress_info_set_result (info, &result);
    nemo_progress_info_finish (info);
    drain ();
}

static void
show_operation (NemoProgressUIHandler *handler, NemoProgressInfo *info)
{
    OperationWatch *watch = g_hash_table_lookup (handler->priv->operations, info);
    g_assert_nonnull (watch);
    g_source_remove (watch->timeout_id);
    watch->timeout_id = 0;
    g_assert_false (operation_show_timeout (watch));
}

static const char *
last_summary (NemoProgressUIHandler *handler)
{
    GList *children = gtk_container_get_children (GTK_CONTAINER (handler->priv->completed_list));
    const char *text = gtk_label_get_text (GTK_LABEL (g_list_last (children)->data));
    g_list_free (children);
    return text;
}

static void
assert_empty_manager (NemoProgressUIHandler *handler)
{
    g_assert_null (nemo_progress_info_manager_get_all_infos (handler->priv->manager));
    g_assert_cmpuint (g_hash_table_size (handler->priv->operations), ==, 0);
    g_assert_cmpuint (handler->priv->active_infos, ==, 0);
}

static void
test_quick_hidden (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation ();
    gpointer weak_info = info;
    g_object_add_weak_pointer (G_OBJECT (info), &weak_info);

    /* queued, started and finished may all be delivered by the same idle. */
    finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_cmpuint (handler->priv->completed_count, ==, 1);
    g_assert_cmpuint (notifications, ==, 1);
    g_assert_nonnull (last_notification);
    g_assert_cmpstr (notification_body, ==, last_summary (handler));
    g_assert_cmpstr (notification_target, ==, notification_body);
    g_assert_false (gtk_widget_get_visible (handler->priv->progress_window));
    g_assert_false (handler->priv->window_held);
    g_assert_cmpuint (holds, ==, 0);
    assert_empty_manager (handler);

    nemo_progress_info_finish (info);
    drain ();
    g_assert_cmpuint (handler->priv->completed_count, ==, 1);
    g_object_unref (info);
    g_assert_null (weak_info);
    g_object_unref (handler);
    drain ();
}

static void
test_visible_and_close (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation ();
    drain ();
    show_operation (handler, info);
    g_assert_true (handler->priv->window_held);
    g_assert_cmpuint (holds, ==, 2);
    finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_true (gtk_widget_get_visible (handler->priv->progress_window));
    g_assert_cmpuint (notifications, ==, 0);
    g_assert_cmpuint (holds, ==, 1);
    g_assert_cmpuint (handler->priv->completed_count, ==, 1);
    assert_empty_manager (handler);

    progress_window_close_clicked (NULL, handler);
    g_assert_cmpuint (holds, ==, 0);
    g_assert_cmpuint (handler->priv->completed_count, ==, 0);
    g_assert_false (gtk_widget_get_visible (handler->priv->progress_window));
    g_object_unref (info);
    g_object_unref (handler);
    drain ();
}

static void
test_concurrent_close (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *first = new_operation ();
    NemoProgressInfo *second = new_operation ();
    drain ();
    show_operation (handler, first);
    show_operation (handler, second);
    finish_operation (first, NEMO_PROGRESS_OUTCOME_PARTIAL);
    g_assert_cmpuint (handler->priv->active_infos, ==, 1);
    g_assert_cmpuint (holds, ==, 2);
    progress_window_close_clicked (NULL, handler);
    g_assert_cmpuint (holds, ==, 1);
    g_assert_cmpuint (handler->priv->completed_count, ==, 0);
    GCancellable *cancel = nemo_progress_info_get_cancellable (second);
    g_assert_false (g_cancellable_is_cancelled (cancel));
    g_object_unref (cancel);
    finish_operation (second, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_cmpuint (notifications, ==, 1);
    g_assert_false (gtk_widget_get_visible (handler->priv->progress_window));
    g_assert_cmpuint (holds, ==, 0);
    assert_empty_manager (handler);
    g_object_unref (first);
    g_object_unref (second);
    g_object_unref (handler);
    drain ();
}

static void
test_mixed_results (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *first = new_operation ();
    NemoProgressInfo *second = new_operation ();
    drain ();
    show_operation (handler, first);
    finish_operation (first, NEMO_PROGRESS_OUTCOME_CANCELLED);
    g_autofree char *cancelled = g_strdup (last_summary (handler));
    finish_operation (second, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_cmpuint (handler->priv->completed_count, ==, 2);
    g_assert_cmpstr (cancelled, !=, last_summary (handler));
    g_assert_null (strstr (last_summary (handler), "All file operations"));
    g_assert_cmpuint (notifications, ==, 0);
    g_object_unref (first);
    g_object_unref (second);
    g_object_unref (handler);
    g_assert_cmpuint (holds, ==, 0);
    drain ();
}

static void
test_dispose_pending (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation ();
    gpointer weak_handler = handler;
    g_object_add_weak_pointer (G_OBJECT (handler), &weak_handler);
    drain ();
    OperationWatch *watch = g_hash_table_lookup (handler->priv->operations, info);
    guint timeout = watch->timeout_id;
    g_assert_cmpuint (holds, ==, 1);
    g_object_run_dispose (G_OBJECT (handler));
    g_object_run_dispose (G_OBJECT (handler));
    g_assert_cmpuint (holds, ==, 0);
    g_assert_null (g_main_context_find_source_by_id (NULL, timeout));
    g_object_unref (handler);
    g_assert_null (weak_handler);
    nemo_progress_info_finish (info);
    drain ();
    g_object_unref (info);
}

static void
test_dispose_visible_active (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation ();
    drain ();
    show_operation (handler, info);
    g_object_unref (handler);
    g_assert_cmpuint (holds, ==, 0);
    /* Destroyed progress widgets must disconnect all live info signals. */
    nemo_progress_info_set_status (info, "Still running");
    nemo_progress_info_set_progress (info, 1, 2);
    nemo_progress_info_finish (info);
    drain ();
    g_object_unref (info);
}

static void
test_notification_reopen (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    GVariant *target = g_variant_ref_sink (g_variant_new_string ("Earlier copy\nVerification failed"));
    g_action_group_activate_action (G_ACTION_GROUP (application), "show-file-operation-results", target);
    g_assert_cmpstr (last_summary (handler), ==,
                     "Earlier completion notification\nEarlier copy\nVerification failed");
    g_assert_true (gtk_widget_get_visible (handler->priv->progress_window));
    g_assert_cmpuint (holds, ==, 1);
    g_variant_unref (target);
    g_object_unref (handler);
    g_assert_cmpuint (holds, ==, 0);
    g_assert_false (g_action_group_has_action (G_ACTION_GROUP (application), "show-file-operation-results"));
    drain ();
}

static void
test_history_limit (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    for (guint i = 0; i < COMPLETION_HISTORY_LIMIT + 3; i++) {
        NemoProgressInfo *info = new_operation ();
        finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
        g_object_unref (info);
    }
    g_assert_cmpuint (handler->priv->completed_count, ==, COMPLETION_HISTORY_LIMIT);
    g_assert_true (gtk_widget_get_visible (handler->priv->history_notice));
    g_assert_cmpuint (holds, ==, 0);
    progress_window_close_clicked (NULL, handler);
    g_assert_cmpuint (handler->priv->completed_count, ==, 0);
    g_assert_false (gtk_widget_get_visible (handler->priv->history_notice));
    g_object_unref (handler);
    drain ();
}

static void
test_model_snapshot (void)
{
    NemoProgressInfo *info = nemo_progress_info_new ();
    NemoProgressResult result = { .operation = NEMO_PROGRESS_OPERATION_COPY,
                                  .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
                                  .completed_items = 2,
                                  .checksum_verified_files = 1,
                                  .verified_symlinks = 1 };
    NemoProgressResult snapshot;
    g_assert_false (nemo_progress_info_get_result (info, &snapshot));
    nemo_progress_info_set_result (info, &result);
    nemo_progress_info_finish (info);
    nemo_progress_info_cancel (info);
    result.outcome = NEMO_PROGRESS_OUTCOME_FAILED;
    nemo_progress_info_set_result (info, &result);
    g_assert_true (nemo_progress_info_get_result (info, &snapshot));
    g_assert_cmpint (snapshot.outcome, ==, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_autofree char *text = nemo_progress_info_get_completion_text (info);
    g_assert_nonnull (strstr (text, "Copy completed."));
    g_assert_nonnull (strstr (text, "SHA-256 verified files (completed): 1"));
    g_assert_nonnull (strstr (text, "Link-text verified symbolic links (completed): 1"));
    drain ();
    g_object_unref (info);

    info = nemo_progress_info_new ();
    nemo_progress_info_finish (info);
    g_free (text);
    text = nemo_progress_info_get_completion_text (info);
    g_assert_cmpstr (text, ==, "Operation finished. Success was not reported.");
    drain ();
    g_object_unref (info);
}

static void
test_notification_input (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    g_autofree char *large = g_strnfill (COMPLETION_TEXT_LIMIT + 1, 'x');
    GVariant *target = g_variant_ref_sink (g_variant_new_string (large));
    g_test_expect_message ("Nemo", G_LOG_LEVEL_WARNING, "*invalid file-operation notification*");
    g_action_group_activate_action (G_ACTION_GROUP (application), "show-file-operation-results", target);
    g_test_assert_expected_messages ();
    g_assert_cmpuint (handler->priv->completed_count, ==, 0);
    g_assert_cmpuint (holds, ==, 0);
    g_variant_unref (target);

    target = g_variant_ref_sink (g_variant_new_string ("<b>not markup</b>"));
    g_action_group_activate_action (G_ACTION_GROUP (application), "show-file-operation-results", target);
    g_assert_nonnull (strstr (last_summary (handler), "<b>not markup</b>"));
    g_action_group_activate_action (G_ACTION_GROUP (application), "show-file-operation-results", target);
    g_assert_cmpuint (handler->priv->completed_count, ==, 1);
    g_variant_unref (target);
    g_object_unref (handler);
    drain ();
}

static void
test_fresh_batch (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation ();
    finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
    progress_window_close_clicked (NULL, handler);
    g_object_unref (info);
    info = new_operation ();
    drain ();
    show_operation (handler, info);
    g_assert_true (gtk_widget_get_visible (handler->priv->progress_window));
    finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_object_unref (info);
    g_object_unref (handler);
    drain ();
}

static void
notification_method (GDBusConnection *connection, const char *sender,
                     const char *path, const char *interface, const char *method,
                     GVariant *parameters, GDBusMethodInvocation *invocation,
                     gpointer data)
{
    if (g_str_equal (method, "GetCapabilities")) {
        const char *capabilities[] = { "body", "actions", NULL };
        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(^as)", capabilities));
    } else if (g_str_equal (method, "GetServerInformation")) {
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(ssss)", "Test", "Nemo", "1", "1.2"));
    } else if (g_str_equal (method, "Notify")) {
        GVariant *body = g_variant_get_child_value (parameters, 4);
        g_assert_nonnull (strstr (g_variant_get_string (body, NULL), "Copy completed."));
        g_variant_unref (body);
        delivered_notifications++;
        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(u)", delivered_notifications));
    } else {
        g_assert_cmpstr (method, ==, "CloseNotification");
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
}

static gboolean
stop_waiting (gpointer data)
{
    *(gboolean *) data = TRUE;
    return G_SOURCE_REMOVE;
}

static void
settle_backends (void)
{
    gboolean finished = FALSE;
    g_timeout_add (100, stop_waiting, &finished);
    while (!finished)
        g_main_context_iteration (NULL, TRUE);
}

static void
shutdown_drain (void)
{
    /* The production thumbnail shutdown re-enters the same main context. */
    for (guint i = 0; i < 5; i++)
        settle_backends ();
}

static void
test_real_backend_lifecycle (void)
{
    static const char xml[] =
        "<node><interface name='org.freedesktop.Notifications'>"
        "<method name='GetCapabilities'><arg type='as' direction='out'/></method>"
        "<method name='GetServerInformation'><arg type='s' direction='out'/>"
        "<arg type='s' direction='out'/><arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
        "<method name='Notify'><arg type='s'/><arg type='u'/><arg type='s'/><arg type='s'/>"
        "<arg type='s'/><arg type='as'/><arg type='a{sv}'/><arg type='i'/>"
        "<arg type='u' direction='out'/></method>"
        "<method name='CloseNotification'><arg type='u'/></method>"
        "</interface></node>";
    static const GDBusInterfaceVTable vtable = { .method_call = notification_method };
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
    g_assert_no_error (error);
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml (xml, &error);
    g_assert_no_error (error);
    guint registration = g_dbus_connection_register_object (
        bus, "/org/freedesktop/Notifications", node->interfaces[0], &vtable, NULL, NULL, &error);
    g_assert_no_error (error);
    GVariant *reply = g_dbus_connection_call_sync (
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        "RequestName", g_variant_new ("(su)", "org.freedesktop.Notifications", 0),
        G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    g_assert_no_error (error);
    g_variant_unref (reply);

    NemoProgressUIHandler *handler = new_handler ();
    if (pending_backend_exit) {
        NemoProgressInfo *info = new_operation ();
        NemoProgressResult result = { .operation = NEMO_PROGRESS_OPERATION_COPY,
                                      .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS };
        nemo_progress_info_set_result (info, &result);
        nemo_progress_info_finish (info);
        while (handler->priv->completed_count == 0)
            g_main_context_iteration (NULL, TRUE);
        g_assert_cmpuint (holds, ==, 0);
        g_object_unref (info);
        GSimpleAction *action = g_object_ref (handler->priv->show_action);
        NemoProgressInfo *pending = new_operation ();
        progress_info_queued_cb (pending, handler);
        OperationWatch *watch = g_hash_table_lookup (handler->priv->operations, pending);
        guint timeout = watch->timeout_id;
        guint summaries = handler->priv->completed_count;
        guint sent = notifications;
        nemo_progress_ui_handler_shutdown (handler);
        nemo_progress_ui_handler_shutdown (handler);
        g_assert_cmpuint (holds, ==, 0);
        g_assert_null (g_main_context_find_source_by_id (NULL, timeout));
        GCancellable *cancel = nemo_progress_info_get_cancellable (pending);
        g_assert_false (g_cancellable_is_cancelled (cancel));
        g_object_unref (cancel);
        GVariant *target = g_variant_ref_sink (g_variant_new_string ("Earlier result"));
        g_action_activate (G_ACTION (action), target);
        status_icon_activate_cb (handler->priv->status_icon, 1, 0, handler);
        g_assert_false (gtk_widget_get_visible (handler->priv->progress_window));
        nemo_progress_info_finish (pending);
        eel_debug_call_at_shutdown (shutdown_drain);
        eel_debug_shut_down ();
        g_assert_cmpuint (delivered_notifications, ==, 1);
        g_assert_cmpuint (notifications, ==, sent);
        g_assert_cmpuint (handler->priv->completed_count, ==, summaries);
        g_assert_cmpuint (holds, ==, 0);
        g_variant_unref (target);
        g_object_unref (action);
        g_object_unref (pending);
        g_object_unref (handler);
        /* Like main(), disposal now follows the final reentrant shutdown drain. */
        g_assert_false (g_action_group_has_action (G_ACTION_GROUP (application), "show-file-operation-results"));
        g_dbus_connection_unregister_object (bus, registration);
        g_dbus_node_info_unref (node);
        g_object_unref (bus);
        return;
    }
    XAppStatusIcon *icon = NULL;
    for (guint i = 0; i < 3; i++) {
        NemoProgressInfo *info = new_operation ();
        NemoProgressResult result = { .operation = NEMO_PROGRESS_OPERATION_COPY,
                                      .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS };
        drain ();
        show_operation (handler, info);
        progress_window_close_clicked (NULL, handler);
        nemo_progress_info_set_result (info, &result);
        nemo_progress_info_finish (info);
        drain ();
        for (guint attempt = 0; attempt < 30 && delivered_notifications <= i; attempt++)
            settle_backends ();
        g_assert_cmpuint (delivered_notifications, ==, i + 1);
        if (icon == NULL)
            icon = handler->priv->status_icon;
        g_assert_true (icon == handler->priv->status_icon);
        status_icon_activate_cb (icon, 1, 0, handler);
        g_assert_true (handler->priv->window_held);
        progress_window_close_clicked (NULL, handler);
        g_assert_cmpuint (holds, ==, 0);
        g_object_unref (info);
    }
    g_object_unref (handler);
    g_assert_false (g_action_group_has_action (G_ACTION_GROUP (application), "show-file-operation-results"));
    for (guint i = 0; i < 5; i++)
        settle_backends ();
    g_dbus_connection_unregister_object (bus, registration);
    g_dbus_node_info_unref (node);
    g_object_unref (bus);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
        g_printerr ("Run through test/run-isolated-regression.py\n");
        return 77;
    }
    g_test_init (&argc, &argv, NULL);
    gtk_init (&argc, &argv);
    nemo_icon_fallback_init ();
    application = g_application_new ("org.nemo.CompletionTest", G_APPLICATION_NON_UNIQUE);
    GError *error = NULL;
    g_assert_true (g_application_register (application, NULL, &error));
    g_assert_no_error (error);
    if (argc > 1 && (g_str_equal (argv[1], "--real-backends") ||
                    g_str_equal (argv[1], "--real-pending-exit"))) {
        real_backends = TRUE;
        pending_backend_exit = g_str_equal (argv[1], "--real-pending-exit");
        g_test_add_func ("/completion/real-backend-lifecycle", test_real_backend_lifecycle);
    } else {
    g_test_add_func ("/completion/quick-hidden", test_quick_hidden);
    g_test_add_func ("/completion/visible-close", test_visible_and_close);
    g_test_add_func ("/completion/concurrent-close", test_concurrent_close);
    g_test_add_func ("/completion/mixed-results", test_mixed_results);
    g_test_add_func ("/completion/dispose-pending", test_dispose_pending);
    g_test_add_func ("/completion/dispose-visible-active", test_dispose_visible_active);
    g_test_add_func ("/completion/notification-reopen", test_notification_reopen);
    g_test_add_func ("/completion/history-limit", test_history_limit);
    g_test_add_func ("/completion/model-snapshot", test_model_snapshot);
    g_test_add_func ("/completion/notification-input", test_notification_input);
    g_test_add_func ("/completion/fresh-batch", test_fresh_batch);
    }
    int result = g_test_run ();
    g_clear_object (&last_notification);
    g_free (notification_body);
    g_free (notification_target);
    g_object_unref (application);
    return result;
}
