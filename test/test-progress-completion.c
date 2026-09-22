/* Exercise the real handler on a private desktop; intercept only app lifecycle
 * and desktop notification/status-icon backends. No live Nemo is involved. */
#include <config.h>
#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>
#include <gtk/gtk.h>
#include "../src/nemo-application.h"
#include <libnemo-private/nemo-icon-fallback.h>
#include <libxapp/xapp-status-icon.h>
#include <eel/eel-debug.h>
#include <libnemo-private/nemo-smpl-prefs.h>
#include <libnemo-private/nemo-file-private.h>

static GApplication *application;
static guint holds;
static guint notifications;
static GNotification *last_notification;
static char *notification_body;
static char *notification_target;
static gboolean real_backends;
static gboolean pending_backend_exit;
static guint delivered_notifications;
static char *schema_directory;
static char *status_tooltip;
static char *status_icon_name;

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
    g_free (status_icon_name);
    status_icon_name = g_strdup (text);
    if (real_backends)
        xapp_status_icon_set_icon_name (icon, text);
}
static void test_status_tooltip (XAppStatusIcon *icon, const char *text)
{
    g_free (status_tooltip);
    status_tooltip = g_strdup (text);
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

static GHashTable *removal_notifications;

static void
removal_test_body (GNotification *notification, const char *text)
{
    g_object_set_data_full (G_OBJECT (notification), "body", g_strdup (text), g_free);
}

static void
removal_test_send (GApplication *app, const char *id, GNotification *notification)
{
    g_hash_table_replace (removal_notifications, g_strdup (id),
                         g_strdup (g_object_get_data (G_OBJECT (notification), "body")));
}

static void
removal_test_withdraw (GApplication *app, const char *id)
{
    g_hash_table_remove (removal_notifications, id);
}

#define g_notification_set_body removal_test_body
#define g_application_send_notification removal_test_send
#define g_application_withdraw_notification removal_test_withdraw
#define g_application_hold test_hold
#define g_application_release test_release
#include "../libnemo-private/nemo-mount-operation.c"
#undef g_notification_set_body
#undef g_application_send_notification
#undef g_application_withdraw_notification
#undef g_application_hold
#undef g_application_release

typedef struct {
    GObject parent;
    GTask *request;
    GMountOperation *mount_op;
    guint calls;
} TestMount;
typedef GObjectClass TestMountClass;
static void test_mount_iface_init (GMountIface *iface);
G_DEFINE_TYPE_WITH_CODE (TestMount, test_mount, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT, test_mount_iface_init))

static char *test_mount_name (GMount *mount) { return g_strdup ("Test device"); }
static gboolean test_mount_can_remove (GMount *mount) { return TRUE; }
static void
test_mount_remove (GMount *mount, GMountUnmountFlags flags, GMountOperation *op,
                   GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
    TestMount *self = (TestMount *) mount;
    g_assert_cmpint (flags, ==, G_MOUNT_UNMOUNT_NONE);
    g_assert_nonnull (op);
    self->calls++;
    self->request = g_task_new (mount, cancel, callback, data);
    self->mount_op = g_object_ref (op);
}
static gboolean
test_mount_finish (GMount *mount, GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean (G_TASK (result), error);
}
static void
test_mount_iface_init (GMountIface *iface)
{
    iface->get_name = test_mount_name;
    iface->can_unmount = test_mount_can_remove;
    iface->can_eject = test_mount_can_remove;
    iface->unmount_with_operation = test_mount_remove;
    iface->unmount_with_operation_finish = test_mount_finish;
    iface->eject_with_operation = test_mount_remove;
    iface->eject_with_operation_finish = test_mount_finish;
}
static void test_mount_class_init (TestMountClass *klass) {}
static void test_mount_init (TestMount *self) {}

typedef struct {
    gboolean done;
    gboolean success;
    GError *error;
} RemovalResult;
static void
test_removal_done (GObject *target, GAsyncResult *result, gpointer data)
{
    RemovalResult *outcome = data;
    outcome->success = nemo_mount_operation_remove_finish (target, result, &outcome->error);
    outcome->done = TRUE;
}

static void
complete_mount (TestMount *mount, GIOErrorEnum error)
{
    if (error == 0)
        g_task_return_boolean (mount->request, TRUE);
    else
        g_task_return_new_error (mount->request, G_IO_ERROR, error, "Test removal failure");
    g_clear_object (&mount->request);
    g_clear_object (&mount->mount_op);
}

static void
drain (void)
{
    while (g_main_context_iteration (NULL, FALSE));
}

static void
test_removal_lifetime (void)
{
    TestMount *a = g_object_new (test_mount_get_type (), NULL);
    TestMount *b = g_object_new (test_mount_get_type (), NULL);
    RemovalResult ra = { 0 }, rb = { 0 };
    g_hash_table_remove_all (removal_notifications);
    nemo_mount_operation_remove (G_OBJECT (a), NEMO_MOUNT_REMOVE_EJECT, NULL, NULL,
                                 test_removal_done, &ra);
    nemo_mount_operation_remove (G_OBJECT (b), NEMO_MOUNT_REMOVE_UNMOUNT, NULL, NULL,
                                 test_removal_done, &rb);
    g_assert_cmpuint (holds, ==, 2);
    g_assert_true (nemo_mount_operation_is_removing ());
    g_assert_cmpuint (g_hash_table_size (removal_notifications), ==, 2);
    g_signal_emit_by_name (a->mount_op, "show-unmount-progress",
                          "Unsafe backend says safe to remove", (gint64) 0, (gint64) 0);
    g_signal_emit_by_name (a->mount_op, "aborted");
    g_assert_false (ra.done);
    g_assert_true (nemo_mount_operation_is_removing ());
    complete_mount (b, 0);
    drain ();
    g_assert_true (rb.done);
    g_assert_true (rb.success);
    g_assert_true (nemo_mount_operation_is_removing ());
    g_assert_cmpuint (g_hash_table_size (removal_notifications), ==, 2);
    GHashTableIter iter;
    gpointer body;
    guint pending = 0, completed = 0;
    g_hash_table_iter_init (&iter, removal_notifications);
    while (g_hash_table_iter_next (&iter, NULL, &body)) {
        pending += strstr (body, "Do not disconnect") != NULL;
        completed += strstr (body, "volume is unmounted") != NULL;
        g_assert_null (strstr (body, "Unsafe backend"));
    }
    g_assert_cmpuint (pending, ==, 1);
    g_assert_cmpuint (completed, ==, 1);
    complete_mount (a, G_IO_ERROR_BUSY);
    drain ();
    g_assert_true (ra.done);
    g_assert_false (ra.success);
    g_assert_error (ra.error, G_IO_ERROR, G_IO_ERROR_BUSY);
    g_clear_error (&ra.error);
    g_assert_false (nemo_mount_operation_is_removing ());
    g_assert_cmpuint (holds, ==, 0);
    g_object_unref (a);
    g_object_unref (b);
}

static void
test_removal_gate (void)
{
    NemoProgressInfoManager *manager = nemo_progress_info_manager_new ();
    NemoProgressInfo *info = nemo_progress_info_new ();
    TestMount *mount = g_object_new (test_mount_get_type (), NULL);
    for (guint state = 0; state < 3; state++) {
        RemovalResult result = { 0 };
        if (state == 1)
            nemo_progress_info_start (info);
        if (state == 2)
            nemo_progress_info_pause (info);
        nemo_mount_operation_remove (G_OBJECT (mount), NEMO_MOUNT_REMOVE_EJECT, NULL, NULL,
                                     test_removal_done, &result);
        drain ();
        g_assert_true (result.done);
        g_assert_false (result.success);
        g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_BUSY);
        g_assert_nonnull (strstr (result.error->message, "queued or paused"));
        g_clear_error (&result.error);
        g_assert_cmpuint (mount->calls, ==, 0);
        g_assert_false (nemo_mount_operation_is_removing ());
    }

    nemo_progress_info_resume (info);
    nemo_progress_info_finish (info);
    drain ();
    RemovalResult result = { 0 };
    nemo_mount_operation_remove (G_OBJECT (mount), NEMO_MOUNT_REMOVE_EJECT, NULL, NULL,
                                 test_removal_done, &result);
    g_assert_cmpuint (mount->calls, ==, 1);
    complete_mount (mount, G_IO_ERROR_CANCELLED);
    drain ();
    g_assert_true (result.done);
    g_assert_false (result.success);
    g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&result.error);
    g_assert_false (nemo_mount_operation_is_removing ());
    g_assert_cmpuint (holds, ==, 0);
    g_object_unref (info);
    g_object_unref (manager);
    g_object_unref (mount);
}

static void
test_removal_shutdown (void)
{
    TestMount *mount = g_object_new (test_mount_get_type (), NULL);
    RemovalResult result = { 0 };
    g_hash_table_remove_all (removal_notifications);
    nemo_mount_operation_remove (G_OBJECT (mount), NEMO_MOUNT_REMOVE_EJECT, NULL, NULL,
                                 test_removal_done, &result);
    g_assert_cmpuint (g_hash_table_size (removal_notifications), ==, 1);
    nemo_mount_operation_shutdown ();
    g_assert_true (nemo_mount_operation_is_removing ());
    g_assert_cmpuint (g_hash_table_size (removal_notifications), ==, 0);
    complete_mount (mount, 0);
    drain ();
    g_assert_true (result.success);
    g_assert_cmpuint (g_hash_table_size (removal_notifications), ==, 0);
    g_assert_false (nemo_mount_operation_is_removing ());
    g_object_unref (mount);
}

static void
mount_reply (GMountOperation *op, GMountOperationResult result, gpointer user_data)
{
    g_assert_cmpint (result, ==, G_MOUNT_OPERATION_ABORTED);
    *(gboolean *) user_data = TRUE;
}

static void
test_removal_no_force (void)
{
    TestMount *mount = g_object_new (test_mount_get_type (), NULL);
    RemovalResult result = { 0 };
    gboolean aborted = FALSE;
    nemo_mount_operation_remove (G_OBJECT (mount), NEMO_MOUNT_REMOVE_EJECT, NULL, NULL,
                                 test_removal_done, &result);
    GMountOperation *op = g_object_ref (mount->mount_op);
    g_signal_connect (op, "reply", G_CALLBACK (mount_reply), &aborted);
    GArray *processes = g_array_new (FALSE, FALSE, sizeof (GPid));
    const char *choices[] = { "Cancel", "Unmount Anyway", NULL };
    g_signal_emit_by_name (op, "show-processes", "Device busy", processes, choices);
    g_assert_true (aborted);
    g_assert_true (nemo_mount_operation_is_removing ());
    complete_mount (mount, G_IO_ERROR_FAILED_HANDLED);
    drain ();
    g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_BUSY);
    g_assert_nonnull (strstr (result.error->message, "Forced removal is disabled"));
    g_clear_error (&result.error);
    g_assert_false (g_signal_has_handler_pending (op,
                    g_signal_lookup ("show-unmount-progress", G_TYPE_MOUNT_OPERATION), 0, TRUE));
    g_array_unref (processes);
    g_object_unref (op);
    g_object_unref (mount);
}

static void
file_removal_done (NemoFile *file, GFile *location, GError *error, gpointer user_data)
{
    RemovalResult *result = user_data;
    result->success = error == NULL;
    result->error = error != NULL ? g_error_copy (error) : NULL;
    result->done = TRUE;
}

static void
test_file_removal_fallback (void)
{
    g_autofree char *path = g_build_filename (g_get_home_dir (), "fake-device", NULL);
    GFile *location = g_file_new_for_path (path);
    NemoFile *file = nemo_file_get (location);
    TestMount *mount = g_object_new (test_mount_get_type (), NULL);
    GMountOperation *op = g_mount_operation_new ();
    RemovalResult result = { 0 };
    file->details->can_eject = FALSE;
    file->details->mount = G_MOUNT (g_object_ref (mount));
    nemo_file_eject (file, op, NULL, file_removal_done, &result);
    g_assert_true (mount->mount_op == op);
    complete_mount (mount, G_IO_ERROR_CANCELLED);
    drain ();
    g_assert_true (result.done);
    g_assert_false (result.success);
    g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&result.error);
    g_clear_object (&file->details->mount);
    g_object_unref (op);
    g_object_unref (file);
    g_object_unref (location);
    g_object_unref (mount);
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
new_operation_kind (NemoProgressOperation operation)
{
    NemoProgressInfo *info = nemo_progress_info_new ();
    NemoProgressResult result = { .operation = operation };
    nemo_progress_info_set_result (info, &result);
    nemo_progress_info_take_initial_details (info, g_strdup ("Waiting to copy test data"));
    nemo_progress_info_take_completion_context (info, g_strdup ("From: source\nTo: destination"));
    nemo_progress_info_queue (info);
    nemo_progress_info_start (info);
    return info;
}

static NemoProgressInfo *
new_operation (void)
{
    return new_operation_kind (NEMO_PROGRESS_OPERATION_COPY);
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
    if (watch->timeout_id != 0) {
        g_source_remove (watch->timeout_id);
        watch->timeout_id = 0;
        g_assert_false (operation_show_timeout (watch));
    }
    g_assert_true (gtk_widget_get_visible (handler->priv->progress_window));
}

static GtkWidget *
last_result (NemoProgressUIHandler *handler)
{
    GList *children = gtk_container_get_children (GTK_CONTAINER (handler->priv->completed_list));
    GtkWidget *row = g_list_last (children)->data;
    g_list_free (children);
    return row;
}

static const char *
last_summary (NemoProgressUIHandler *handler)
{
    return gtk_label_get_text (GTK_LABEL (g_object_get_data (
        G_OBJECT (last_result (handler)), "summary-label")));
}

static const char *
last_details (NemoProgressUIHandler *handler)
{
    return g_object_get_data (G_OBJECT (last_result (handler)), "result-text");
}

static void
assert_empty_manager (NemoProgressUIHandler *handler)
{
    g_assert_null (nemo_progress_info_manager_get_all_infos (handler->priv->manager));
    g_assert_cmpuint (g_hash_table_size (handler->priv->operations), ==, 0);
    g_assert_cmpuint (handler->priv->active_infos, ==, 0);
}

static void
wait_for_progress (void)
{
    gint64 until = g_get_monotonic_time () + 150 * G_TIME_SPAN_MILLISECOND;
    do {
        drain ();
        g_usleep (1000);
    } while (g_get_monotonic_time () < until);
}

static void
test_active_progress (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation ();
    drain ();
    show_operation (handler, info);
    progress_window_close_clicked (NULL, handler);
    nemo_progress_info_set_status (info, "Copying");
    nemo_progress_info_set_progress (info, 100, 100);
    wait_for_progress ();
    g_assert_cmpfloat (nemo_progress_info_get_progress (info), ==, 0.99);
    g_assert_cmpuint (handler->priv->active_percent, ==, 99);
    g_assert_nonnull (strstr (status_tooltip, "99%"));
    g_assert_cmpstr (status_icon_name, !=, "nemo-progress-100-symbolic");
    nemo_progress_info_set_details (info, "Flushing destination");
    wait_for_progress ();
    g_assert_nonnull (strstr (gtk_window_get_title (GTK_WINDOW (handler->priv->progress_window)),
                              "Flushing destination"));
    nemo_progress_info_pulse_progress (info);
    nemo_progress_info_set_details (info, "Verifying existing files");
    wait_for_progress ();
    g_assert_cmpuint (handler->priv->active_percent, ==, 0);
    g_assert_true (handler->priv->active_indeterminate);
    g_assert_null (strstr (status_tooltip, "%"));
    g_assert_nonnull (strstr (gtk_window_get_title (GTK_WINDOW (handler->priv->progress_window)),
                              "Verifying existing files"));
    nemo_progress_info_set_progress (info, 0, 100);
    wait_for_progress ();
    g_assert_false (handler->priv->active_indeterminate);
    g_assert_cmpuint (handler->priv->active_percent, ==, 0);
    nemo_progress_info_set_progress (info, 100, 100);
    finish_operation (info, NEMO_PROGRESS_OUTCOME_FAILED);
    g_assert_cmpfloat (nemo_progress_info_get_progress (info), <, 1.0);
    g_object_unref (info);
    g_object_unref (handler);

    for (guint cancelled = 0; cancelled < 2; cancelled++) {
        handler = new_handler ();
        info = new_operation ();
        nemo_progress_info_set_progress (info, 100, 100);
        if (cancelled)
            nemo_progress_info_cancel (info);
        finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
        if (cancelled)
            g_assert_cmpfloat (nemo_progress_info_get_progress (info), <, 1.0);
        else
            g_assert_cmpfloat (nemo_progress_info_get_progress (info), ==, 1.0);
        g_object_unref (info);
        g_object_unref (handler);
    }
}

static void
test_aggregate_progress (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *infos[3];
    for (guint i = 0; i < 3; i++) {
        infos[i] = new_operation ();
        nemo_progress_info_set_progress (infos[i], 60, 100);
    }
    drain ();
    show_operation (handler, infos[0]);
    wait_for_progress ();
    g_assert_cmpuint (handler->priv->active_percent, ==, 60);
    for (guint i = 0; i < 3; i++) {
        finish_operation (infos[i], NEMO_PROGRESS_OUTCOME_SUCCESS);
        g_object_unref (infos[i]);
    }
    g_object_unref (handler);
}

static void
test_verification_preference (void)
{
    GError *error = NULL;
    GSettingsSchemaSource *source = g_settings_schema_source_new_from_directory (
        schema_directory, g_settings_schema_source_get_default (), FALSE, &error);
    g_assert_no_error (error);
    GSettingsSchema *schema = g_settings_schema_source_lookup (source, "org.nemo.preferences", FALSE);
    g_assert_nonnull (schema);
    GSettings *saved_preferences = nemo_preferences;
    g_autofree char *path = g_build_filename (g_get_home_dir (), "verification-settings.ini", NULL);
    GSettingsBackend *backend = g_keyfile_settings_backend_new (path, "/", NULL);
    nemo_preferences = g_settings_new_full (schema, backend, NULL);
    g_settings_reset (nemo_preferences, NEMO_PREFERENCES_VERIFY_FILE_COPIES);
    g_assert_true (nemo_smpl_verify_file_copies ());
    g_assert_true (g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_VERIFY_FILE_COPIES, FALSE));
    g_settings_sync ();
    g_clear_object (&nemo_preferences);
    g_object_unref (backend);
    g_assert_true (g_file_test (path, G_FILE_TEST_IS_REGULAR));
    backend = g_keyfile_settings_backend_new (path, "/", NULL);
    nemo_preferences = g_settings_new_full (schema, backend, NULL);
    g_assert_false (nemo_smpl_verify_file_copies ());
    g_assert_true (g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_VERIFY_FILE_COPIES, TRUE));
    g_assert_true (nemo_smpl_verify_file_copies ());
    g_clear_object (&nemo_preferences);
    g_object_unref (backend);
    nemo_preferences = saved_preferences;
    g_settings_schema_unref (schema);
    g_settings_schema_source_unref (source);
}

static void
test_quick_visible (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation ();
    gpointer weak_info = info;
    g_object_add_weak_pointer (G_OBJECT (info), &weak_info);

    /* queued, started and finished may all be delivered by the same idle. */
    finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_cmpuint (handler->priv->completed_count, ==, 1);
    g_assert_cmpuint (notifications, ==, 0);
    g_assert_null (last_notification);
    const char *summary = last_summary (handler);
    g_assert_true (g_str_has_prefix (summary, "Copy completed.\n"));
    g_assert_nonnull (strstr (summary, "From: source\nTo: destination"));
    g_assert_null (strstr (summary, "Waiting"));
    g_assert_null (strstr (summary, "Preparing"));
    g_assert_true (gtk_widget_get_visible (handler->priv->progress_window));
    g_assert_true (handler->priv->window_held);
    g_assert_cmpuint (holds, ==, 1);
    assert_empty_manager (handler);

    nemo_progress_info_finish (info);
    drain ();
    g_assert_cmpuint (handler->priv->completed_count, ==, 1);
    g_object_unref (info);
    g_assert_null (weak_info);
    g_object_unref (handler);
    g_assert_cmpuint (holds, ==, 0);
    drain ();
}

static void
test_hidden_completion (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation ();
    drain ();
    show_operation (handler, info);
    progress_window_close_clicked (NULL, handler);
    finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_cmpuint (notifications, ==, 1);
    g_assert_nonnull (last_notification);
    g_assert_cmpstr (notification_body, ==, last_summary (handler));
    g_assert_cmpstr (notification_target, ==, last_details (handler));
    g_assert_nonnull (strstr (notification_target, "physical-media read"));
    g_assert_null (strstr (notification_body, "physical-media read"));
    g_assert_false (gtk_widget_get_visible (handler->priv->progress_window));
    g_assert_cmpuint (holds, ==, 0);
    assert_empty_manager (handler);
    g_object_unref (info);
    g_object_unref (handler);
    drain ();
}

static void
test_quick_generic_hidden (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation_kind (NEMO_PROGRESS_OPERATION_UNKNOWN);
    nemo_progress_info_finish (info);
    drain ();
    g_assert_cmpuint (handler->priv->completed_count, ==, 0);
    g_assert_cmpuint (notifications, ==, 0);
    g_assert_null (handler->priv->progress_window);
    g_assert_cmpuint (holds, ==, 0);
    assert_empty_manager (handler);
    g_object_unref (info);
    g_object_unref (handler);
    drain ();
}

static void
test_queued_move_visible (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = nemo_progress_info_new ();
    NemoProgressResult result = { .operation = NEMO_PROGRESS_OPERATION_MOVE };
    nemo_progress_info_set_result (info, &result);
    nemo_progress_info_take_initial_details (info, g_strdup ("Waiting to move test data"));
    nemo_progress_info_pause (info);
    nemo_progress_info_queue (info);
    drain ();
    show_operation (handler, info);
    g_assert_false (nemo_progress_info_get_is_started (info));
    g_assert_true (nemo_progress_info_get_is_paused (info));
    OperationWatch *watch = g_hash_table_lookup (handler->priv->operations, info);
    g_assert_cmpuint (watch->timeout_id, ==, 0);
    g_assert_cmpuint (handler->priv->active_infos, ==, 1);
    g_assert_cmpuint (holds, ==, 2);
    result.outcome = NEMO_PROGRESS_OUTCOME_CANCELLED;
    nemo_progress_info_set_result (info, &result);
    nemo_progress_info_finish (info);
    drain ();
    assert_empty_manager (handler);
    g_object_unref (info);
    g_object_unref (handler);
    g_assert_cmpuint (holds, ==, 0);
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
test_keyboard_close (void)
{
    const guint keys[] = { GDK_KEY_Return, GDK_KEY_KP_Enter, GDK_KEY_Escape };

    for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
        NemoProgressUIHandler *handler = new_handler ();
        NemoProgressInfo *info = new_operation ();
        drain ();
        GtkWidget *window = handler->priv->progress_window;
        GtkWidget *button = gtk_window_get_default_widget (GTK_WINDOW (window));
        g_assert_true (GTK_IS_BUTTON (button));
        g_assert_cmpstr (gtk_button_get_label (GTK_BUTTON (button)), ==, "_Close");
        g_assert_true (gtk_widget_has_default (button));
        GList *widgets = gtk_container_get_children (GTK_CONTAINER (handler->priv->list));
        NemoProgressInfoWidget *progress = widgets->data;
        GtkWidget *pause = progress->priv->running_start_pause_button;
        GList *buttons = gtk_container_get_children (GTK_CONTAINER (gtk_widget_get_parent (pause)));
        GtkWidget *cancel_button = g_list_last (buttons)->data;
        gtk_widget_grab_focus (cancel_button);
        g_assert_true (gtk_window_get_focus (GTK_WINDOW (window)) == cancel_button);
        g_list_free (buttons);
        g_list_free (widgets);
        GdkEventKey event = { .type = GDK_KEY_PRESS, .keyval = keys[i] };
        gboolean handled = FALSE;
        g_signal_emit_by_name (window, "key-press-event", &event, &handled);
        g_assert_true (handled);
        g_assert_false (gtk_widget_get_visible (window));
        g_assert_cmpuint (handler->priv->active_infos, ==, 1);
        g_assert_cmpuint (holds, ==, 1);
        g_assert_false (nemo_progress_info_get_is_paused (info));
        GCancellable *cancel = nemo_progress_info_get_cancellable (info);
        g_assert_false (g_cancellable_is_cancelled (cancel));
        g_object_unref (cancel);

        finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
        status_icon_activate_cb (handler->priv->status_icon, 1, 0, handler);
        handled = FALSE;
        g_signal_emit_by_name (window, "key-press-event", &event, &handled);
        g_assert_true (handled);
        g_assert_false (gtk_widget_get_visible (window));
        g_assert_cmpuint (handler->priv->completed_count, ==, 0);
        g_assert_cmpuint (holds, ==, 0);
        g_object_unref (info);
        g_object_unref (handler);
        drain ();
    }
}

static void
test_dialog_parent (void)
{
    for (guint before_queue = 0; before_queue < 2; before_queue++) {
        NemoProgressUIHandler *handler = new_handler ();
        NemoProgressInfo *info = new_operation ();
        if (!before_queue)
            drain ();
        GtkWidget *dialog = gtk_dialog_new ();
        gtk_window_set_modal (GTK_WINDOW (dialog), TRUE);
        nemo_progress_info_attach_dialog (info, GTK_WINDOW (dialog));
        gtk_widget_show (dialog);
        /* Deliver queued presentation while the conflict is already visible. */
        drain ();
        g_assert_true (gtk_window_get_transient_for (GTK_WINDOW (dialog)) ==
                       GTK_WINDOW (handler->priv->progress_window));
        g_assert_true (gtk_widget_get_visible (dialog));
        g_assert_true (gtk_window_get_modal (GTK_WINDOW (dialog)));
        g_assert_true (gtk_widget_get_visible (handler->priv->progress_window));
        gtk_widget_destroy (dialog);
        finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
        g_object_unref (info);
        g_object_unref (handler);
        drain ();
    }
}

static void
test_completion_details (void)
{
    for (guint failed = 0; failed < 2; failed++) {
        NemoProgressUIHandler *handler = new_handler ();
        NemoProgressInfo *info = new_operation ();
        NemoProgressResult result = {
            .operation = NEMO_PROGRESS_OPERATION_COPY,
            .outcome = failed ? NEMO_PROGRESS_OUTCOME_PARTIAL : NEMO_PROGRESS_OUTCOME_SUCCESS,
            .completed_items = 2, .completed_regular_files = 2,
            .checksum_verified_files = 2, .failed_items = failed,
            .verification_requested = TRUE
        };
        nemo_progress_info_take_completion_details (info,
            g_strdup ("From: source\nTo: destination\nRecovery: retained-file\nOptional metadata warning"));
        nemo_progress_info_set_result (info, &result);
        nemo_progress_info_finish (info);
        drain ();
        g_assert_nonnull (strstr (last_summary (handler), "SHA-256 verified files (completed): 2"));
        g_assert_nonnull (strstr (last_summary (handler), "From: source\nTo: destination"));
        g_assert_null (strstr (last_summary (handler), "Optional metadata"));
        g_assert_null (strstr (last_summary (handler), "physical-media read"));
        g_assert_nonnull (strstr (last_details (handler), "Recovery: retained-file"));
        g_assert_nonnull (strstr (last_details (handler), "Optional metadata"));
        GtkExpander *details = g_object_get_data (G_OBJECT (last_result (handler)), "details-expander");
        g_assert_cmpint (gtk_expander_get_expanded (details), ==, failed);
        if (failed) {
            g_assert_nonnull (strstr (last_summary (handler), "Copy incomplete."));
            g_assert_nonnull (strstr (last_summary (handler), "Failed items: 1"));
            g_assert_null (strstr (last_summary (handler), "All copied regular files"));
        } else {
            g_assert_nonnull (strstr (last_summary (handler), "Copy completed."));
            g_assert_nonnull (strstr (last_summary (handler), "All copied regular files were SHA-256 verified."));
        }
        g_object_unref (info);
        g_object_unref (handler);
        drain ();
    }
}

static void
test_generic_with_transfer (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *copy = new_operation ();
    NemoProgressInfo *generic = new_operation_kind (NEMO_PROGRESS_OPERATION_UNKNOWN);
    drain ();
    nemo_progress_info_finish (generic);
    drain ();
    g_assert_cmpuint (handler->priv->completed_count, ==, 0);
    g_assert_true (gtk_widget_get_visible (handler->priv->progress_window));
    finish_operation (copy, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_cmpuint (handler->priv->completed_count, ==, 1);
    g_assert_true (g_str_has_prefix (last_summary (handler), "Copy completed."));
    g_assert_null (strstr (last_summary (handler), "Success was not reported"));
    g_assert_cmpuint (notifications, ==, 0);
    g_object_unref (generic);
    g_object_unref (copy);
    g_object_unref (handler);
    drain ();
}

static void
test_generic_visible (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation_kind (NEMO_PROGRESS_OPERATION_UNKNOWN);
    drain ();
    show_operation (handler, info);
    nemo_progress_info_finish (info);
    drain ();
    g_assert_cmpuint (handler->priv->completed_count, ==, 0);
    g_assert_cmpuint (notifications, ==, 0);
    g_assert_false (gtk_widget_get_visible (handler->priv->progress_window));
    g_assert_cmpuint (holds, ==, 0);
    assert_empty_manager (handler);
    g_object_unref (info);
    g_object_unref (handler);
    drain ();
}

static void
test_unknown_transfer_result (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation ();
    nemo_progress_info_finish (info);
    drain ();
    g_assert_cmpuint (handler->priv->completed_count, ==, 1);
    g_assert_nonnull (strstr (last_summary (handler), "Success was not reported."));
    g_assert_null (strstr (last_summary (handler), "Copy completed."));
    GtkExpander *details = g_object_get_data (G_OBJECT (last_result (handler)), "details-expander");
    g_assert_true (gtk_expander_get_expanded (details));
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
    NemoProgressInfo *third = new_operation ();
    drain ();
    g_assert_false (gtk_widget_get_visible (handler->priv->progress_window));
    finish_operation (second, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_cmpuint (notifications, ==, 1);
    g_assert_false (gtk_widget_get_visible (handler->priv->progress_window));
    finish_operation (third, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_cmpuint (notifications, ==, 2);
    g_assert_cmpuint (holds, ==, 0);
    assert_empty_manager (handler);
    g_object_unref (first);
    g_object_unref (second);
    g_object_unref (third);
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
    NemoProgressInfo *info = new_operation_kind (NEMO_PROGRESS_OPERATION_UNKNOWN);
    gpointer weak_handler = handler;
    g_object_add_weak_pointer (G_OBJECT (handler), &weak_handler);
    drain ();
    OperationWatch *watch = g_hash_table_lookup (handler->priv->operations, info);
    guint timeout = watch->timeout_id;
    g_assert_cmpuint (timeout, !=, 0);
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
    g_assert_cmpstr (last_summary (handler), ==, "Earlier completion notification");
    g_assert_cmpstr (last_details (handler), ==, "Earlier copy\nVerification failed");
    GtkExpander *details = g_object_get_data (G_OBJECT (last_result (handler)), "details-expander");
    g_assert_true (gtk_expander_get_expanded (details));
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
    g_assert_cmpuint (holds, ==, 1);
    progress_window_close_clicked (NULL, handler);
    g_assert_cmpuint (holds, ==, 0);
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
    nemo_progress_info_take_completion_details (info, g_strdup ("From: source\nTo: destination"));
    nemo_progress_info_set_result (info, &result);
    nemo_progress_info_finish (info);
    nemo_progress_info_take_completion_details (info, g_strdup ("Changed context"));
    nemo_progress_info_cancel (info);
    result.outcome = NEMO_PROGRESS_OUTCOME_FAILED;
    nemo_progress_info_set_result (info, &result);
    g_assert_true (nemo_progress_info_get_result (info, &snapshot));
    g_assert_cmpint (snapshot.outcome, ==, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_autofree char *text = nemo_progress_info_get_completion_text (info);
    g_assert_nonnull (strstr (text, "Copy completed."));
    g_assert_nonnull (strstr (text, "From: source\nTo: destination"));
    g_assert_null (strstr (text, "Changed context"));
    g_assert_nonnull (strstr (text, "SHA-256 verified files (completed): 1"));
    g_assert_nonnull (strstr (text, "Link-text verified symbolic links (completed): 1"));
    drain ();
    g_object_unref (info);

    info = nemo_progress_info_new ();
    nemo_progress_info_finish (info);
    g_free (text);
    text = nemo_progress_info_get_completion_text (info);
    g_assert_cmpstr (text, ==, "Operation finished.");
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
    g_assert_nonnull (strstr (last_details (handler), "<b>not markup</b>"));
    g_action_group_activate_action (G_ACTION_GROUP (application), "show-file-operation-results", target);
    g_assert_cmpuint (handler->priv->completed_count, ==, 1);
    g_variant_unref (target);
    g_object_unref (handler);
    drain ();
}

static void
test_verification_wording (void)
{
    const struct {
        NemoProgressResult result;
        const char *expected;
        gboolean all_verified;
    } cases[] = {
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
              .completed_items = 1, .completed_regular_files = 1 },
            "Content verification was not requested.", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
              .completed_items = 13, .completed_regular_files = 11, .completed_directories = 2,
              .checksum_verified_files = 11, .verification_requested = TRUE },
            "Regular files copied: 11\nDirectories completed: 2", TRUE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_PARTIAL,
              .completed_items = 1, .completed_regular_files = 1, .checksum_verified_files = 1,
              .skipped_items = 1, .verification_requested = TRUE },
            "Copy incomplete.", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_FAILED,
              .failed_items = 1, .verification_requested = TRUE },
            "No completed files were checksum verified.", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_CANCELLED,
              .completed_items = 1, .completed_regular_files = 1, .checksum_verified_files = 1,
              .verification_requested = TRUE },
            "Copy cancelled.", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_MOVE, .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
              .completed_items = 1, .atomic_moves = 1, .verification_requested = TRUE },
            "Atomic moves (not checksum verified): 1", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
              .completed_items = 1, .completed_directories = 1, .verification_requested = TRUE },
            "No regular files were copied; checksum verification does not apply.", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
              .completed_items = 1, .completed_symlinks = 1, .verified_symlinks = 1,
              .verification_requested = TRUE },
            "Link-text verified symbolic links (completed): 1", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
              .completed_items = 2, .completed_regular_files = 2, .checksum_verified_files = 1,
              .verification_requested = TRUE },
            "SHA-256 verified files (completed): 1", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
              .completed_items = 1, .checksum_verified_files = 1, .verification_requested = TRUE },
            "SHA-256 verified files (completed): 1", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_MOVE, .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
              .completed_items = 2, .completed_regular_files = 1, .checksum_verified_files = 1,
              .atomic_moves = 1, .verification_requested = TRUE },
            "Atomic moves (not checksum verified): 1", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
              .existing_verified_regular_files = 3, .verification_requested = TRUE },
            "Existing regular files SHA-256 verified (not rewritten): 3\nAll required regular-file contents were SHA-256 verified.", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_PARTIAL,
              .existing_verified_regular_files = 3, .failed_items = 1, .verification_requested = TRUE },
            "Copy incomplete.", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_RETAINED,
              .unverified_retained_files = 3 },
            "Copy finished with existing files retained (not verified).", FALSE
        },
        {
            { .operation = NEMO_PROGRESS_OPERATION_COPY, .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
              .existing_verified_symlinks = 2, .verification_requested = TRUE },
            "Existing symbolic links link-text verified (not rewritten): 2", FALSE
        },
    };

    for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
        NemoProgressInfo *info = nemo_progress_info_new ();
        nemo_progress_info_set_result (info, &cases[i].result);
        nemo_progress_info_finish (info);
        g_autofree char *text = nemo_progress_info_get_completion_text (info);
        g_assert_nonnull (strstr (text, cases[i].expected));
        g_assert_cmpint (strstr (text, "All copied regular files were SHA-256 verified.") != NULL,
                         ==, cases[i].all_verified);
        if (cases[i].result.outcome != NEMO_PROGRESS_OUTCOME_SUCCESS)
            g_assert_null (strstr (text, "All required regular-file contents"));
        drain ();
        g_object_unref (info);
    }
}

static void
test_fresh_batch (void)
{
    NemoProgressUIHandler *handler = new_handler ();
    NemoProgressInfo *info = new_operation ();
    drain ();
    progress_window_close_clicked (NULL, handler);
    finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_false (gtk_widget_get_visible (handler->priv->progress_window));
    g_assert_cmpuint (handler->priv->completed_count, ==, 1);
    g_assert_cmpuint (notifications, ==, 1);
    g_object_unref (info);
    info = new_operation ();
    drain ();
    g_assert_true (gtk_widget_get_visible (handler->priv->progress_window));
    g_assert_false (handler->priv->should_show_status_icon);
    finish_operation (info, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_cmpuint (notifications, ==, 1);
    g_assert_cmpuint (handler->priv->completed_count, ==, 2);
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
        drain ();
        progress_window_close_clicked (NULL, handler);
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
        g_assert_nonnull (watch);
        g_assert_cmpuint (watch->timeout_id, ==, 0);
        guint summaries = handler->priv->completed_count;
        guint sent = notifications;
        nemo_progress_ui_handler_shutdown (handler);
        nemo_progress_ui_handler_shutdown (handler);
        g_assert_cmpuint (holds, ==, 0);
        g_assert_null (handler->priv->operations);
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
    schema_directory = g_path_get_dirname (argv[0]);
    removal_notifications = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
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
    g_test_add_func ("/completion/quick-visible", test_quick_visible);
    g_test_add_func ("/completion/hidden-completion", test_hidden_completion);
    g_test_add_func ("/completion/quick-generic-hidden", test_quick_generic_hidden);
    g_test_add_func ("/completion/queued-move-visible", test_queued_move_visible);
    g_test_add_func ("/completion/visible-close", test_visible_and_close);
    g_test_add_func ("/completion/keyboard-close", test_keyboard_close);
    g_test_add_func ("/completion/dialog-parent", test_dialog_parent);
    g_test_add_func ("/completion/details", test_completion_details);
    g_test_add_func ("/completion/generic-with-transfer", test_generic_with_transfer);
    g_test_add_func ("/completion/generic-visible", test_generic_visible);
    g_test_add_func ("/completion/unknown-transfer-result", test_unknown_transfer_result);
    g_test_add_func ("/completion/concurrent-close", test_concurrent_close);
    g_test_add_func ("/completion/mixed-results", test_mixed_results);
    g_test_add_func ("/completion/dispose-pending", test_dispose_pending);
    g_test_add_func ("/completion/dispose-visible-active", test_dispose_visible_active);
    g_test_add_func ("/completion/notification-reopen", test_notification_reopen);
    g_test_add_func ("/completion/history-limit", test_history_limit);
    g_test_add_func ("/completion/model-snapshot", test_model_snapshot);
    g_test_add_func ("/completion/notification-input", test_notification_input);
    g_test_add_func ("/completion/fresh-batch", test_fresh_batch);
    g_test_add_func ("/completion/verification-wording", test_verification_wording);
    g_test_add_func ("/completion/active-progress", test_active_progress);
    g_test_add_func ("/completion/aggregate-progress", test_aggregate_progress);
    g_test_add_func ("/removal/notification-lifetime", test_removal_lifetime);
    g_test_add_func ("/removal/unfinished-transfer-gate", test_removal_gate);
    g_test_add_func ("/removal/shutdown", test_removal_shutdown);
    g_test_add_func ("/removal/no-force", test_removal_no_force);
    g_test_add_func ("/removal/nemo-file-fallback", test_file_removal_fallback);
    g_test_add_func ("/preferences/verification-default", test_verification_preference);
    }
    int result = g_test_run ();
    g_clear_object (&last_notification);
    g_free (notification_body);
    g_free (notification_target);
    g_free (schema_directory);
    g_free (status_tooltip);
    g_free (status_icon_name);
    g_hash_table_unref (removal_notifications);
    g_object_unref (application);
    return result;
}
