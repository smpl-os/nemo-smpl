/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#include "nemo-archive-iso.h"

#include <stdint.h>
#include <libisofs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct _NemoArchiveIso {
    IsoImageFilesystem *filesystem;
    IsoDataSource *source;
    IsoFileSource *member;
    NemoArchiveIsoReadAt read_at;
    gpointer read_data;
    GCancellable *cancellable;
    GError *read_error;
};

/* libisofs has process-wide counters and message/charset configuration.
 * Serialize library execution, but never hold this lock over a GIO or SQLite
 * callback. Recursive entry permits archives backed by another ISO member. */
static GRecMutex library_mutex;
static GPrivate library_depth = G_PRIVATE_INIT (NULL);

static gboolean
library_enter (GCancellable *cancellable, GError **error)
{
    while (!g_rec_mutex_trylock (&library_mutex)) {
        if (g_cancellable_set_error_if_cancelled (cancellable, error))
            return FALSE;
        g_usleep (1000);
    }
    g_private_set (&library_depth,
                   GUINT_TO_POINTER (GPOINTER_TO_UINT (g_private_get (&library_depth)) + 1));
    return TRUE;
}

static void
library_leave (void)
{
    g_private_set (&library_depth,
                   GUINT_TO_POINTER (GPOINTER_TO_UINT (g_private_get (&library_depth)) - 1));
    g_rec_mutex_unlock (&library_mutex);
}

static guint
library_suspend (void)
{
    guint depth = GPOINTER_TO_UINT (g_private_get (&library_depth));
    for (guint i = 0; i < depth; i++)
        library_leave ();
    return depth;
}

static void
library_resume (guint depth)
{
    for (guint i = 0; i < depth; i++)
        library_enter (NULL, NULL);
}

static gboolean
iso_error (NemoArchiveIso *iso, int status, GError **error)
{
    if (iso && iso->read_error)
        g_propagate_error (error, g_steal_pointer (&iso->read_error));
    else if (!iso || !g_cancellable_set_error_if_cancelled (iso->cancellable, error))
        g_set_error (error, G_IO_ERROR,
                     status == (int) ISO_FILE_DOESNT_EXIST ? G_IO_ERROR_NOT_FOUND :
                     status == (int) ISO_OUT_OF_MEM ? G_IO_ERROR_NO_SPACE : G_IO_ERROR_INVALID_DATA,
                     "Cannot read ISO filesystem: %s", iso_error_to_msg (status));
    return FALSE;
}

static int
source_open (IsoDataSource *source)
{
    return 1;
}

static int
source_close (IsoDataSource *source)
{
    return 1;
}

static void
source_free (IsoDataSource *source)
{
}

static int
source_read (IsoDataSource *source, uint32_t block, uint8_t *buffer)
{
    NemoArchiveIso *iso = source->data;
    guint depth = library_suspend ();
    gboolean success;
    if (iso->read_error)
        success = FALSE;
    else
        success = iso->read_at (iso->read_data, (goffset) block * 2048, buffer, 2048,
                                iso->cancellable, &iso->read_error);
    library_resume (depth);
    return success ? 1 : ISO_DATA_SOURCE_FAILURE;
}

static gboolean
operation_begin (NemoArchiveIso *iso, GCancellable *cancellable, GError **error)
{
    if (g_cancellable_set_error_if_cancelled (cancellable, error) ||
        !library_enter (cancellable, error))
        return FALSE;
    iso->cancellable = cancellable;
    g_clear_error (&iso->read_error);
    return TRUE;
}

static void
operation_end (NemoArchiveIso *iso)
{
    iso->cancellable = NULL;
    library_leave ();
}

NemoArchiveIso *
nemo_archive_iso_new (NemoArchiveIsoReadAt read_at, gpointer read_data,
                      GCancellable *cancellable, GError **error)
{
    static gboolean initialized;
    NemoArchiveIso *iso = g_new0 (NemoArchiveIso, 1);
    IsoReadOpts *options = NULL;
    int status;
    iso->read_at = read_at;
    iso->read_data = read_data;
    if (!operation_begin (iso, cancellable, error))
        goto failed;
    if (!initialized) {
        /* In particular, never let libisofs call process-global setlocale(). */
        status = iso_init_with_flag (1);
        if (status < 0 || iso->read_error) {
            iso_error (iso, status, error);
            goto unlock;
        }
        if (iso_set_local_charset ("UTF-8", 0) <= 0) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                 "Cannot configure UTF-8 ISO filenames");
            goto unlock;
        }
        iso_set_msgs_severities ("NEVER", "NEVER", "nemo");
        iso_set_abort_severity ("WARNING");
        initialized = TRUE;
    }
    iso->source = g_new0 (IsoDataSource, 1);
    iso->source->refcount = 1;
    iso->source->open = source_open;
    iso->source->close = source_close;
    iso->source->read_block = source_read;
    iso->source->free_data = source_free;
    iso->source->data = iso;
    status = iso_read_opts_new (&options, 0);
    if (status < 0) {
        iso_error (iso, status, error);
        goto unlock;
    }
    if ((status = iso_read_opts_set_input_charset (options, "UTF-8")) < 0 ||
        (status = iso_read_opts_set_no_aaip (options, 1)) < 0 ||
        (status = iso_read_opts_set_no_md5 (options, 1)) < 0 ||
        (status = iso_read_opts_load_system_area (options, 0)) < 0) {
        iso_error (iso, status, error);
        goto unlock;
    }
    status = iso_image_filesystem_new (iso->source, options, 0x1fffff, &iso->filesystem);
    iso_read_opts_free (options);
    options = NULL;
    if (status < 0 || iso->read_error) {
        iso_error (iso, status, error);
        goto unlock;
    }
    operation_end (iso);
    return iso;

unlock:
    if (options)
        iso_read_opts_free (options);
    operation_end (iso);
failed:
    nemo_archive_iso_free (iso);
    return NULL;
}

void
nemo_archive_iso_free (NemoArchiveIso *iso)
{
    if (!iso)
        return;
    library_enter (NULL, NULL);
    if (iso->member) {
        iso_file_source_close (iso->member);
        iso_file_source_unref (iso->member);
    }
    if (iso->filesystem)
        iso_filesystem_unref (iso->filesystem);
    if (iso->source)
        iso_data_source_unref (iso->source);
    library_leave ();
    g_clear_error (&iso->read_error);
    g_free (iso);
}

static gboolean
walk_source (NemoArchiveIso *iso, IsoFileSource *source, const char *path,
             guint depth, gsize max_path, guint max_depth,
             NemoArchiveIsoVisit visit, gpointer data, GError **error)
{
    struct stat attributes;
    NemoArchiveIsoEntry entry = { 0 };
    char *link = NULL;
    int status;
    guint lock_depth;
    gboolean success;
    if (g_cancellable_set_error_if_cancelled (iso->cancellable, error))
        return FALSE;
    if (depth > max_depth || strlen (path) > max_path) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "ISO member exceeds the path or directory-depth limit");
        return FALSE;
    }
    status = iso_file_source_lstat (source, &attributes);
    if (status < 0)
        return iso_error (iso, status, error);
    entry.path = path;
    entry.type = S_ISDIR (attributes.st_mode) ? G_FILE_TYPE_DIRECTORY :
                 S_ISREG (attributes.st_mode) ? G_FILE_TYPE_REGULAR :
                 S_ISLNK (attributes.st_mode) ? G_FILE_TYPE_SYMBOLIC_LINK : G_FILE_TYPE_SPECIAL;
    entry.size = attributes.st_size;
    entry.mtime = attributes.st_mtime;
    if (entry.type == G_FILE_TYPE_SYMBOLIC_LINK) {
        link = g_malloc (max_path + 1);
        status = iso_file_source_readlink (source, link, max_path + 1);
        if (status < 0) {
            g_free (link);
            return iso_error (iso, status, error);
        }
        entry.symlink = link;
    }
    lock_depth = library_suspend ();
    success = visit (&entry, data, iso->cancellable, error);
    library_resume (lock_depth);
    g_free (link);
    if (!success || entry.type != G_FILE_TYPE_DIRECTORY)
        return success;
    status = iso_file_source_open (source);
    if (status < 0)
        return iso_error (iso, status, error);
    for (;;) {
        IsoFileSource *child = NULL;
        char *name, *child_path;
        if (g_cancellable_set_error_if_cancelled (iso->cancellable, error)) {
            success = FALSE;
            break;
        }
        status = iso_file_source_readdir (source, &child);
        if (status == 0)
            break;
        if (status < 0) {
            success = iso_error (iso, status, error);
            break;
        }
        name = iso_file_source_get_name (child);
        if (!name || !*name || strlen (name) > max_path || strchr (name, '/') ||
            strcmp (name, ".") == 0 || strcmp (name, "..") == 0 ||
            !g_utf8_validate (name, -1, NULL)) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 "ISO filesystem contains an invalid member name");
            free (name);
            iso_file_source_unref (child);
            success = FALSE;
            break;
        }
        child_path = *path ? g_strconcat (path, "/", name, NULL) : g_strdup (name);
        free (name);
        success = walk_source (iso, child, child_path, depth + 1, max_path, max_depth,
                                visit, data, error);
        g_free (child_path);
        iso_file_source_unref (child);
        if (!success)
            break;
    }
    status = iso_file_source_close (source);
    if (status < 0 && success)
        success = iso_error (iso, status, error);
    return success;
}

gboolean
nemo_archive_iso_walk (NemoArchiveIso *iso, NemoArchiveIsoVisit visit, gpointer data,
                       gsize max_path, guint max_depth, GCancellable *cancellable, GError **error)
{
    IsoFileSource *root = NULL;
    gboolean success;
    int status;
    if (!operation_begin (iso, cancellable, error))
        return FALSE;
    status = iso->filesystem->get_root (iso->filesystem, &root);
    success = status < 0 ? iso_error (iso, status, error) :
              walk_source (iso, root, "", 0, max_path, max_depth, visit, data, error);
    if (root)
        iso_file_source_unref (root);
    if (success && iso->read_error)
        success = iso_error (iso, (int) ISO_DATA_SOURCE_FAILURE, error);
    operation_end (iso);
    return success;
}

gboolean
nemo_archive_iso_open (NemoArchiveIso *iso, const char *path,
                       GCancellable *cancellable, GError **error)
{
    static const guint8 zisofs_magic[] = { 0x37, 0xe4, 0x53, 0x96, 0xc9, 0xdb, 0xd6, 0x07 };
    static const guint8 zisofs2_magic[] = { 0xef, 0x22, 0x55, 0xa1, 0xbc, 0x1b, 0x95, 0xa0 };
    char *absolute = g_strconcat ("/", path, NULL);
    IsoFileSource *member = NULL;
    struct stat attributes;
    gboolean success = FALSE, opened = FALSE;
    guint8 magic[8];
    int status;
    if (!operation_begin (iso, cancellable, error)) {
        g_free (absolute);
        return FALSE;
    }
    if (iso->member) {
        g_free (absolute);
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PENDING,
                             "An ISO member is already open on this reader");
        operation_end (iso);
        return FALSE;
    }
    status = iso->filesystem->get_by_path (iso->filesystem, absolute, &member);
    g_free (absolute);
    if (status < 0 || (status = iso_file_source_lstat (member, &attributes)) < 0) {
        iso_error (iso, status, error);
        goto done;
    }
    if (!S_ISREG (attributes.st_mode)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Only regular ISO members can be read");
        goto done;
    }
    status = iso_file_source_open (member);
    if (status < 0) {
        iso_error (iso, status, error);
        goto done;
    }
    opened = TRUE;
    /* Low-level IsoFileSource exposes on-disc bytes for zisofs. Do not return
     * compressed bytes as if they were the logical file's contents. */
    status = iso_file_source_read (member, magic, sizeof magic);
    if (status < 0) {
        iso_error (iso, status, error);
        goto done;
    }
    if (status == sizeof magic &&
        (memcmp (magic, zisofs_magic, sizeof magic) == 0 ||
         memcmp (magic, zisofs2_magic, sizeof magic) == 0)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Compressed zisofs members are not supported by the ISO reader");
        goto done;
    }
    {
        off_t position = iso_file_source_lseek (member, 0, SEEK_SET);
        if (position < 0) {
            iso_error (iso, (int) position, error);
            goto done;
        }
    }
    iso->member = member;
    member = NULL;
    success = TRUE;
done:
    if (member) {
        if (opened)
            iso_file_source_close (member);
        iso_file_source_unref (member);
    }
    if (success && iso->read_error)
        success = iso_error (iso, (int) ISO_DATA_SOURCE_FAILURE, error);
    operation_end (iso);
    return success;
}

gssize
nemo_archive_iso_read (NemoArchiveIso *iso, void *buffer, gsize count,
                       GCancellable *cancellable, GError **error)
{
    int result;
    if (!count)
        return 0;
    if (!operation_begin (iso, cancellable, error))
        return -1;
    result = iso_file_source_read (iso->member, buffer, MIN (count, G_MAXINT));
    if (result < 0 || iso->read_error) {
        iso_error (iso, result, error);
        result = -1;
    }
    operation_end (iso);
    return result;
}

gboolean
nemo_archive_iso_seek (NemoArchiveIso *iso, goffset offset,
                       GCancellable *cancellable, GError **error)
{
    off_t position;
    gboolean success;
    if (!operation_begin (iso, cancellable, error))
        return FALSE;
    position = iso_file_source_lseek (iso->member, offset, SEEK_SET);
    success = position >= 0 && !iso->read_error;
    if (!success)
        iso_error (iso, (int) position, error);
    operation_end (iso);
    return success;
}
