/* Descriptor-bound transfer transactions. Private to the SMPL engine. */
#ifndef NEMO_TRANSFER_SAFETY_H
#define NEMO_TRANSFER_SAFETY_H

#include <config.h>
#include <gio/gio.h>

#ifdef NEMO_SMPL
typedef struct _NemoTransferGuard NemoTransferGuard;
typedef struct _NemoTransferTransaction NemoTransferTransaction;
typedef struct _NemoTransferUndo NemoTransferUndo;

NemoTransferGuard *nemo_transfer_guard_new (GList *sources, GFile *destination,
                                          gboolean move);
NemoTransferGuard *nemo_transfer_guard_ref (NemoTransferGuard *guard);
void nemo_transfer_guard_unref (NemoTransferGuard *guard);
gboolean nemo_transfer_guard_check (NemoTransferGuard *guard, GError **error);
gboolean nemo_transfer_guard_release (NemoTransferGuard *guard, GError **error);
gboolean nemo_transfer_guard_can_copy_fallback (NemoTransferGuard *guard);
GFile *nemo_transfer_guard_file (NemoTransferGuard *guard, GFile *file,
                               gboolean destination, GError **error);
GFile *nemo_transfer_guard_directory (NemoTransferGuard *guard, GFile *directory,
                                    gboolean destination, GError **error);
gboolean nemo_transfer_retire_directory (NemoTransferGuard *guard, GFile *source,
                                        GCancellable *cancellable, GError **error);
gboolean nemo_transfer_guard_expect (NemoTransferGuard *guard, GFile *destination,
                                    GError **error);
char *nemo_transfer_guard_take_details (NemoTransferGuard *guard);
void nemo_transfer_guard_describe_destination (NemoTransferGuard *guard, GFile *destination);

NemoTransferTransaction *nemo_transfer_transaction_new (NemoTransferGuard *guard,
                                                       GFile *source, GFile *destination,
                                                       GCancellable *cancellable,
                                                       GError **error);
GFile *nemo_transfer_transaction_source (NemoTransferTransaction *transaction);
GFile *nemo_transfer_transaction_destination (NemoTransferTransaction *transaction);
GFile *nemo_transfer_transaction_stage (NemoTransferTransaction *transaction);
gboolean nemo_transfer_transaction_stage_created (NemoTransferTransaction *transaction,
                                                  GError **error);
gboolean nemo_transfer_transaction_publish (NemoTransferTransaction *transaction,
                                           gboolean overwrite, gboolean *published,
                                           GCancellable *cancellable, GError **error);
gboolean nemo_transfer_transaction_retire_source (NemoTransferTransaction *transaction,
                                                 GCancellable *cancellable, GError **error);
gboolean nemo_transfer_transaction_finish (NemoTransferTransaction *transaction, GError **error);
NemoTransferUndo *nemo_transfer_transaction_undo (NemoTransferTransaction *transaction,
                                               gboolean move);
void nemo_transfer_transaction_free (NemoTransferTransaction *transaction);

gboolean nemo_transfer_native_move (NemoTransferGuard *guard, GFile *source,
                                   GFile *destination, gboolean overwrite,
                                   gboolean *published, NemoTransferUndo **undo,
                                   GCancellable *cancellable, GError **error);
gboolean nemo_transfer_sync_existing (NemoTransferGuard *guard, GFile *destination,
                                     GCancellable *cancellable, GError **error);

NemoTransferUndo *nemo_transfer_undo_ref (NemoTransferUndo *undo);
void nemo_transfer_undo_unref (NemoTransferUndo *undo);
char *nemo_transfer_undo_take_details (NemoTransferUndo *undo);
gboolean nemo_transfer_undo_release (NemoTransferUndo *undo, GError **error);
gboolean nemo_transfer_undo_check (NemoTransferUndo *undo, gboolean redo,
                                 GCancellable *cancellable, GError **error);
gboolean nemo_transfer_undo_apply (NemoTransferUndo *undo, gboolean redo,
                                 GCancellable *cancellable, GError **error);

#endif
#endif
