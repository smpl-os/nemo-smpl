#include <config.h>
#include <glib/gi18n.h>
#include "nemo-mount-operation.h"
#include "nemo-progress-info-manager.h"

typedef struct {
    GMountOperation *mount_op;
    GApplication *application;
    NemoMountRemoval removal;
    char *name;
    char *notification_id;
    gulong progress_handler;
    gulong aborted_handler;
    gulong processes_handler;
    gboolean busy;
    gboolean notifications_closed;
} Removal;

static gint pending_removals;
static GList *removals;

gboolean
nemo_mount_operation_is_removing (void)
{
    return g_atomic_int_get (&pending_removals) != 0;
}

gboolean
nemo_mount_operation_check_transfers (GError **error)
{
#ifdef NEMO_SMPL
    NemoProgressInfoManager *manager = nemo_progress_info_manager_new ();
    GList *infos = nemo_progress_info_manager_get_all_infos (manager);
    gboolean active = FALSE;

    for (GList *l = infos; l != NULL; l = l->next) {
        if (!nemo_progress_info_get_is_finished (l->data)) {
            active = TRUE;
            break;
        }
    }
    g_object_unref (manager);
    if (active) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                             _("Device removal is blocked while file operations are unfinished. "
                               "Wait for all operations, including queued or paused transfers, to finish, "
                               "or cancel them and wait for cancellation to complete. No files were cancelled automatically."));
        return FALSE;
    }
#endif
    return TRUE;
}

static void
removal_notify (Removal *data, const char *title, const char *message)
{
    if (data->application != NULL && !data->notifications_closed) {
        GNotification *notification = g_notification_new (title);
        GIcon *icon = g_themed_icon_new ("media-removable");
        g_autofree char *body = g_strdup_printf ("%s\n%s", data->name, message);
        g_notification_set_body (notification, body);
        g_notification_set_icon (notification, icon);
        g_application_send_notification (data->application, data->notification_id, notification);
        g_object_unref (icon);
        g_object_unref (notification);
    }
}

static void
removal_progress (GMountOperation *op, const char *message,
                  gint64 time_left, gint64 bytes_left, Removal *data)
{
    /* A backend's zero-byte update precedes its async finish and can still
     * be followed by an error. Never relay it as permission to disconnect. */
    removal_notify (data, _("Preparing device removal"),
                    bytes_left == 0 ?
                    _("Waiting for the device removal result. Do not disconnect the device yet.") :
                    _("The system is finishing device operations. Do not disconnect the device yet."));
}

static void
removal_aborted (GMountOperation *op, Removal *data)
{
    /* This signal ends mount-operation interaction, not the async request. */
    removal_notify (data, _("Waiting for device removal"),
                    _("Waiting for the device removal result. Do not disconnect the device yet."));
}

#ifdef NEMO_SMPL
static void
removal_processes (GMountOperation *op, const char *message, GArray *processes,
                   const char **choices, Removal *data)
{
    data->busy = TRUE;
    g_signal_stop_emission_by_name (op, "show-processes");
    g_mount_operation_reply (op, G_MOUNT_OPERATION_ABORTED);
}
#endif

static void
removal_disconnect (Removal *data)
{
    if (data->progress_handler != 0) {
        g_signal_handler_disconnect (data->mount_op, data->progress_handler);
        data->progress_handler = 0;
    }
    if (data->aborted_handler != 0) {
        g_signal_handler_disconnect (data->mount_op, data->aborted_handler);
        data->aborted_handler = 0;
    }
    if (data->processes_handler != 0) {
        g_signal_handler_disconnect (data->mount_op, data->processes_handler);
        data->processes_handler = 0;
    }
}

void
nemo_mount_operation_shutdown (void)
{
    for (GList *l = removals; l != NULL; l = l->next) {
        Removal *data = l->data;
        if (data->application != NULL && !data->notifications_closed)
            g_application_withdraw_notification (data->application, data->notification_id);
        data->notifications_closed = TRUE;
    }
}

static void
removal_free (gpointer user_data)
{
    Removal *data = user_data;
    removal_disconnect (data);
    removals = g_list_remove (removals, data);
    g_clear_object (&data->mount_op);
    if (data->application != NULL) {
        g_application_release (data->application);
        g_object_unref (data->application);
    }
    g_free (data->name);
    g_free (data->notification_id);
    g_free (data);
}

static void
removal_done (GObject *target, GAsyncResult *result, gpointer user_data)
{
    GTask *task = user_data;
    Removal *data = g_task_get_task_data (task);
    GError *error = NULL;
    gboolean success = FALSE;

    if (G_IS_MOUNT (target)) {
        success = data->removal == NEMO_MOUNT_REMOVE_EJECT ?
            g_mount_eject_with_operation_finish (G_MOUNT (target), result, &error) :
            g_mount_unmount_with_operation_finish (G_MOUNT (target), result, &error);
    } else if (G_IS_VOLUME (target)) {
        success = g_volume_eject_with_operation_finish (G_VOLUME (target), result, &error);
    } else if (G_IS_DRIVE (target)) {
        success = data->removal == NEMO_MOUNT_REMOVE_STOP ?
            g_drive_stop_finish (G_DRIVE (target), result, &error) :
            g_drive_eject_with_operation_finish (G_DRIVE (target), result, &error);
    } else if (G_IS_FILE (target)) {
        if (data->removal == NEMO_MOUNT_REMOVE_STOP)
            success = g_file_stop_mountable_finish (G_FILE (target), result, &error);
        else if (data->removal == NEMO_MOUNT_REMOVE_EJECT)
            success = g_file_eject_mountable_with_operation_finish (G_FILE (target), result, &error);
        else
            success = g_file_unmount_mountable_with_operation_finish (G_FILE (target), result, &error);
    }

    removal_disconnect (data);
    removals = g_list_remove (removals, data);
    g_atomic_int_add (&pending_removals, -1);
    if (success) {
        removal_notify (data, _("Device removal completed"),
                        data->removal == NEMO_MOUNT_REMOVE_UNMOUNT ?
                        _("The volume is unmounted. Other volumes on this device may still be in use. "
                          "Eject or Safely Remove the whole device before unplugging it.") :
                        _("The system completed the removal request. Disconnect only if no other volumes on this device remain mounted."));
        g_task_return_boolean (task, TRUE);
    } else {
        if (data->busy) {
            g_clear_error (&error);
            error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_BUSY,
                                         _("The device is still in use. Close files and applications using it, "
                                           "then try again. Forced removal is disabled to protect your data."));
        }
        if (error == NULL)
            error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED,
                                         _("The system did not confirm device removal."));
        removal_notify (data, g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ?
                        _("Device removal cancelled") : _("Device removal failed"),
                        _("Removal was not completed. Do not disconnect the device."));
        g_task_return_error (task, error);
    }
    g_object_unref (task);
}

void
nemo_mount_operation_remove (GObject *target, NemoMountRemoval removal,
                             GMountOperation *mount_op, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (target, cancellable, callback, user_data);
    GError *error = NULL;
    Removal *data;
    gboolean supported = (G_IS_MOUNT (target) && removal != NEMO_MOUNT_REMOVE_STOP) ||
                         (G_IS_VOLUME (target) && removal == NEMO_MOUNT_REMOVE_EJECT) ||
                         (G_IS_DRIVE (target) && removal != NEMO_MOUNT_REMOVE_UNMOUNT) ||
                         G_IS_FILE (target);

    g_task_set_source_tag (task, nemo_mount_operation_remove);
    /* Preserve the backend's actual result even if cancellation arrives after
     * a successful removal, rather than converting that success to an error. */
    g_task_set_check_cancellable (task, FALSE);
    if (!supported) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                 _("This location does not support the requested removal operation."));
        g_object_unref (task);
        return;
    }
    if (g_cancellable_set_error_if_cancelled (cancellable, &error) ||
        !nemo_mount_operation_check_transfers (&error)) {
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    data = g_new0 (Removal, 1);
    data->removal = removal;
    data->mount_op = mount_op != NULL ? g_object_ref (mount_op) : gtk_mount_operation_new (NULL);
    data->notification_id = g_uuid_string_random ();
    if (G_IS_MOUNT (target))
        data->name = g_mount_get_name (G_MOUNT (target));
    else if (G_IS_VOLUME (target))
        data->name = g_volume_get_name (G_VOLUME (target));
    else if (G_IS_DRIVE (target))
        data->name = g_drive_get_name (G_DRIVE (target));
    else
        data->name = g_file_get_parse_name (G_FILE (target));
    GApplication *application = g_application_get_default ();
    if (application != NULL && g_application_get_is_registered (application)) {
        data->application = g_object_ref (application);
        g_application_hold (application);
    }
    g_task_set_task_data (task, data, removal_free);
    removals = g_list_prepend (removals, data);
    data->progress_handler = g_signal_connect (data->mount_op, "show-unmount-progress",
                                               G_CALLBACK (removal_progress), data);
    data->aborted_handler = g_signal_connect (data->mount_op, "aborted",
                                              G_CALLBACK (removal_aborted), data);
#ifdef NEMO_SMPL
    /* Do not offer GTK's force-unmount choice for a busy device. */
    data->processes_handler = g_signal_connect (data->mount_op, "show-processes",
                                                G_CALLBACK (removal_processes), data);
#endif
    g_atomic_int_inc (&pending_removals);
    removal_notify (data, _("Preparing device removal"),
                    _("Do not disconnect the device until the removal operation completes."));

    if (G_IS_MOUNT (target)) {
        if (removal == NEMO_MOUNT_REMOVE_EJECT)
            g_mount_eject_with_operation (G_MOUNT (target), G_MOUNT_UNMOUNT_NONE, data->mount_op,
                                          cancellable, removal_done, task);
        else
            g_mount_unmount_with_operation (G_MOUNT (target), G_MOUNT_UNMOUNT_NONE, data->mount_op,
                                            cancellable, removal_done, task);
    } else if (G_IS_VOLUME (target)) {
        g_volume_eject_with_operation (G_VOLUME (target), G_MOUNT_UNMOUNT_NONE, data->mount_op,
                                       cancellable, removal_done, task);
    } else if (G_IS_DRIVE (target)) {
        if (removal == NEMO_MOUNT_REMOVE_STOP)
            g_drive_stop (G_DRIVE (target), G_MOUNT_UNMOUNT_NONE, data->mount_op,
                           cancellable, removal_done, task);
        else
            g_drive_eject_with_operation (G_DRIVE (target), G_MOUNT_UNMOUNT_NONE, data->mount_op,
                                          cancellable, removal_done, task);
    } else {
        if (removal == NEMO_MOUNT_REMOVE_STOP)
            g_file_stop_mountable (G_FILE (target), G_MOUNT_UNMOUNT_NONE, data->mount_op,
                                   cancellable, removal_done, task);
        else if (removal == NEMO_MOUNT_REMOVE_EJECT)
            g_file_eject_mountable_with_operation (G_FILE (target), G_MOUNT_UNMOUNT_NONE, data->mount_op,
                                                   cancellable, removal_done, task);
        else
            g_file_unmount_mountable_with_operation (G_FILE (target), G_MOUNT_UNMOUNT_NONE, data->mount_op,
                                                     cancellable, removal_done, task);
    }
}

gboolean
nemo_mount_operation_remove_finish (GObject *target, GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (g_task_is_valid (result, target), FALSE);
    g_return_val_if_fail (g_async_result_is_tagged (result, nemo_mount_operation_remove), FALSE);
    return g_task_propagate_boolean (G_TASK (result), error);
}
