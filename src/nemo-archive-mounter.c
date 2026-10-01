/* Read-only archive locations backed by libarchive.
 * Copyright (C) 2026 nemo-smpl contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>
#include "nemo-archive-mounter.h"

#ifdef NEMO_SMPL
#include <libnemo-private/nemo-archive-file.h>
#endif

#include <string.h>

gboolean
nemo_archive_mounter_is_archive (const char *mime_type)
{
    const char *types[] = {
        "application/zip",
        "application/x-zip-compressed",
        "application/x-tar",
        "application/x-compressed-tar",
        "application/x-bzip-compressed-tar",
        "application/x-xz-compressed-tar",
        "application/x-lzma-compressed-tar",
        "application/x-lzip-compressed-tar",
        "application/x-zstd-compressed-tar",
        "application/x-7z-compressed",
        "application/x-rar",
        "application/x-rar-compressed",
        "application/vnd.rar",
        "application/x-cd-image",
        "application/x-iso9660-image",
        "application/vnd.efi.iso",
    };

    for (guint i = 0; i < G_N_ELEMENTS (types); i++)
        if (g_strcmp0 (mime_type, types[i]) == 0)
            return TRUE;
    return FALSE;
}

GFile *
nemo_archive_mounter_get_root (GFile *archive)
{
    g_return_val_if_fail (G_IS_FILE (archive), NULL);

#ifdef NEMO_SMPL
    return nemo_archive_file_new_for_archive (archive);
#else
    g_autofree char *uri = g_file_get_uri (archive);
    /* GVfs decodes the URI authority once, then the archive backend decodes
     * its host field again to recover the backing file's complete URI. */
    g_autofree char *escaped = g_uri_escape_string (uri, NULL, FALSE);
    g_autofree char *host = g_uri_escape_string (escaped, NULL, FALSE);
    g_autofree char *root = g_strconcat ("archive://", host, "/", NULL);
    return g_file_new_for_uri (root);
#endif
}

GFile *
nemo_archive_mounter_get_archive (GFile *location)
{
    g_return_val_if_fail (G_IS_FILE (location), NULL);

#ifdef NEMO_SMPL
    GFile *archive = nemo_archive_file_get_archive (location);
    if (archive != NULL)
        return archive;
#endif
    if (!g_file_has_uri_scheme (location, "archive"))
        return NULL;
    g_autofree char *uri = g_file_get_uri (location);
    if (!g_str_has_prefix (uri, "archive://"))
        return NULL;
    const char *host = uri + strlen ("archive://");
    const char *end = strchr (host, '/');
    if (end == NULL || end == host)
        return NULL;
    g_autofree char *escaped = g_uri_unescape_segment (host, end, NULL);
    if (escaped == NULL)
        return NULL;
    g_autofree char *archive_uri = g_uri_unescape_string (escaped, NULL);
    if (archive_uri == NULL)
        return NULL;
    g_autofree char *scheme = g_uri_parse_scheme (archive_uri);
    return scheme != NULL ? g_file_new_for_uri (archive_uri) : NULL;
}
