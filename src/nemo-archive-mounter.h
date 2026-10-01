/* nemo-archive-mounter.h - Read-only archive locations
 *
 * Copyright (C) 2026 nemo-smpl contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#ifndef NEMO_ARCHIVE_MOUNTER_H
#define NEMO_ARCHIVE_MOUNTER_H

#include <gio/gio.h>

G_BEGIN_DECLS

gboolean nemo_archive_mounter_is_archive (const gchar *mime_type);

/* URI-only helpers. Normal slot navigation loads the root asynchronously. */
GFile *nemo_archive_mounter_get_root (GFile *archive);
GFile *nemo_archive_mounter_get_archive (GFile *location);

G_END_DECLS

#endif /* NEMO_ARCHIVE_MOUNTER_H */
