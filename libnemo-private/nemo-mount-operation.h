/* Shared async removal routing and feedback. */
#ifndef NEMO_MOUNT_OPERATION_H
#define NEMO_MOUNT_OPERATION_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef enum {
    NEMO_MOUNT_REMOVE_UNMOUNT,
    NEMO_MOUNT_REMOVE_EJECT,
    NEMO_MOUNT_REMOVE_STOP
} NemoMountRemoval;

/* Start on the main thread. The target is a GMount, GVolume, GDrive or GFile.
 * The callback always runs, including when the safety gate refuses removal. */
void nemo_mount_operation_remove (GObject *target,
                                  NemoMountRemoval removal,
                                  GMountOperation *mount_op,
                                  GCancellable *cancellable,
                                  GAsyncReadyCallback callback,
                                  gpointer user_data);
gboolean nemo_mount_operation_remove_finish (GObject *target,
                                             GAsyncResult *result,
                                             GError **error);

/* Atomic-safe. New transfers must not start while any removal is pending. */
gboolean nemo_mount_operation_is_removing (void);
gboolean nemo_mount_operation_check_transfers (GError **error);
/* Main-thread shutdown: withdraw pending feedback without cancelling removal. */
void nemo_mount_operation_shutdown (void);

G_END_DECLS
#endif
