#ifndef NEMO_ARCHIVE_CREATE_H
#define NEMO_ARCHIVE_CREATE_H

#include <gio/gio.h>
#include <gtk/gtk.h>
#include "nemo-progress-info.h"

G_BEGIN_DECLS

typedef struct {
    /* published includes an uncertain/partially changed destination namespace. */
    gboolean published;
    /* All members independently verified and publication synchronized. */
    gboolean archive_ok;
    gboolean sources_removed;
    guint64 archived_items;
    guint64 removed_items;
} NemoArchiveCreateResult;

/* Main-thread entry point; sources are referenced, never consumed. Only .7z
 * output is supported. Cancellation is through the returned progress object.
 * The caller owns the returned reference and must hold the application alive
 * until callback. All scanning, compression, verification and cleanup run off
 * the main thread. callback runs in the calling thread's main context. */
NemoProgressInfo *nemo_archive_create_async (GList *sources,
                                            GFile *destination,
                                            gboolean move,
                                            gboolean overwrite,
                                            GtkWindow *parent,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);

/* Always fills outcome, including on FALSE; FALSE does not imply that the
 * destination is unchanged or that no sources have been removed. Exact
 * recovery/retention information is also recorded in the progress details. */
gboolean nemo_archive_create_finish (GAsyncResult *result,
                                     NemoArchiveCreateResult *outcome,
                                     GError **error);

G_END_DECLS
#endif
