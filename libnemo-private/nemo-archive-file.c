/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "nemo-archive-file.h"
#include "nemo-archive-iso.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SCHEME "nemo-archive"
#define URI_PREFIX SCHEME "://"
#define MAX_PATH_BYTES (64 * 1024)
#define MAX_DEPTH 128
#define MAX_NESTING 16
#define MAX_RETAINED_INDEXES 16
#define MAX_RETAINED_BYTES (G_GUINT64_CONSTANT (4) * 1024 * 1024 * 1024)
#define ID_ATTRIBUTES "standard::size,etag::value,time::modified,time::modified-usec,time::modified-nsec,id::file"
#ifndef G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC
#define G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC "time::modified-nsec"
#endif

typedef struct _Index Index;
typedef struct _ArchiveCache ArchiveCache;

typedef struct {
    GObject parent;
    GFile *archive;
    char *path;
    char *uri;
    ArchiveCache *cache;
    gboolean valid;
} NemoArchiveFile;

typedef GObjectClass NemoArchiveFileClass;

typedef struct {
    char *path;
    GFileInfo *info;
    gint64 ordinal;
    gboolean hardlink;
} Member;

struct _Index {
    gint references;
    sqlite3 *database;
    GMutex mutex;
    char *identity;
    guint64 disk_bytes;
    gboolean is_iso;
};

struct _ArchiveCache {
    guint references;
    char *uri;
    GMutex mutex;
    GCond changed;
    gboolean building;
    Index *index;
};

/* The registry is weak: only live files/streams own cache references. No lock
 * here is held during source access, SQLite operations, or index destruction. */
static GMutex cache_registry_mutex;
static GHashTable *cache_registry;
static GQueue retained_indexes = G_QUEUE_INIT;
static guint64 retained_bytes;
static void index_free (Index *index);

/* cache_registry_mutex protects completed-index ownership and the LRU.
 * Return the removed reference for destruction AFTER releasing all locks. */
static Index *
cache_detach_index (ArchiveCache *cache)
{
    Index *index = cache->index;
    if (index) {
        cache->index = NULL;
        g_queue_remove (&retained_indexes, cache);
        retained_bytes -= index->disk_bytes;
    }
    return index;
}

static ArchiveCache *
cache_acquire (GFile *archive)
{
    char *uri = g_file_get_uri (archive);
    ArchiveCache *cache;
    g_mutex_lock (&cache_registry_mutex);
    if (!cache_registry)
        cache_registry = g_hash_table_new (g_str_hash, g_str_equal);
    cache = g_hash_table_lookup (cache_registry, uri);
    if (!cache) {
        cache = g_new0 (ArchiveCache, 1);
        cache->uri = g_strdup (uri);
        g_mutex_init (&cache->mutex);
        g_cond_init (&cache->changed);
        g_hash_table_insert (cache_registry, cache->uri, cache);
    }
    cache->references++;
    g_mutex_unlock (&cache_registry_mutex);
    g_free (uri);
    return cache;
}

static ArchiveCache *
cache_ref (ArchiveCache *cache)
{
    g_mutex_lock (&cache_registry_mutex);
    cache->references++;
    g_mutex_unlock (&cache_registry_mutex);
    return cache;
}

static void
cache_release (ArchiveCache *cache)
{
    gboolean destroy;
    Index *index = NULL;
    if (!cache)
        return;
    g_mutex_lock (&cache_registry_mutex);
    destroy = --cache->references == 0;
    if (destroy) {
        index = cache_detach_index (cache);
        g_hash_table_remove (cache_registry, cache->uri);
        if (g_hash_table_size (cache_registry) == 0)
            g_clear_pointer (&cache_registry, g_hash_table_unref);
    }
    g_mutex_unlock (&cache_registry_mutex);
    if (destroy) {
        index_free (index);
        g_mutex_clear (&cache->mutex);
        g_cond_clear (&cache->changed);
        g_free (cache->uri);
        g_free (cache);
    }
}

typedef struct {
    struct archive *archive;
    GFile *file;
    GFileInputStream *input;
    GCancellable *cancellable;
    GError *error;
    char *identity;
    char *stream_identity;
    goffset size;
    goffset offset;
    guint8 buffer[64 * 1024];
} Reader;

typedef struct {
    GFileEnumerator parent;
    Index *index;
    sqlite3_stmt *statement;
} NemoArchiveEnumerator;

typedef GFileEnumeratorClass NemoArchiveEnumeratorClass;

typedef struct {
    GFileInputStream parent;
    Reader *reader;
    GFileInfo *info;
    goffset position;
    gint64 ordinal;
    ArchiveCache *cache;
    NemoArchiveIso *iso;
    char *iso_path;
    gboolean failed;
} NemoArchiveInput;

typedef GFileInputStreamClass NemoArchiveInputClass;

static void archive_file_iface_init (GFileIface *iface);
G_DEFINE_TYPE_WITH_CODE (NemoArchiveFile, nemo_archive_file, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE (G_TYPE_FILE, archive_file_iface_init))
G_DEFINE_TYPE (NemoArchiveEnumerator, nemo_archive_enumerator, G_TYPE_FILE_ENUMERATOR)
G_DEFINE_TYPE (NemoArchiveInput, nemo_archive_input, G_TYPE_FILE_INPUT_STREAM)

static gboolean
fail (GError **error, GIOErrorEnum code, const char *message)
{
    g_set_error_literal (error, G_IO_ERROR, code, message);
    return FALSE;
}

static gboolean
cancelled (GCancellable *cancellable, GError **error)
{
    return g_cancellable_set_error_if_cancelled (cancellable, error);
}

static gboolean
archive_locale_push (locale_t *previous, GError **error)
{
    static gsize initialized;
    static locale_t utf8_locale;

    if (g_once_init_enter (&initialized)) {
        utf8_locale = newlocale (LC_CTYPE_MASK, "C.UTF-8", (locale_t) 0);
        if (!utf8_locale)
            utf8_locale = newlocale (LC_CTYPE_MASK, "C.utf8", (locale_t) 0);
        if (!utf8_locale)
            utf8_locale = newlocale (LC_CTYPE_MASK, "en_US.UTF-8", (locale_t) 0);
        g_once_init_leave (&initialized, 1);
    }
    if (!utf8_locale)
        return fail (error, G_IO_ERROR_NOT_SUPPORTED, "Archive reading requires an installed UTF-8 locale");
    /* libarchive converts entry names to the active locale, even when the
     * archive stores UTF-8. A thread-local scope avoids both lossy names under
     * LANG=C and process-wide locale changes racing unrelated GUI threads. */
    *previous = uselocale (utf8_locale);
    if (!*previous)
        return fail (error, G_IO_ERROR_FAILED, "Cannot select a UTF-8 locale for archive reading");
    return TRUE;
}

/* Never canonicalize a path by silently removing a traversal above root. */
static char *
normalize_path (const char *path, gboolean member)
{
    char **parts;
    GPtrArray *stack;
    GString *result;
    guint i;

    if (path == NULL || strlen (path) > MAX_PATH_BYTES ||
        (member && path[0] == '/'))
        return NULL;
    parts = g_strsplit (path, "/", -1);
    stack = g_ptr_array_new ();
    for (i = 0; parts[i]; i++) {
        if (!*parts[i] || strcmp (parts[i], ".") == 0)
            continue;
        if (strcmp (parts[i], "..") == 0) {
            if (member || stack->len == 0)
                goto invalid;
            g_ptr_array_set_size (stack, stack->len - 1);
        } else {
            if (stack->len >= MAX_DEPTH)
                goto invalid;
            g_ptr_array_add (stack, parts[i]);
        }
    }
    result = g_string_new ("");
    for (i = 0; i < stack->len; i++) {
        if (i)
            g_string_append_c (result, '/');
        g_string_append (result, g_ptr_array_index (stack, i));
    }
    g_ptr_array_unref (stack);
    g_strfreev (parts);
    return g_string_free (result, FALSE);

invalid:
    g_ptr_array_unref (stack);
    g_strfreev (parts);
    return NULL;
}

static char *
info_identity (GFileInfo *info)
{
    const char *etag = g_file_info_get_etag (info);
    const char *id = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILE);
    char *description, *digest;

    /* Without a source change token verified copying cannot be claimed. */
    if (etag == NULL &&
        !(g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE) &&
          g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED)))
        return NULL;
    description = g_strdup_printf ("%s\n%s\n%" G_GOFFSET_FORMAT "\n%" G_GUINT64_FORMAT
                                   "\n%u\n%u", etag ? etag : "", id ? id : "",
                                   g_file_info_get_size (info),
                                   g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED),
                                   g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC),
                                   g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC));
    digest = g_compute_checksum_for_string (G_CHECKSUM_SHA256, description, -1);
    g_free (description);
    return digest;
}

static gboolean
reader_check_identity (Reader *reader, GError **error)
{
    GFileInfo *info;
    char *identity;
    gboolean same;

    info = g_file_query_info (reader->file, ID_ATTRIBUTES, 0, reader->cancellable, error);
    if (info == NULL)
        return FALSE;
    identity = info_identity (info);
    same = g_strcmp0 (identity, reader->identity) == 0;
    g_free (identity);
    g_object_unref (info);
    if (!same)
        return fail (error, G_IO_ERROR_WRONG_ETAG, "Archive changed while it was being read");
    if (reader->stream_identity) {
        info = g_file_input_stream_query_info (reader->input, ID_ATTRIBUTES, reader->cancellable, error);
        if (!info)
            return FALSE;
        identity = info_identity (info);
        same = g_strcmp0 (identity, reader->stream_identity) == 0;
        g_free (identity);
        g_object_unref (info);
        if (!same)
            return fail (error, G_IO_ERROR_WRONG_ETAG, "Open archive source changed while it was being read");
    }
    return TRUE;
}

static void
reader_free (Reader *reader)
{
    if (!reader)
        return;
    if (reader->archive)
        archive_read_free (reader->archive);
    g_clear_object (&reader->input);
    g_clear_object (&reader->file);
    g_clear_object (&reader->cancellable);
    g_clear_error (&reader->error);
    g_free (reader->identity);
    g_free (reader->stream_identity);
    g_free (reader);
}

static void
reader_cancellable (Reader *reader, GCancellable *cancellable)
{
    g_set_object (&reader->cancellable, cancellable);
}

static void
reader_error (Reader *reader, GError **error)
{
    if (reader->error) {
        g_propagate_error (error, g_steal_pointer (&reader->error));
    } else {
        const char *message = archive_error_string (reader->archive);
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Cannot read archive: %s",
                     message ? message : "invalid or unsupported archive data");
    }
}

static la_ssize_t
backing_read (struct archive *archive, void *data, const void **buffer)
{
    Reader *reader = data;
    gssize count;

    g_clear_error (&reader->error);
    count = g_input_stream_read (G_INPUT_STREAM (reader->input), reader->buffer,
                                 sizeof reader->buffer, reader->cancellable, &reader->error);
    if (count < 0) {
        archive_set_error (archive, EIO, "%s", reader->error->message);
        return -1;
    }
    reader->offset += count;
    *buffer = reader->buffer;
    return count;
}

static la_int64_t
backing_seek (struct archive *archive, void *data, la_int64_t offset, int whence)
{
    Reader *reader = data;
    goffset base, target;

    g_clear_error (&reader->error);
    if (cancelled (reader->cancellable, &reader->error))
        goto failed;
    if (whence == SEEK_SET)
        base = 0;
    else if (whence == SEEK_CUR)
        base = reader->offset;
    else if (whence == SEEK_END && reader->size >= 0)
        base = reader->size;
    else {
        fail (&reader->error, G_IO_ERROR_NOT_SUPPORTED,
              "Seeking from the end requires a known archive size");
        goto failed;
    }
    if ((offset > 0 && base > G_MAXINT64 - offset) ||
        (offset < 0 && offset < -base)) {
        fail (&reader->error, G_IO_ERROR_INVALID_ARGUMENT, "Invalid archive seek offset");
        goto failed;
    }
    target = base + offset;
    if (g_seekable_can_seek (G_SEEKABLE (reader->input))) {
        if (!g_seekable_seek (G_SEEKABLE (reader->input), target, G_SEEK_SET,
                             reader->cancellable, &reader->error))
            goto failed;
        reader->offset = target;
        return target;
    }

    /* Remote and nested streams need no disk staging or unbounded byte cache. */
    if (target < reader->offset) {
        GFileInputStream *replacement;
        if (!reader_check_identity (reader, &reader->error))
            goto failed;
        replacement = g_file_read (reader->file, reader->cancellable, &reader->error);
        if (!replacement)
            goto failed;
        g_object_unref (reader->input);
        reader->input = replacement;
        reader->offset = 0;
    }
    while (reader->offset < target) {
        gssize count = g_input_stream_read (G_INPUT_STREAM (reader->input), reader->buffer,
                                          MIN ((goffset) sizeof reader->buffer, target - reader->offset),
                                          reader->cancellable, &reader->error);
        if (count < 0)
            goto failed;
        if (count == 0) {
            fail (&reader->error, G_IO_ERROR_INVALID_DATA, "Archive seek exceeds the available data");
            goto failed;
        }
        reader->offset += count;
    }
    return target;

failed:
    archive_set_error (archive, EIO, "%s", reader->error->message);
    return ARCHIVE_FATAL;
}

static Reader *
reader_new (GFile *file, GCancellable *cancellable, GError **error)
{
    static const int filters[] = {
        ARCHIVE_FILTER_GZIP, ARCHIVE_FILTER_BZIP2, ARCHIVE_FILTER_COMPRESS,
        ARCHIVE_FILTER_LZMA, ARCHIVE_FILTER_XZ, ARCHIVE_FILTER_LZIP,
        ARCHIVE_FILTER_LZ4, ARCHIVE_FILTER_UU, ARCHIVE_FILTER_RPM,
#ifdef ARCHIVE_FILTER_ZSTD
        ARCHIVE_FILTER_ZSTD,
#endif
    };
    Reader *reader = g_new0 (Reader, 1);
    GFileInfo *info, *stream_info;
    guint i;

    reader->file = g_object_ref (file);
    reader_cancellable (reader, cancellable);
    info = g_file_query_info (file, ID_ATTRIBUTES, 0, cancellable, error);
    if (!info)
        goto failed;
    reader->identity = info_identity (info);
    reader->size = g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE) ?
                   g_file_info_get_size (info) : -1;
    if (!reader->identity) {
        g_object_unref (info);
        fail (error, G_IO_ERROR_NOT_SUPPORTED, "Archive source does not provide a stable change token");
        goto failed;
    }
    reader->input = g_file_read (file, cancellable, error);
    if (!reader->input) {
        g_object_unref (info);
        goto failed;
    }
    if (g_file_is_native (file)) {
        static const char *attributes[] = {
            G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_ATTRIBUTE_ETAG_VALUE,
            G_FILE_ATTRIBUTE_TIME_MODIFIED, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
            G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC, G_FILE_ATTRIBUTE_ID_FILE
        };
        /* Bind native metadata to the opened descriptor, not just the pathname,
         * so an atomic replacement cannot mix an old inode with a new index. */
        stream_info = g_file_input_stream_query_info (reader->input, ID_ATTRIBUTES, cancellable, error);
        if (!stream_info) {
            g_object_unref (info);
            goto failed;
        }
        for (i = 0; i < G_N_ELEMENTS (attributes); i++) {
            if (g_file_info_has_attribute (info, attributes[i]) &&
                g_file_info_has_attribute (stream_info, attributes[i])) {
                char *a = g_file_info_get_attribute_as_string (info, attributes[i]);
                char *b = g_file_info_get_attribute_as_string (stream_info, attributes[i]);
                gboolean same = g_strcmp0 (a, b) == 0;
                g_free (a);
                g_free (b);
                if (!same) {
                    g_object_unref (stream_info);
                    g_object_unref (info);
                    fail (error, G_IO_ERROR_WRONG_ETAG, "Archive changed while its source was opened");
                    goto failed;
                }
            }
        }
        reader->stream_identity = info_identity (stream_info);
        g_object_unref (stream_info);
        if (!reader->stream_identity) {
            g_object_unref (info);
            fail (error, G_IO_ERROR_NOT_SUPPORTED, "Open archive source does not provide a stable change token");
            goto failed;
        }
    }
    g_object_unref (info);
    if (!reader_check_identity (reader, error))
        goto failed;
    reader->archive = archive_read_new ();
    archive_read_support_format_all (reader->archive);
    /* Some support_filter calls register executable fallbacks and return WARN.
     * Probe on a disposable reader and register only native implementations. */
    for (i = 0; i < G_N_ELEMENTS (filters); i++) {
        struct archive *probe = archive_read_new ();
        if (archive_read_support_filter_by_code (probe, filters[i]) == ARCHIVE_OK)
            archive_read_support_filter_by_code (reader->archive, filters[i]);
        archive_read_free (probe);
    }
    archive_read_set_callback_data (reader->archive, reader);
    archive_read_set_read_callback (reader->archive, backing_read);
    archive_read_set_seek_callback (reader->archive, backing_seek);
    if (archive_read_open1 (reader->archive) != ARCHIVE_OK) {
        reader_error (reader, error);
        goto failed;
    }
    return reader;

failed:
    reader_free (reader);
    return NULL;
}

static void
member_free (gpointer data)
{
    Member *member = data;
    if (!member)
        return;
    g_free (member->path);
    g_object_unref (member->info);
    g_free (member);
}

static gboolean
iso_read_at (gpointer data, goffset offset, void *buffer, gsize count,
             GCancellable *cancellable, GError **error)
{
    Reader *reader = data;
    gsize done = 0;
    reader_cancellable (reader, cancellable);
    if (reader->offset != offset &&
        backing_seek (reader->archive, reader, offset, SEEK_SET) < 0) {
        reader_error (reader, error);
        return FALSE;
    }
    while (done < count) {
        gssize size = g_input_stream_read (G_INPUT_STREAM (reader->input), (char *) buffer + done,
                                          count - done, cancellable, error);
        if (size < 0)
            return FALSE;
        if (!size)
            return fail (error, G_IO_ERROR_INVALID_DATA, "ISO metadata or member extent is truncated");
        done += size;
        reader->offset += size;
    }
    return TRUE;
}

static Reader *
reader_iso_member (GFile *file, const char *path, const char *identity,
                   NemoArchiveIso **iso, GCancellable *cancellable, GError **error)
{
    Reader *reader = reader_new (file, cancellable, error);
    *iso = NULL;
    if (!reader)
        return NULL;
    if (strcmp (reader->identity, identity) != 0) {
        fail (error, G_IO_ERROR_WRONG_ETAG, "ISO changed while opening its member");
        goto failed;
    }
    *iso = nemo_archive_iso_new (iso_read_at, reader, cancellable, error);
    if (!*iso || !nemo_archive_iso_open (*iso, path, cancellable, error) ||
        !reader_check_identity (reader, error))
        goto failed;
    return reader;
failed:
    g_clear_pointer (iso, nemo_archive_iso_free);
    reader_free (reader);
    return NULL;
}

static void
index_free (Index *index)
{
    if (!index || !g_atomic_int_dec_and_test (&index->references))
        return;
    if (index->database)
        sqlite3_close (index->database);
    g_mutex_clear (&index->mutex);
    g_free (index->identity);
    g_free (index);
}

static Index *
index_ref (Index *index)
{
    g_atomic_int_inc (&index->references);
    return index;
}

static int
sql_cancelled (void *data)
{
    return data && g_cancellable_is_cancelled (data);
}

static gboolean
sql_error (Index *index, int status, GCancellable *cancellable, GError **error)
{
    GIOErrorEnum code;
    if (cancelled (cancellable, error))
        return FALSE;
    code = (status & 0xff) == SQLITE_FULL ? G_IO_ERROR_NO_SPACE :
           (status & 0xff) == SQLITE_NOMEM ? G_IO_ERROR_NO_SPACE : G_IO_ERROR_FAILED;
    if ((status & 0xff) == SQLITE_FULL)
        fail (error, code, "Archive metadata index reached its 4 GiB limit or its filesystem is full");
    else
        g_set_error (error, G_IO_ERROR, code, "Cannot access archive metadata index: %s",
                     sqlite3_errmsg (index->database));
    return FALSE;
}

static gboolean
index_lock (Index *index, GCancellable *cancellable, GError **error)
{
    for (;;) {
        if (cancelled (cancellable, error))
            return FALSE;
        if (g_mutex_trylock (&index->mutex)) {
            sqlite3_progress_handler (index->database, 256, sql_cancelled, cancellable);
            return TRUE;
        }
        g_usleep (1000);
    }
}

static void
index_unlock (Index *index)
{
    sqlite3_progress_handler (index->database, 0, NULL, NULL);
    g_mutex_unlock (&index->mutex);
}

static gboolean
sql_execute (Index *index, const char *sql, GCancellable *cancellable, GError **error)
{
    int status;
    if (cancelled (cancellable, error))
        return FALSE;
    status = sqlite3_exec (index->database, sql, NULL, NULL, NULL);
    return status == SQLITE_OK || sql_error (index, status, cancellable, error);
}

static gboolean
sql_prepare (Index *index, const char *sql, sqlite3_stmt **statement,
             GCancellable *cancellable, GError **error)
{
    int status;
    if (cancelled (cancellable, error))
        return FALSE;
    status = sqlite3_prepare_v2 (index->database, sql, -1, statement, NULL);
    return status == SQLITE_OK || sql_error (index, status, cancellable, error);
}

static Index *
index_database_new (const char *identity, GCancellable *cancellable, GError **error)
{
    Index *index = g_new0 (Index, 1);
    char *directory, *path = NULL;
    int fd = -1, status, attempt;
    struct stat attributes;

    index->references = 1;
    index->identity = g_strdup (identity);
    g_mutex_init (&index->mutex);
    /* Do not use SQLite's anonymous-database/temp-directory selection: it may
     * stage in RAM or a small /tmp tmpfs. The cache filesystem holds metadata
     * only; unlink immediately so even process termination reclaims the file. */
    directory = g_build_filename (g_get_user_cache_dir (), "nemo", "archive-metadata", NULL);
    if (g_mkdir_with_parents (directory, 0700) != 0) {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "Cannot create archive metadata directory: %s", g_strerror (errno));
        g_free (directory);
        goto failed;
    }
    if (g_lstat (directory, &attributes) != 0 || !S_ISDIR (attributes.st_mode) ||
        attributes.st_uid != getuid () || (attributes.st_mode & 0777) != 0700) {
        fail (error, G_IO_ERROR_PERMISSION_DENIED,
              "Archive metadata cache directory must be owned by this user and private (0700)");
        g_free (directory);
        goto failed;
    }
    for (attempt = 0; attempt < 16; attempt++) {
        g_free (path);
        path = g_strdup_printf ("%s/.archive-index-%08x-%08x.db",
                                directory, g_random_int (), g_random_int ());
        fd = g_open (path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST)
            break;
    }
    g_free (directory);
    if (fd < 0) {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "Cannot create archive metadata index: %s", g_strerror (errno));
        g_free (path);
        goto failed;
    }
    status = sqlite3_open_v2 (path, &index->database,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_PRIVATECACHE, NULL);
    if (g_unlink (path) != 0) {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "Cannot unlink private archive metadata index: %s", g_strerror (errno));
        close (fd);
        g_free (path);
        goto failed;
    }
    close (fd);
    g_free (path);
    if (status != SQLITE_OK) {
        sql_error (index, status, cancellable, error);
        goto failed;
    }
    sqlite3_progress_handler (index->database, 256, sql_cancelled, cancellable);
    /* Two MiB of shared page cache; no mmap, journal, or content staging.
     * The 4 GiB metadata-file ceiling is independent of member count.
     * Create the parent index BEFORE insertion so no full-table sorting or
     * temporary table is required, including when enumerating a large folder. */
    if (!sql_execute (index,
            "PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;"
            "PRAGMA locking_mode=EXCLUSIVE; PRAGMA temp_store=MEMORY;"
            "PRAGMA page_size=4096; PRAGMA cache_size=-2048; PRAGMA mmap_size=0;"
            "PRAGMA max_page_count=1048576;"
            "CREATE TABLE members (path TEXT PRIMARY KEY, parent TEXT,"
            "type INTEGER NOT NULL, ordinal INTEGER, size INTEGER, mtime INTEGER,"
            "mtime_nsec INTEGER, hardlink INTEGER NOT NULL, target TEXT) WITHOUT ROWID;"
            "CREATE INDEX members_parent ON members(parent,path);"
            "BEGIN;", cancellable, error))
        goto failed;
    return index;

failed:
    index_free (index);
    return NULL;
}

static GFileInfo *
member_info (const char *path, GFileType type, const char *identity)
{
    GFileInfo *info = g_file_info_new ();
    const char *name = strrchr (path, '/');
    char *display, *content_type, *etag_input, *etag;
    GIcon *icon;

    name = name ? name + 1 : (*path ? path : "/");
    display = g_filename_display_name (name);
    g_file_info_set_name (info, name);
    g_file_info_set_display_name (info, display);
    g_file_info_set_edit_name (info, display);
    g_free (display);
    g_file_info_set_file_type (info, type);
    g_file_info_set_is_hidden (info, name[0] == '.' && name[1] != '\0');
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_RENAME, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
    content_type = type == G_FILE_TYPE_DIRECTORY ? g_strdup ("inode/directory") :
                   g_content_type_guess (name, NULL, 0, NULL);
    g_file_info_set_content_type (info, content_type ? content_type : "application/octet-stream");
    icon = g_content_type_get_icon (content_type ? content_type : "application/octet-stream");
    g_file_info_set_icon (info, icon);
    g_object_unref (icon);
    g_free (content_type);
    etag_input = g_strconcat (identity, "/", path, NULL);
    etag = g_compute_checksum_for_string (G_CHECKSUM_SHA256, etag_input, -1);
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE, etag);
    g_free (etag_input);
    g_free (etag);
    return info;
}

static gboolean
index_store (Index *index, sqlite3_stmt *lookup, sqlite3_stmt *insert,
             const char *path, GFileType type, gint64 ordinal, struct archive_entry *entry,
             GCancellable *cancellable, GError **error)
{
    int status;
    const char *slash = strrchr (path, '/');
    const char *target = entry ? archive_entry_symlink (entry) : NULL;
    gboolean success = FALSE;
    if (cancelled (cancellable, error))
        return FALSE;
    status = sqlite3_bind_text (lookup, 1, path, -1, SQLITE_TRANSIENT);
    if (status != SQLITE_OK) {
        sql_error (index, status, cancellable, error);
        goto done;
    }
    status = sqlite3_step (lookup);
    if (status == SQLITE_ROW) {
        GFileType existing = sqlite3_column_int (lookup, 0);
        if (type != G_FILE_TYPE_DIRECTORY || existing != G_FILE_TYPE_DIRECTORY ||
            (index->is_iso && entry && sqlite3_column_type (lookup, 1) != SQLITE_NULL)) {
            char *display = g_filename_display_name (path);
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "Archive contains duplicate or conflicting member path '%s' "
                         "(existing type %d, incoming type %d%s)",
                         display, existing, type,
                         entry && archive_entry_hardlink (entry) ? ", hard link" : "");
            g_free (display);
            goto done;
        }
        if (!entry) {
            success = TRUE;
            goto done;
        }
    } else if (status != SQLITE_DONE) {
        sql_error (index, status, cancellable, error);
        goto done;
    }
    if (target && strlen (target) > MAX_PATH_BYTES) {
        fail (error, G_IO_ERROR_INVALID_DATA, "Archive link target exceeds the path limit");
        goto done;
    }
    status = sqlite3_bind_text (insert, 1, path, -1, SQLITE_TRANSIENT);
    if (status == SQLITE_OK && *path)
        status = sqlite3_bind_text (insert, 2, path, slash ? slash - path : 0, SQLITE_TRANSIENT);
    if (status == SQLITE_OK && target)
        status = sqlite3_bind_text (insert, 9, target, -1, SQLITE_TRANSIENT);
    if (status != SQLITE_OK) {
        sql_error (index, status, cancellable, error);
        goto done;
    }
    sqlite3_bind_int (insert, 3, type);
    if (entry)
        sqlite3_bind_int64 (insert, 4, ordinal);
    if (entry && archive_entry_size_is_set (entry) && archive_entry_size (entry) >= 0)
        sqlite3_bind_int64 (insert, 5, archive_entry_size (entry));
    if (entry && archive_entry_mtime_is_set (entry) && archive_entry_mtime (entry) >= 0) {
        sqlite3_bind_int64 (insert, 6, archive_entry_mtime (entry));
        sqlite3_bind_int64 (insert, 7, archive_entry_mtime_nsec (entry));
    }
    sqlite3_bind_int (insert, 8, entry && archive_entry_hardlink (entry));
    status = sqlite3_step (insert);
    success = status == SQLITE_DONE || sql_error (index, status, cancellable, error);
done:
    sqlite3_reset (lookup);
    sqlite3_clear_bindings (lookup);
    sqlite3_reset (insert);
    sqlite3_clear_bindings (insert);
    return success;
}

static gboolean
index_entry (Index *index, sqlite3_stmt *lookup, sqlite3_stmt *insert,
             struct archive_entry *entry, gint64 ordinal, GCancellable *cancellable, GError **error)
{
    const char *pathname = archive_entry_pathname_utf8 (entry);
    char *path, *slash;
    GFileType type;
    mode_t mode = archive_entry_filetype (entry);
    gboolean success;
    if (!pathname)
        pathname = archive_entry_pathname (entry);
    path = normalize_path (pathname, TRUE);
    if (!path)
        return fail (error, G_IO_ERROR_INVALID_DATA, "Archive contains an unsafe or excessive member path");
    if (archive_entry_is_encrypted (entry) > 0) {
        g_free (path);
        return fail (error, G_IO_ERROR_NOT_SUPPORTED, "Encrypted archive members are not supported");
    }
    type = mode == AE_IFDIR ? G_FILE_TYPE_DIRECTORY :
           mode == AE_IFREG ? G_FILE_TYPE_REGULAR :
           mode == AE_IFLNK ? G_FILE_TYPE_SYMBOLIC_LINK : G_FILE_TYPE_SPECIAL;
    if (archive_entry_hardlink (entry))
        type = G_FILE_TYPE_REGULAR;
    for (slash = strchr (path, '/'); slash; slash = strchr (slash + 1, '/')) {
        *slash = '\0';
        success = index_store (index, lookup, insert, path, G_FILE_TYPE_DIRECTORY,
                                0, NULL, cancellable, error);
        *slash = '/';
        if (!success) {
            g_free (path);
            return FALSE;
        }
    }
    success = index_store (index, lookup, insert, path, type, ordinal, entry, cancellable, error);
    g_free (path);
    return success;
}

typedef struct {
    Index *index;
    sqlite3_stmt *lookup;
    sqlite3_stmt *insert;
    gint64 ordinal;
} IsoIndexBuild;

static gboolean
index_iso_entry (const NemoArchiveIsoEntry *item, gpointer data,
                 GCancellable *cancellable, GError **error)
{
    IsoIndexBuild *build = data;
    struct archive_entry *entry;
    gboolean success;
    if (build->ordinal == G_MAXINT64)
        return fail (error, G_IO_ERROR_NO_SPACE, "ISO directory ordinal exceeds SQLite's integer range");
    entry = archive_entry_new ();
    archive_entry_set_pathname (entry, item->path);
    archive_entry_set_filetype (entry, item->type == G_FILE_TYPE_DIRECTORY ? AE_IFDIR :
                                item->type == G_FILE_TYPE_REGULAR ? AE_IFREG :
                                item->type == G_FILE_TYPE_SYMBOLIC_LINK ? AE_IFLNK : AE_IFIFO);
    if (item->size >= 0)
        archive_entry_set_size (entry, item->size);
    if (item->mtime >= 0)
        archive_entry_set_mtime (entry, item->mtime, 0);
    if (item->symlink)
        archive_entry_set_symlink (entry, item->symlink);
    success = index_entry (build->index, build->lookup, build->insert, entry,
                            build->ordinal++, cancellable, error);
    archive_entry_free (entry);
    return success;
}

static Index *
index_build (NemoArchiveFile *file, const char *identity, GCancellable *cancellable, GError **error)
{
    Index *index = NULL;
    Reader *reader = NULL;
    struct archive_entry *entry;
    sqlite3_stmt *lookup = NULL, *insert = NULL;
    gint64 ordinal = 0;
    int status;
    locale_t previous;
    gboolean success = FALSE;
    if (!archive_locale_push (&previous, error))
        return NULL;
    reader = reader_new (file->archive, cancellable, error);
    if (!reader)
        goto done;
    if (strcmp (identity, reader->identity) != 0) {
        fail (error, G_IO_ERROR_WRONG_ETAG, "Archive changed before its directory was indexed");
        goto done;
    }
    index = index_database_new (identity, cancellable, error);
    if (!index ||
        !sql_prepare (index, "SELECT type,ordinal FROM members WHERE path=?1", &lookup, cancellable, error) ||
        !sql_prepare (index, "INSERT OR REPLACE INTO members VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)",
                      &insert, cancellable, error) ||
        !index_store (index, lookup, insert, "", G_FILE_TYPE_DIRECTORY, 0, NULL, cancellable, error))
        goto done;
    for (;;) {
        if (cancelled (cancellable, error))
            goto done;
        status = archive_read_next_header (reader->archive, &entry);
        if (status == ARCHIVE_EOF)
            break;
        if (status != ARCHIVE_OK) {
            reader_error (reader, error);
            goto done;
        }
        if (!ordinal &&
            (archive_format (reader->archive) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_ISO9660 &&
            archive_filter_code (reader->archive, 0) == ARCHIVE_FILTER_NONE) {
            NemoArchiveIso *iso = nemo_archive_iso_new (iso_read_at, reader, cancellable, error);
            IsoIndexBuild build = { index, lookup, insert, 0 };
            gboolean indexed;
            if (!iso)
                goto done;
            index->is_iso = TRUE;
            indexed = nemo_archive_iso_walk (iso, index_iso_entry, &build, MAX_PATH_BYTES,
                                              MAX_DEPTH, cancellable, error);
            nemo_archive_iso_free (iso);
            if (!indexed)
                goto done;
            break;
        }
        if (ordinal == G_MAXINT64) {
            fail (error, G_IO_ERROR_NO_SPACE, "Archive header ordinal exceeds SQLite's integer range");
            goto done;
        }
        if (!index_entry (index, lookup, insert, entry, ordinal++, cancellable, error))
            goto done;
        if (archive_read_data_skip (reader->archive) != ARCHIVE_OK) {
            reader_error (reader, error);
            goto done;
        }
    }
    if (!reader_check_identity (reader, error) ||
        !sql_execute (index, "COMMIT; PRAGMA query_only=ON;", cancellable, error))
        goto done;
    {
        sqlite3_stmt *pages = NULL;
        if (!sql_prepare (index, "PRAGMA page_count", &pages, cancellable, error))
            goto done;
        status = sqlite3_step (pages);
        if (status == SQLITE_ROW)
            index->disk_bytes = (guint64) sqlite3_column_int64 (pages, 0) * 4096;
        sqlite3_finalize (pages);
        if (status != SQLITE_ROW) {
            sql_error (index, status, cancellable, error);
            goto done;
        }
    }
    success = TRUE;
done:
    sqlite3_finalize (lookup);
    sqlite3_finalize (insert);
    reader_free (reader);
    if (index)
        sqlite3_progress_handler (index->database, 0, NULL, NULL);
    if (!success)
        g_clear_pointer (&index, index_free);
    uselocale (previous);
    return index;
}

static Index *
index_new (NemoArchiveFile *file, GCancellable *cancellable, GError **error)
{
    ArchiveCache *cache = file->cache;
    if (!file->valid) {
        fail (error, G_IO_ERROR_INVALID_ARGUMENT, "Invalid archive URI or member path");
        return NULL;
    }
    for (;;) {
        GFileInfo *info;
        char *identity;
        Index *index, *old;
        GPtrArray *evicted;
        if (cancelled (cancellable, error))
            return NULL;
        info = g_file_query_info (file->archive, ID_ATTRIBUTES, 0, cancellable, error);
        if (!info)
            return NULL;
        identity = info_identity (info);
        g_object_unref (info);
        if (!identity) {
            fail (error, G_IO_ERROR_NOT_SUPPORTED, "Archive source does not provide a stable change token");
            return NULL;
        }
        g_mutex_lock (&cache->mutex);
        if (cache->building) {
            g_free (identity);
            while (cache->building && !g_cancellable_is_cancelled (cancellable))
                g_cond_wait_until (&cache->changed, &cache->mutex,
                                   g_get_monotonic_time () + 100 * G_TIME_SPAN_MILLISECOND);
            g_mutex_unlock (&cache->mutex);
            continue;
        }
        g_mutex_lock (&cache_registry_mutex);
        if (cache->index && strcmp (cache->index->identity, identity) == 0) {
            index = index_ref (cache->index);
            g_queue_remove (&retained_indexes, cache);
            g_queue_push_tail (&retained_indexes, cache);
            g_mutex_unlock (&cache_registry_mutex);
            g_mutex_unlock (&cache->mutex);
            g_free (identity);
            return index;
        }
        old = cache_detach_index (cache);
        g_mutex_unlock (&cache_registry_mutex);
        cache->building = TRUE;
        g_mutex_unlock (&cache->mutex);
        index_free (old);
        index = index_build (file, identity, cancellable, error);
        g_free (identity);
        evicted = g_ptr_array_new_with_free_func ((GDestroyNotify) index_free);
        g_mutex_lock (&cache->mutex);
        cache->building = FALSE;
        g_mutex_lock (&cache_registry_mutex);
        if (index) {
            cache->index = index_ref (index);
            retained_bytes += index->disk_bytes;
            g_queue_push_tail (&retained_indexes, cache);
        }
        while (retained_indexes.length > MAX_RETAINED_INDEXES ||
               retained_bytes > MAX_RETAINED_BYTES) {
            ArchiveCache *oldest = g_queue_peek_head (&retained_indexes);
            g_ptr_array_add (evicted, cache_detach_index (oldest));
        }
        g_mutex_unlock (&cache_registry_mutex);
        g_cond_broadcast (&cache->changed);
        g_mutex_unlock (&cache->mutex);
        g_ptr_array_unref (evicted);
        return index;
    }
}

#define MEMBER_COLUMNS "path,type,ordinal,size,mtime,mtime_nsec,hardlink,target"

static Member *
member_from_row (Index *index, sqlite3_stmt *statement)
{
    Member *member = g_new0 (Member, 1);
    GFileType type = sqlite3_column_int (statement, 1);
    const char *target = (const char *) sqlite3_column_text (statement, 7);
    member->path = g_strdup ((const char *) sqlite3_column_text (statement, 0));
    member->ordinal = sqlite3_column_int64 (statement, 2);
    member->hardlink = sqlite3_column_int (statement, 6);
    member->info = member_info (member->path, type, index->identity);
    if (sqlite3_column_type (statement, 3) != SQLITE_NULL)
        g_file_info_set_size (member->info, sqlite3_column_int64 (statement, 3));
    if (sqlite3_column_type (statement, 4) != SQLITE_NULL) {
        guint32 nsec = sqlite3_column_int64 (statement, 5);
        g_file_info_set_attribute_uint64 (member->info, G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                          sqlite3_column_int64 (statement, 4));
        g_file_info_set_attribute_uint32 (member->info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, nsec / 1000);
        g_file_info_set_attribute_uint32 (member->info, G_FILE_ATTRIBUTE_TIME_MODIFIED_NSEC, nsec);
    }
    if (type == G_FILE_TYPE_SYMBOLIC_LINK) {
        g_file_info_set_is_symlink (member->info, TRUE);
        if (target)
            g_file_info_set_symlink_target (member->info, target);
    }
    return member;
}

static Member *
index_lookup (Index *index, const char *path, GCancellable *cancellable, GError **error)
{
    sqlite3_stmt *statement = NULL;
    Member *member = NULL;
    int status;
    if (!index_lock (index, cancellable, error))
        return NULL;
    if (sql_prepare (index, "SELECT " MEMBER_COLUMNS " FROM members WHERE path=?1",
                     &statement, cancellable, error)) {
        status = sqlite3_bind_text (statement, 1, path, -1, SQLITE_TRANSIENT);
        if (status == SQLITE_OK)
            status = sqlite3_step (statement);
        if (status == SQLITE_ROW)
            member = member_from_row (index, statement);
        else if (status == SQLITE_DONE)
            fail (error, G_IO_ERROR_NOT_FOUND, "Archive member does not exist");
        else
            sql_error (index, status, cancellable, error);
    }
    sqlite3_finalize (statement);
    index_unlock (index);
    return member;
}

static GFileInfo *
archive_query_info (GFile *file, const char *attributes, GFileQueryInfoFlags flags,
                    GCancellable *cancellable, GError **error)
{
    NemoArchiveFile *self = (NemoArchiveFile *) file;
    Index *index = index_new (self, cancellable, error);
    GFileInfo *info = NULL;
    Member *member;

    if (!index)
        return NULL;
    member = index_lookup (index, self->path, cancellable, error);
    if (member)
        info = g_file_info_dup (member->info);
    member_free (member);
    index_free (index);
    return info;
}

static GFileInfo *
archive_filesystem_info (GFile *file, const char *attributes,
                         GCancellable *cancellable, GError **error)
{
    GFileInfo *info;
    if (cancelled (cancellable, error))
        return NULL;
    if (!((NemoArchiveFile *) file)->valid) {
        fail (error, G_IO_ERROR_INVALID_ARGUMENT, "Invalid archive URI or member path");
        return NULL;
    }
    info = g_file_info_new ();
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY, TRUE);
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, SCHEME);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_REMOTE, TRUE);
    return info;
}

static GFileInfo *
enumerator_next (GFileEnumerator *enumerator, GCancellable *cancellable, GError **error)
{
    NemoArchiveEnumerator *self = (NemoArchiveEnumerator *) enumerator;
    GFileInfo *info = NULL;
    int status;
    if (cancelled (cancellable, error))
        return NULL;
    if (!self->statement || !index_lock (self->index, cancellable, error))
        return NULL;
    status = sqlite3_step (self->statement);
    if (status == SQLITE_ROW) {
        Member *member = member_from_row (self->index, self->statement);
        info = g_object_ref (member->info);
        member_free (member);
    } else {
        if (status != SQLITE_DONE)
            sql_error (self->index, status, cancellable, error);
        sqlite3_finalize (self->statement);
        self->statement = NULL;
    }
    index_unlock (self->index);
    return info;
}

static void
enumerator_clear (NemoArchiveEnumerator *self)
{
    if (self->statement) {
        g_mutex_lock (&self->index->mutex);
        sqlite3_finalize (self->statement);
        self->statement = NULL;
        g_mutex_unlock (&self->index->mutex);
    }
    g_clear_pointer (&self->index, index_free);
}

static gboolean
enumerator_close (GFileEnumerator *enumerator, GCancellable *cancellable, GError **error)
{
    NemoArchiveEnumerator *self = (NemoArchiveEnumerator *) enumerator;
    enumerator_clear (self);
    return !cancelled (cancellable, error);
}

static void
enumerator_finalize (GObject *object)
{
    NemoArchiveEnumerator *self = (NemoArchiveEnumerator *) object;
    enumerator_clear (self);
    G_OBJECT_CLASS (nemo_archive_enumerator_parent_class)->finalize (object);
}

static void
nemo_archive_enumerator_class_init (NemoArchiveEnumeratorClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = enumerator_finalize;
    klass->next_file = enumerator_next;
    klass->close_fn = enumerator_close;
}

static void
nemo_archive_enumerator_init (NemoArchiveEnumerator *self)
{
}

static GFileEnumerator *
archive_enumerate (GFile *file, const char *attributes, GFileQueryInfoFlags flags,
                   GCancellable *cancellable, GError **error)
{
    NemoArchiveFile *self = (NemoArchiveFile *) file;
    Index *index = index_new (self, cancellable, error);
    Member *member;
    NemoArchiveEnumerator *result;

    if (!index)
        return NULL;
    member = index_lookup (index, self->path, cancellable, error);
    if (!member || g_file_info_get_file_type (member->info) != G_FILE_TYPE_DIRECTORY) {
        if (member)
            fail (error, G_IO_ERROR_NOT_DIRECTORY, "Archive member is not a directory");
        member_free (member);
        index_free (index);
        return NULL;
    }
    member_free (member);
    result = g_object_new (nemo_archive_enumerator_get_type (), "container", file, NULL);
    result->index = index;
    if (!index_lock (index, cancellable, error)) {
        g_object_unref (result);
        return NULL;
    }
    if (!sql_prepare (index, "SELECT " MEMBER_COLUMNS
                     " FROM members WHERE parent=?1 ORDER BY path",
                     &result->statement, cancellable, error)) {
        index_unlock (index);
        g_object_unref (result);
        return NULL;
    }
    {
        int status = sqlite3_bind_text (result->statement, 1, self->path, -1, SQLITE_TRANSIENT);
        if (status != SQLITE_OK) {
            sql_error (index, status, cancellable, error);
            index_unlock (index);
            g_object_unref (result);
            return NULL;
        }
    }
    index_unlock (index);
    return G_FILE_ENUMERATOR (result);
}

static gssize
input_read_utf8 (GInputStream *stream, void *buffer, gsize count,
                 GCancellable *cancellable, GError **error)
{
    NemoArchiveInput *self = (NemoArchiveInput *) stream;
    la_ssize_t result;

    if (cancelled (cancellable, error))
        return -1;
    if (self->failed) {
        fail (error, G_IO_ERROR_INVALID_DATA, "Archive member stream has failed");
        return -1;
    }
    reader_cancellable (self->reader, cancellable);
    result = self->iso ?
             nemo_archive_iso_read (self->iso, buffer, count, cancellable, error) :
             archive_read_data (self->reader->archive, buffer, MIN (count, G_MAXSSIZE));
    if (result < 0) {
        if (!self->iso)
            reader_error (self->reader, error);
        self->failed = TRUE;
        return -1;
    }
    self->position += result;
    if (g_file_info_has_attribute (self->info, G_FILE_ATTRIBUTE_STANDARD_SIZE) &&
        (self->position > g_file_info_get_size (self->info) ||
         (result == 0 && self->position != g_file_info_get_size (self->info)))) {
        fail (error, G_IO_ERROR_INVALID_DATA, "Archive member size does not match its header");
        self->failed = TRUE;
        return -1;
    }
    if (result == 0 && !reader_check_identity (self->reader, error)) {
        self->failed = TRUE;
        return -1;
    }
    return result;
}

static gssize
input_read (GInputStream *stream, void *buffer, gsize count,
            GCancellable *cancellable, GError **error)
{
    locale_t previous;
    gssize count_read;
    if (!archive_locale_push (&previous, error))
        return -1;
    count_read = input_read_utf8 (stream, buffer, count, cancellable, error);
    uselocale (previous);
    return count_read;
}

static gssize
input_skip (GInputStream *stream, gsize count, GCancellable *cancellable, GError **error)
{
    guint8 buffer[16 * 1024];
    gsize total = 0;
    count = MIN (count, G_MAXSSIZE);
    while (total < count) {
        gssize read = input_read (stream, buffer, MIN (sizeof buffer, count - total),
                                  cancellable, error);
        if (read < 0)
            return -1;
        if (read == 0)
            break;
        total += read;
    }
    return total;
}

static gboolean
input_close (GInputStream *stream, GCancellable *cancellable, GError **error)
{
    NemoArchiveInput *self = (NemoArchiveInput *) stream;
    gboolean success = !cancelled (cancellable, error);
    if (self->reader) {
        reader_cancellable (self->reader, cancellable);
        if (success)
            success = reader_check_identity (self->reader, error);
        g_clear_pointer (&self->iso, nemo_archive_iso_free);
        g_clear_pointer (&self->reader, reader_free);
    }
    g_clear_pointer (&self->cache, cache_release);
    return success;
}

static GFileInfo *
input_query_info (GFileInputStream *stream, const char *attributes,
                  GCancellable *cancellable, GError **error)
{
    NemoArchiveInput *self = (NemoArchiveInput *) stream;
    if (cancelled (cancellable, error))
        return NULL;
    if (!self->reader) {
        fail (error, G_IO_ERROR_CLOSED, "Archive member stream is closed");
        return NULL;
    }
    reader_cancellable (self->reader, cancellable);
    if (!reader_check_identity (self->reader, error))
        return NULL;
    return g_file_info_dup (self->info);
}

static goffset
input_tell (GFileInputStream *stream)
{
    return ((NemoArchiveInput *) stream)->position;
}

static gboolean
input_can_seek (GFileInputStream *stream)
{
    return TRUE;
}

static Reader *
reader_at_member_utf8 (GFile *file, gint64 member_ordinal, const char *identity,
                       GCancellable *cancellable, GError **error)
{
    Reader *reader = reader_new (file, cancellable, error);
    struct archive_entry *entry;
    gint64 ordinal;

    if (!reader)
        return NULL;
    if (strcmp (reader->identity, identity) != 0) {
        fail (error, G_IO_ERROR_WRONG_ETAG, "Archive changed while opening its member");
        goto failed;
    }
    for (ordinal = 0; ordinal <= member_ordinal; ordinal++) {
        if (cancelled (cancellable, error))
            goto failed;
        if (archive_read_next_header (reader->archive, &entry) != ARCHIVE_OK) {
            reader_error (reader, error);
            goto failed;
        }
        if (ordinal < member_ordinal && archive_read_data_skip (reader->archive) != ARCHIVE_OK) {
            reader_error (reader, error);
            goto failed;
        }
    }
    return reader;

failed:
    reader_free (reader);
    return NULL;
}

static Reader *
reader_at_member (GFile *file, gint64 member_ordinal, const char *identity,
                  GCancellable *cancellable, GError **error)
{
    locale_t previous;
    Reader *reader;
    if (!archive_locale_push (&previous, error))
        return NULL;
    reader = reader_at_member_utf8 (file, member_ordinal, identity, cancellable, error);
    uselocale (previous);
    return reader;
}

static gboolean
input_seek_utf8 (GFileInputStream *stream, goffset offset, GSeekType type,
                 GCancellable *cancellable, GError **error)
{
    NemoArchiveInput *self = (NemoArchiveInput *) stream;
    Reader *replacement;
    NemoArchiveIso *replacement_iso = NULL;
    goffset base, target, position = 0;
    gboolean known_size = g_file_info_has_attribute (self->info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
    goffset size = g_file_info_get_size (self->info);
    guint8 buffer[32 * 1024];

    if (cancelled (cancellable, error))
        return FALSE;
    if (!self->reader)
        return fail (error, G_IO_ERROR_CLOSED, "Archive member stream is closed");
    if (self->failed)
        return fail (error, G_IO_ERROR_INVALID_DATA, "Archive member stream has failed");
    if (type == G_SEEK_SET)
        base = 0;
    else if (type == G_SEEK_CUR)
        base = self->position;
    else if (type == G_SEEK_END && known_size)
        base = size;
    else
        return fail (error, G_IO_ERROR_NOT_SUPPORTED, "Seeking from the end requires a known member size");
    if ((offset > 0 && base > G_MAXINT64 - offset) ||
        (offset < 0 && offset < -base))
        return fail (error, G_IO_ERROR_INVALID_ARGUMENT, "Invalid archive member seek offset");
    target = base + offset;
    if (known_size && target > size)
        return fail (error, G_IO_ERROR_INVALID_ARGUMENT, "Cannot seek past the end of an archive member");
    reader_cancellable (self->reader, cancellable);
    if (!reader_check_identity (self->reader, error))
        return FALSE;
    if (target == self->position && !(known_size && target == size))
        return TRUE;

    /* Replaying into a separate reader keeps failed/cancelled seeks atomic and
     * needs only a fixed-size discard buffer, never a whole-member cache. */
    replacement = self->iso ?
                  reader_iso_member (self->reader->file, self->iso_path, self->reader->identity,
                                      &replacement_iso, cancellable, error) :
                  reader_at_member (self->reader->file, self->ordinal,
                                     self->reader->identity, cancellable, error);
    if (!replacement)
        return FALSE;
    if (replacement_iso) {
        if (!nemo_archive_iso_seek (replacement_iso, target, cancellable, error))
            goto failed;
        goto positioned;
    }
    while (position < target) {
        la_ssize_t count;
        if (cancelled (cancellable, error))
            goto failed;
        count = archive_read_data (replacement->archive, buffer,
                                   MIN ((goffset) sizeof buffer, target - position));
        if (count < 0) {
            reader_error (replacement, error);
            goto failed;
        }
        if (count == 0) {
            fail (error, G_IO_ERROR_INVALID_DATA, "Archive member ended before the requested seek position");
            goto failed;
        }
        position += count;
    }
    if (known_size && target == size) {
        la_ssize_t count;
        if (cancelled (cancellable, error))
            goto failed;
        count = archive_read_data (replacement->archive, buffer, 1);
        if (count < 0) {
            reader_error (replacement, error);
            goto failed;
        }
        if (count != 0) {
            fail (error, G_IO_ERROR_INVALID_DATA, "Archive member size does not match its header");
            goto failed;
        }
    }
positioned:
    if (!reader_check_identity (replacement, error))
        goto failed;
    nemo_archive_iso_free (self->iso);
    reader_free (self->reader);
    self->iso = replacement_iso;
    self->reader = replacement;
    self->position = target;
    return TRUE;

failed:
    nemo_archive_iso_free (replacement_iso);
    reader_free (replacement);
    return FALSE;
}

static gboolean
input_seek (GFileInputStream *stream, goffset offset, GSeekType type,
            GCancellable *cancellable, GError **error)
{
    locale_t previous;
    gboolean success;
    if (!archive_locale_push (&previous, error))
        return FALSE;
    success = input_seek_utf8 (stream, offset, type, cancellable, error);
    uselocale (previous);
    return success;
}

static void
input_finalize (GObject *object)
{
    NemoArchiveInput *self = (NemoArchiveInput *) object;
    nemo_archive_iso_free (self->iso);
    reader_free (self->reader);
    cache_release (self->cache);
    g_clear_object (&self->info);
    g_free (self->iso_path);
    G_OBJECT_CLASS (nemo_archive_input_parent_class)->finalize (object);
}

static void
nemo_archive_input_class_init (NemoArchiveInputClass *klass)
{
    GInputStreamClass *input = G_INPUT_STREAM_CLASS (klass);
    G_OBJECT_CLASS (klass)->finalize = input_finalize;
    input->read_fn = input_read;
    input->skip = input_skip;
    input->close_fn = input_close;
    klass->query_info = input_query_info;
    klass->tell = input_tell;
    klass->can_seek = input_can_seek;
    klass->seek = input_seek;
}

static void
nemo_archive_input_init (NemoArchiveInput *self)
{
}

static GFileInputStream *
archive_read_file (GFile *file, GCancellable *cancellable, GError **error)
{
    NemoArchiveFile *self = (NemoArchiveFile *) file;
    Index *index = index_new (self, cancellable, error);
    Member *member = NULL;
    Reader *reader = NULL;
    NemoArchiveIso *iso = NULL;
    NemoArchiveInput *result;

    if (!index)
        return NULL;
    member = index_lookup (index, self->path, cancellable, error);
    if (!member)
        goto failed;
    if (g_file_info_get_file_type (member->info) != G_FILE_TYPE_REGULAR || member->hardlink) {
        fail (error, g_file_info_get_file_type (member->info) == G_FILE_TYPE_DIRECTORY ?
              G_IO_ERROR_IS_DIRECTORY : G_IO_ERROR_NOT_SUPPORTED,
              "Only regular, non-link archive members can be read");
        goto failed;
    }
    reader = index->is_iso ?
             reader_iso_member (self->archive, self->path, index->identity, &iso, cancellable, error) :
             reader_at_member (self->archive, member->ordinal, index->identity, cancellable, error);
    if (!reader)
        goto failed;
    result = g_object_new (nemo_archive_input_get_type (), NULL);
    result->reader = reader;
    result->iso = iso;
    result->iso_path = iso ? g_strdup (self->path) : NULL;
    result->info = g_file_info_dup (member->info);
    result->ordinal = member->ordinal;
    result->cache = cache_ref (self->cache);
    member_free (member);
    index_free (index);
    return G_FILE_INPUT_STREAM (result);

failed:
    nemo_archive_iso_free (iso);
    reader_free (reader);
    member_free (member);
    index_free (index);
    return NULL;
}

static GFile *
file_new (GFile *archive, const char *path)
{
    NemoArchiveFile *file = g_object_new (nemo_archive_file_get_type (), NULL);
    char *backing_uri, *escaped, *authority, *member;
    GFile *ancestor;
    guint depth = 0;

    file->archive = archive ? g_object_ref (archive) : NULL;
    file->path = path ? g_strdup (path) : NULL;
    file->valid = archive && path;
    for (ancestor = archive;
         ancestor && G_TYPE_CHECK_INSTANCE_TYPE (ancestor, nemo_archive_file_get_type ());
         ancestor = ((NemoArchiveFile *) ancestor)->archive) {
        if (!((NemoArchiveFile *) ancestor)->valid || ++depth >= MAX_NESTING) {
            file->valid = FALSE;
            break;
        }
    }
    if (file->valid) {
        file->cache = cache_acquire (archive);
        backing_uri = g_file_get_uri (archive);
        escaped = g_uri_escape_string (backing_uri, NULL, FALSE);
        authority = g_uri_escape_string (escaped, NULL, FALSE);
        member = g_uri_escape_string (path, "/", FALSE);
        file->uri = g_strconcat (URI_PREFIX, authority, "/", member, NULL);
        g_free (backing_uri);
        g_free (escaped);
        g_free (authority);
        g_free (member);
    } else {
        file->uri = g_strdup (URI_PREFIX "invalid/");
    }
    return G_FILE (file);
}

static GFile *
uri_lookup (GVfs *vfs, const char *uri, gpointer data)
{
    static GPrivate nesting = G_PRIVATE_INIT (NULL);
    char *authority = NULL, *once = NULL, *backing = NULL, *decoded = NULL, *path = NULL;
    char *scheme = NULL;
    GFile *archive = NULL, *result;
    const char *slash;
    guint depth = GPOINTER_TO_UINT (g_private_get (&nesting));

    if (g_ascii_strncasecmp (uri, URI_PREFIX, strlen (URI_PREFIX)) != 0 ||
        strlen (uri) > 1024 * 1024 || depth >= MAX_NESTING)
        goto done;
    slash = strchr (uri + strlen (URI_PREFIX), '/');
    if (!slash || strchr (slash, '?') || strchr (slash, '#'))
        goto done;
    authority = g_strndup (uri + strlen (URI_PREFIX), slash - uri - strlen (URI_PREFIX));
    once = g_uri_unescape_string (authority, NULL);
    backing = once ? g_uri_unescape_string (once, NULL) : NULL;
    scheme = backing ? g_uri_parse_scheme (backing) : NULL;
    if (!scheme)
        goto done;
    decoded = g_uri_unescape_string (slash + 1, NULL);
    path = decoded ? normalize_path (decoded, FALSE) : NULL;
    if (path) {
        g_private_set (&nesting, GUINT_TO_POINTER (depth + 1));
        archive = g_file_new_for_uri (backing);
        g_private_set (&nesting, GUINT_TO_POINTER (depth));
    }
done:
    result = file_new (archive, path);
    if (!((NemoArchiveFile *) result)->valid) {
        g_free (((NemoArchiveFile *) result)->uri);
        ((NemoArchiveFile *) result)->uri = g_strdup (uri);
    }
    g_clear_object (&archive);
    g_free (authority);
    g_free (once);
    g_free (backing);
    g_free (decoded);
    g_free (path);
    g_free (scheme);
    return result;
}

static GFile *
parse_lookup (GVfs *vfs, const char *name, gpointer data)
{
    /* Unlike URI lookup, GVfs asks every registered parser about plain paths. */
    if (g_ascii_strncasecmp (name, SCHEME ":", strlen (SCHEME ":")) != 0)
        return NULL;
    return uri_lookup (vfs, name, data);
}

void
nemo_archive_file_register (void)
{
    static gsize registered;
    if (g_once_init_enter (&registered)) {
        gboolean success = g_vfs_register_uri_scheme (g_vfs_get_default (), SCHEME,
                                                       uri_lookup, NULL, NULL,
                                                       parse_lookup, NULL, NULL);
        if (!success)
            g_warning ("Unable to register the " SCHEME " URI scheme");
        g_once_init_leave (&registered, 1);
    }
}

GFile *
nemo_archive_file_new_for_archive (GFile *archive)
{
    g_return_val_if_fail (G_IS_FILE (archive), NULL);
    nemo_archive_file_register ();
    return file_new (archive, "");
}

GFile *
nemo_archive_file_get_archive (GFile *file)
{
    if (!G_TYPE_CHECK_INSTANCE_TYPE (file, nemo_archive_file_get_type ()))
        return NULL;
    return ((NemoArchiveFile *) file)->valid ?
           g_object_ref (((NemoArchiveFile *) file)->archive) : NULL;
}

static GFile *file_dup (GFile *file) { return g_object_ref (file); }
static guint file_hash (GFile *file) { return g_str_hash (((NemoArchiveFile *) file)->uri); }
static gboolean file_native (GFile *file) { return FALSE; }
static char *file_scheme (GFile *file) { return g_strdup (SCHEME); }
static gboolean file_has_scheme (GFile *file, const char *scheme) { return g_ascii_strcasecmp (scheme, SCHEME) == 0; }
static char *file_path (GFile *file) { return NULL; }
static char *file_uri (GFile *file) { return g_strdup (((NemoArchiveFile *) file)->uri); }

static gboolean
file_equal (GFile *a, GFile *b)
{
    return G_TYPE_CHECK_INSTANCE_TYPE (b, nemo_archive_file_get_type ()) &&
           strcmp (((NemoArchiveFile *) a)->uri, ((NemoArchiveFile *) b)->uri) == 0;
}

static char *
file_basename (GFile *file)
{
    NemoArchiveFile *self = (NemoArchiveFile *) file;
    const char *slash;
    if (!self->valid)
        return g_strdup ("invalid");
    if (!*self->path)
        return g_file_get_basename (self->archive);
    slash = strrchr (self->path, '/');
    return g_strdup (slash ? slash + 1 : self->path);
}

static GFile *
file_parent (GFile *file)
{
    NemoArchiveFile *self = (NemoArchiveFile *) file;
    char *path, *slash;
    GFile *parent;
    if (!self->valid || !*self->path)
        return NULL;
    path = g_strdup (self->path);
    slash = strrchr (path, '/');
    if (slash)
        *slash = '\0';
    else
        *path = '\0';
    parent = file_new (self->archive, path);
    g_free (path);
    return parent;
}

static gboolean
file_prefix (GFile *prefix, GFile *file)
{
    NemoArchiveFile *a = (NemoArchiveFile *) prefix, *b;
    gsize length;
    if (!G_TYPE_CHECK_INSTANCE_TYPE (file, nemo_archive_file_get_type ()))
        return FALSE;
    b = (NemoArchiveFile *) file;
    if (!a->valid || !b->valid || !g_file_equal (a->archive, b->archive))
        return FALSE;
    length = strlen (a->path);
    return !length ? *b->path != '\0' :
           g_str_has_prefix (b->path, a->path) && b->path[length] == '/';
}

static char *
file_relative (GFile *parent, GFile *descendant)
{
    NemoArchiveFile *self = (NemoArchiveFile *) parent;
    gsize length = self->valid ? strlen (self->path) : 0;
    if (file_equal (parent, descendant))
        return g_strdup ("");
    if (!file_prefix (parent, descendant))
        return NULL;
    return g_strdup (((NemoArchiveFile *) descendant)->path + length + (length != 0));
}

static GFile *
file_resolve (GFile *file, const char *relative)
{
    NemoArchiveFile *self = (NemoArchiveFile *) file;
    char *combined, *path;
    GFile *result;
    if (!self->valid)
        return file_dup (file);
    combined = relative[0] == '/' ? g_strdup (relative) :
               g_strconcat (self->path, "/", relative, NULL);
    path = normalize_path (combined, FALSE);
    result = file_new (self->archive, path);
    g_free (combined);
    g_free (path);
    return result;
}

static GFile *
file_display_child (GFile *file, const char *name, GError **error)
{
    if (!*name || strchr (name, '/') || strcmp (name, ".") == 0 ||
        strcmp (name, "..") == 0 || !g_utf8_validate (name, -1, NULL)) {
        fail (error, G_IO_ERROR_INVALID_FILENAME, "Invalid archive child name");
        return NULL;
    }
    return file_resolve (file, name);
}

static gboolean
file_readonly (GFile *file, GCancellable *cancellable, GError **error)
{
    if (cancelled (cancellable, error))
        return FALSE;
    return fail (error, G_IO_ERROR_PERMISSION_DENIED, "Archive contents are read-only");
}

static GFile *
file_rename (GFile *file, const char *name, GCancellable *cancellable, GError **error)
{
    file_readonly (file, cancellable, error);
    return NULL;
}

static gboolean
file_set_attribute (GFile *file, const char *attribute, GFileAttributeType type,
                    gpointer value, GFileQueryInfoFlags flags, GCancellable *cancellable, GError **error)
{
    return file_readonly (file, cancellable, error);
}

static GFileOutputStream *
file_create (GFile *file, GFileCreateFlags flags, GCancellable *cancellable, GError **error)
{
    file_readonly (file, cancellable, error);
    return NULL;
}

static GFileOutputStream *
file_replace (GFile *file, const char *etag, gboolean backup, GFileCreateFlags flags,
              GCancellable *cancellable, GError **error)
{
    file_readonly (file, cancellable, error);
    return NULL;
}

static gboolean
file_move (GFile *source, GFile *destination, GFileCopyFlags flags,
           GCancellable *cancellable, GFileProgressCallback progress, gpointer data, GError **error)
{
    /* Refuse before GIO's copy-and-delete fallback can publish a destination. */
    return file_readonly (source, cancellable, error);
}

static void
file_finalize (GObject *object)
{
    NemoArchiveFile *self = (NemoArchiveFile *) object;
    g_clear_object (&self->archive);
    cache_release (self->cache);
    g_free (self->path);
    g_free (self->uri);
    G_OBJECT_CLASS (nemo_archive_file_parent_class)->finalize (object);
}

static void
nemo_archive_file_class_init (NemoArchiveFileClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = file_finalize;
}

static void
nemo_archive_file_init (NemoArchiveFile *self)
{
}

static void
archive_file_iface_init (GFileIface *iface)
{
    iface->supports_thread_contexts = TRUE;
    iface->dup = file_dup;
    iface->hash = file_hash;
    iface->equal = file_equal;
    iface->is_native = file_native;
    iface->has_uri_scheme = file_has_scheme;
    iface->get_uri_scheme = file_scheme;
    iface->get_basename = file_basename;
    iface->get_path = file_path;
    iface->get_uri = file_uri;
    iface->get_parse_name = file_uri;
    iface->get_parent = file_parent;
    iface->prefix_matches = file_prefix;
    iface->get_relative_path = file_relative;
    iface->resolve_relative_path = file_resolve;
    iface->get_child_for_display_name = file_display_child;
    iface->query_info = archive_query_info;
    iface->query_filesystem_info = archive_filesystem_info;
    iface->enumerate_children = archive_enumerate;
    iface->read_fn = archive_read_file;
    iface->delete_file = file_readonly;
    iface->trash = file_readonly;
    iface->make_directory = file_readonly;
    iface->set_display_name = file_rename;
    iface->set_attribute = file_set_attribute;
    iface->create = file_create;
    iface->append_to = file_create;
    iface->replace = file_replace;
    iface->move = file_move;
}
