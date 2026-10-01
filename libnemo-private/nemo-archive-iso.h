/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEMO_ARCHIVE_ISO_H
#define NEMO_ARCHIVE_ISO_H

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NemoArchiveIso NemoArchiveIso;

typedef struct {
    const char *path;
    GFileType type;
    goffset size;
    gint64 mtime;
    const char *symlink;
} NemoArchiveIsoEntry;

/* The caller owns read_data and keeps it alive until the ISO handle is freed.
 * Callbacks read exactly the requested bytes, or return a cancellable error. */
typedef gboolean (*NemoArchiveIsoReadAt) (gpointer read_data, goffset offset,
                                          void *buffer, gsize count,
                                          GCancellable *cancellable, GError **error);
typedef gboolean (*NemoArchiveIsoVisit) (const NemoArchiveIsoEntry *entry,
                                        gpointer user_data, GCancellable *cancellable,
                                        GError **error);

NemoArchiveIso *nemo_archive_iso_new     (NemoArchiveIsoReadAt read_at, gpointer read_data,
                                         GCancellable *cancellable, GError **error);
void            nemo_archive_iso_free  (NemoArchiveIso *iso);
gboolean        nemo_archive_iso_walk  (NemoArchiveIso *iso, NemoArchiveIsoVisit visit,
                                         gpointer user_data, gsize max_path, guint max_depth,
                                         GCancellable *cancellable, GError **error);
gboolean        nemo_archive_iso_open  (NemoArchiveIso *iso, const char *path,
                                         GCancellable *cancellable, GError **error);
gssize          nemo_archive_iso_read  (NemoArchiveIso *iso, void *buffer, gsize count,
                                         GCancellable *cancellable, GError **error);
gboolean        nemo_archive_iso_seek  (NemoArchiveIso *iso, goffset offset,
                                         GCancellable *cancellable, GError **error);

G_END_DECLS
#endif
