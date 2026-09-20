/* Exercise the real asynchronous worker. Wrappers only affect this test's
 * disposable files; in particular, no installed schema or user data is used. */
#define _GNU_SOURCE
#include <config.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixinputstream.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libnemo-private/nemo-file-operations.h"
#include "libnemo-private/nemo-file-conflict-dialog.h"
#include "libnemo-private/nemo-file-undo-manager.h"
#include "libnemo-private/nemo-global-preferences.h"
#include "libnemo-private/nemo-progress-info-manager.h"

typedef enum {
    NONE, CORRUPT, UNKNOWN_METADATA, READBACK, SYNC_EIO, SYNC_ENOSPC,
    SYNC_EINTR, RANGE_EIO, RANGE_EINVAL, RANGE_ENOSYS, RANGE_EOPNOTSUPP,
    DIR_SYNC, DIR_OPEN, CANCEL_CREATE, CANCEL_WRITE, CANCEL_PUBLISH,
    CANCEL_DELETE, COLLISION, CLEANUP, DEFAULT_PERMS, SYNCFS_EIO, SYNCFS_EINTR,
    ATTR_UNSUPPORTED, ATTR_DENIED, ATTR_FAILED, ATTR_NO_SPACE, PULL_FAILED, PULL_CANCEL,
    PUBLISH_RACE, FOLDER_RACE, OPTIONAL_METADATA, MISSING_TYPE,
    SOURCE_EOF, VERIFY_EOF, PULL_EOF, NO_VERSION, SOURCE_CHANGED, CANCEL_VERIFY,
    SCAN_FILE, SCAN_OPEN, SCAN_READ
} Fault;

typedef struct {
    const char *name;
    Fault fault;
    gboolean move, verify, fallback, replace;
} TestCase;

static const TestCase cases[] = {
    { "copy", NONE, FALSE, FALSE, FALSE, FALSE },
    { "crossfs-copy", NONE, FALSE, FALSE, FALSE, FALSE },
    { "zero-byte-copy", NONE, FALSE, FALSE, FALSE, FALSE },
    { "zero-advertised-size-copy", NONE, FALSE, FALSE, FALSE, FALSE },
    { "short-read-copy", SOURCE_EOF, FALSE, FALSE, FALSE, TRUE },
    { "short-read-move", SOURCE_EOF, TRUE, FALSE, TRUE, TRUE },
    { "readback-short-read", VERIFY_EOF, TRUE, FALSE, TRUE, TRUE },
    { "pull-short-read", PULL_EOF, FALSE, FALSE, FALSE, TRUE },
    { "metadata-no-version-move", NO_VERSION, TRUE, FALSE, TRUE, FALSE },
    { "source-changed-move", SOURCE_CHANGED, TRUE, FALSE, TRUE, TRUE },
    { "cancel-verification", CANCEL_VERIFY, TRUE, FALSE, TRUE, TRUE },
    { "verified-copy", NONE, FALSE, TRUE, FALSE, FALSE },
    { "private-source", NONE, FALSE, TRUE, FALSE, FALSE },
    { "metadata-preserved", NONE, FALSE, TRUE, FALSE, FALSE },
    { "default-permissions", DEFAULT_PERMS, FALSE, TRUE, FALSE, FALSE },
    { "default-permissions-move", DEFAULT_PERMS, TRUE, FALSE, TRUE, FALSE },
    { "pull-copy", NONE, FALSE, FALSE, FALSE, TRUE },
    { "pull-chunked-copy", NONE, FALSE, FALSE, FALSE, TRUE },
    { "pull-read-unsupported", NONE, FALSE, FALSE, FALSE, TRUE },
    { "pull-syncfs-eio", SYNCFS_EIO, FALSE, FALSE, FALSE, TRUE },
    { "pull-syncfs-eintr", SYNCFS_EINTR, FALSE, FALSE, FALSE, TRUE },
    { "pull-fsync-eio", SYNC_EIO, FALSE, FALSE, FALSE, TRUE },
    { "pull-failure-cleanup", PULL_FAILED, FALSE, FALSE, FALSE, TRUE },
    { "pull-cancel-cleanup", PULL_CANCEL, FALSE, FALSE, FALSE, TRUE },
    { "pull-verify-unavailable", READBACK, FALSE, TRUE, FALSE, TRUE },
    { "pull-move-unavailable", READBACK, TRUE, FALSE, TRUE, TRUE },
    { "attributes-unsupported", ATTR_UNSUPPORTED, TRUE, FALSE, TRUE, FALSE },
    { "attributes-denied", ATTR_DENIED, TRUE, FALSE, TRUE, FALSE },
    { "attributes-eio", ATTR_FAILED, TRUE, FALSE, TRUE, TRUE },
    { "attributes-enospc", ATTR_NO_SPACE, TRUE, FALSE, TRUE, TRUE },
    { "symlink-attributes-eio", ATTR_FAILED, TRUE, FALSE, TRUE, TRUE },
    { "chunked-copy", NONE, FALSE, TRUE, FALSE, FALSE },
    { "samefs-move", NONE, TRUE, FALSE, FALSE, FALSE },
    { "fallback-move", NONE, TRUE, FALSE, TRUE, FALSE },
    { "real-crossfs-move", NONE, TRUE, FALSE, FALSE, FALSE },
    { "replace-corrupt", CORRUPT, TRUE, FALSE, TRUE, TRUE },
    { "skip-all", CORRUPT, TRUE, FALSE, TRUE, TRUE },
    { "metadata-unknown", UNKNOWN_METADATA, TRUE, FALSE, TRUE, TRUE },
    { "readback-unavailable", READBACK, TRUE, FALSE, TRUE, TRUE },
    { "writer-eio", SYNC_EIO, TRUE, FALSE, TRUE, TRUE },
    { "writer-enospc", SYNC_ENOSPC, TRUE, FALSE, TRUE, TRUE },
    { "writer-eintr", SYNC_EINTR, TRUE, FALSE, TRUE, FALSE },
    { "final-range-eio", RANGE_EIO, TRUE, FALSE, TRUE, TRUE },
    { "range-einval", RANGE_EINVAL, TRUE, FALSE, TRUE, FALSE },
    { "range-enosys", RANGE_ENOSYS, TRUE, FALSE, TRUE, FALSE },
    { "range-eopnotsupp", RANGE_EOPNOTSUPP, TRUE, FALSE, TRUE, FALSE },
    { "directory-fsync", DIR_SYNC, TRUE, FALSE, TRUE, TRUE },
    { "directory-open", DIR_OPEN, TRUE, FALSE, TRUE, TRUE },
    { "cancel-before-write", CANCEL_CREATE, TRUE, FALSE, TRUE, TRUE },
    { "cancel-during-write", CANCEL_WRITE, TRUE, FALSE, TRUE, TRUE },
    { "cancel-after-publish", CANCEL_PUBLISH, TRUE, FALSE, TRUE, TRUE },
    { "cancel-before-delete", CANCEL_DELETE, TRUE, FALSE, TRUE, TRUE },
    { "long-name", NONE, TRUE, FALSE, TRUE, FALSE },
    { "broken-symlink", NONE, TRUE, FALSE, TRUE, FALSE },
    { "fifo-symlink", NONE, TRUE, FALSE, TRUE, FALSE },
    { "folder-copy", NONE, FALSE, TRUE, FALSE, FALSE },
    { "folder-move", NONE, TRUE, FALSE, TRUE, FALSE },
    { "folder-new-copy", NONE, FALSE, TRUE, FALSE, FALSE },
    { "folder-new-move", NONE, TRUE, FALSE, TRUE, FALSE },
    { "folder-parent-fsync", DIR_SYNC, TRUE, FALSE, TRUE, FALSE },
    { "folder-partial-move", CORRUPT, TRUE, FALSE, TRUE, TRUE },
    { "merge-all", NONE, TRUE, FALSE, TRUE, FALSE },
    { "replace-all", NONE, TRUE, FALSE, TRUE, TRUE },
    { "rename-conflict", NONE, TRUE, FALSE, TRUE, TRUE },
    { "stage-collision", COLLISION, TRUE, FALSE, TRUE, FALSE },
    { "cleanup-failure", CLEANUP, TRUE, FALSE, TRUE, TRUE },
    { "file-over-folder", NONE, FALSE, TRUE, FALSE, FALSE },
    { "folder-over-file", NONE, FALSE, TRUE, FALSE, FALSE },
    { "move-file-over-folder", NONE, TRUE, FALSE, FALSE, FALSE },
    { "move-folder-over-file", NONE, TRUE, FALSE, FALSE, FALSE },
    { "fallback-file-over-folder", NONE, TRUE, FALSE, TRUE, FALSE },
    { "fallback-folder-over-file", NONE, TRUE, FALSE, TRUE, FALSE },
    { "publish-directory-race", PUBLISH_RACE, TRUE, FALSE, TRUE, FALSE },
    { "publish-directory-race-copy", PUBLISH_RACE, FALSE, TRUE, FALSE, FALSE },
    { "folder-target-race", FOLDER_RACE, TRUE, FALSE, TRUE, FALSE },
    { "nested-move-file-over-folder", NONE, TRUE, FALSE, FALSE, FALSE },
    { "nested-move-folder-over-file", NONE, TRUE, FALSE, FALSE, FALSE },
    { "metadata-optional-absent", OPTIONAL_METADATA, FALSE, TRUE, FALSE, TRUE },
    { "metadata-optional-absent-move", OPTIONAL_METADATA, TRUE, FALSE, TRUE, TRUE },
    { "metadata-type-missing", MISSING_TYPE, FALSE, TRUE, FALSE, TRUE },
    { "samefs-verified-move", NONE, TRUE, TRUE, FALSE, FALSE },
    { "empty-folder-copy", NONE, FALSE, TRUE, FALSE, FALSE },
    { "empty-folder-move", NONE, TRUE, TRUE, TRUE, FALSE },
    { "conflict-skip", NONE, FALSE, TRUE, FALSE, TRUE },
    { "partial-conflict-copy", NONE, FALSE, TRUE, FALSE, TRUE },
    { "prescan-unreadable-file", SCAN_FILE, FALSE, TRUE, FALSE, FALSE },
    { "prescan-unreadable-folder", SCAN_OPEN, FALSE, TRUE, FALSE, FALSE },
    { "prescan-readdir-error", SCAN_READ, FALSE, TRUE, FALSE, FALSE },
    { "prescan-skip-then-cancel", SCAN_FILE, FALSE, TRUE, FALSE, FALSE },
};

typedef struct {
    char *source, *dest;
    gboolean symlink;
    mode_t source_mode;
    struct timespec source_mtime;
    int writer, writes, attributes, closed, periodic, syncs, range;
    int input_streams, input_closes, output_closes;
    int checksum, stage_checksum, publish, dirsync, deleted;
    int parent_fd, parent_open, backend_copy, syncfs_calls, filesystem_sync;
    int payload_fd, payload_open, adopted;
    gboolean parent_closed;
} Item;

static struct {
    const TestCase *test;
    char *root, *source_dir, *dest_dir, *foreign, *current_source, *contents, *raced_destination;
    GPtrArray *items;
    GHashTable *stages, *backend_payloads;
    GCancellable *cancel;
    GInputStream *pull_input;
    GFileEnumerator *scan_enumerator;
    NemoProgressInfo *progress;
    NemoProgressResult result;
    guint finished;
    gboolean running, done, success, timed_out;
    int sequence, injections, conflicts, warnings, cleanup_warnings, attempts, ancestor_syncs;
    int power_warnings, mismatch_warnings, readback_warnings, cleanup_logs;
    int raced_mkdir_attempts, source_move_attempts;
} fixture;

static const char payload[] = "Original source bytes: SHA-256 must match before deletion.\n";
static const char previous[] = "The previous destination must survive failed replacement.\n";
static const char foreign_data[] = "An unrelated process owns this staging name.\n";

char *__real_g_file_get_path (GFile *);

static gboolean
named (const char *name)
{
    return strcmp (fixture.test->name, name) == 0;
}

static gboolean
pull_case (void)
{
    return g_str_has_prefix (fixture.test->name, "pull-");
}

static void
assert_source_readback (Item *item)
{
    if (fixture.test->fault == NO_VERSION)
        g_assert_cmpint (item->checksum, >, item->syncs);
    else
        g_assert_cmpint (item->checksum, ==, 0);
}

static gboolean
file_over_folder (void)
{
    return !g_str_has_prefix (fixture.test->name, "nested-") &&
           g_str_has_suffix (fixture.test->name, "file-over-folder");
}

static gboolean
folder_over_file (void)
{
    return !g_str_has_prefix (fixture.test->name, "nested-") &&
           g_str_has_suffix (fixture.test->name, "folder-over-file");
}

static gboolean
nested_type_conflict (void)
{
    return g_str_has_prefix (fixture.test->name, "nested-move-");
}

static void
expected_warning (const char *domain, GLogLevelFlags level, const char *message, gpointer data)
{
    if (g_str_has_prefix (message, "Could not inhibit power management:") &&
        strstr (message, "org.freedesktop.DBus.Error.NameHasNoOwner") &&
        strstr (message, "org.gnome.SessionManager")) {
        fixture.power_warnings++;
        g_test_message ("Isolated session has no power manager: %s", message);
    } else if ((fixture.test->fault == CORRUPT || fixture.test->fault == CLEANUP) &&
               g_str_has_prefix (message, "verify-copy: CHECKSUM MISMATCH")) {
        fixture.mismatch_warnings++;
    } else if ((fixture.test->fault == READBACK || fixture.test->fault == VERIFY_EOF) &&
               g_str_has_prefix (message, "verify-copy: could not read back")) {
        fixture.readback_warnings++;
    } else if (fixture.test->fault == CLEANUP &&
               g_str_has_prefix (message, "Could not remove incomplete copy")) {
        fixture.cleanup_logs++;
    } else {
        g_error ("Unexpected production warning: %s", message);
    }
}

static gboolean
under (const char *path, const char *root)
{
    return path && root && g_str_has_prefix (path, root) &&
           (path[strlen (root)] == '/' || path[strlen (root)] == '\0');
}

static Item *
source_item (const char *path)
{
    for (guint i = 0; fixture.items && i < fixture.items->len; i++) {
        Item *item = g_ptr_array_index (fixture.items, i);
        if (g_strcmp0 (path, item->source) == 0)
            return item;
    }
    return NULL;
}

char *
__wrap_g_file_get_path (GFile *file)
{
    char *path = __real_g_file_get_path (file);
    if (fixture.running && (pull_case () || fixture.test->fault == SOURCE_EOF) &&
        source_item (path))
        g_clear_pointer (&path, g_free);
    return path;
}

gboolean __real_g_file_is_native (GFile *);
gboolean
__wrap_g_file_is_native (GFile *file)
{
    char *path = __real_g_file_get_path (file);
    gboolean remote = fixture.running && fixture.test->fault == SOURCE_EOF && source_item (path);
    g_free (path);
    return remote ? FALSE : __real_g_file_is_native (file);
}

static char *
fd_path (int fd)
{
    char proc[64], path[4096];
    g_snprintf (proc, sizeof proc, "/proc/self/fd/%d", fd);
    ssize_t size = readlink (proc, path, sizeof path - 1);
    if (size < 0)
        return NULL;
    path[size] = '\0';
    return g_strdup (path);
}

static gboolean
exists (const char *path)
{
    struct stat st;
    return lstat (path, &st) == 0;
}

static void
inject_io (GError **error, GIOErrorEnum code)
{
    fixture.injections++;
    g_set_error_literal (error, G_IO_ERROR, code, "Injected copy-integrity failure");
}

int __real_open (const char *, int, ...);
int __real_open64 (const char *, int, ...);
static int
open_with_fault (const char *path, int flags, mode_t mode, gboolean large_file)
{
    if (fixture.running) {
        Item *item = source_item (path);
        Item *stage = g_hash_table_lookup (fixture.stages, path);
        if ((fixture.test->fault == DIR_OPEN && (flags & O_DIRECTORY) &&
             under (path, fixture.dest_dir)) ||
            (fixture.test->fault == READBACK && !pull_case () && stage && stage->syncs)) {
            fixture.injections++;
            errno = EACCES;
            return -1;
        }
        if (item && !item->symlink && !(flags & O_DIRECTORY)) {
            g_assert_true ((flags & O_NOFOLLOW) != 0);
            g_assert_true ((flags & O_NONBLOCK) != 0);
        }
    }
    int fd = large_file ? __real_open64 (path, flags, mode) : __real_open (path, flags, mode);
    if (fixture.running && fd >= 0 && pull_case ()) {
        Item *item = source_item (fixture.current_source);
        if (item && g_strcmp0 (path, fixture.dest_dir) == 0 && (flags & O_DIRECTORY)) {
            g_assert_cmpint (item->parent_open, ==, 0);
            item->parent_fd = fd;
            item->parent_open = ++fixture.sequence;
        }
        item = g_hash_table_lookup (fixture.backend_payloads, path);
        if (item) {
            g_assert_true ((flags & O_NOFOLLOW) != 0);
            g_assert_true ((flags & O_NONBLOCK) != 0);
            g_assert_cmpint (flags & O_ACCMODE, ==, O_RDONLY);
            if (item->backend_copy) {
                g_assert_cmpint (item->filesystem_sync, >, item->backend_copy);
                item->payload_fd = fd;
                item->payload_open = ++fixture.sequence;
            } else {
                /* A large opaque pull may open an advisory pacing descriptor
                 * during the backend's progress callback, before syncfs. */
                g_assert_cmpint (item->parent_open, >, 0);
                g_assert_false (item->parent_closed);
            }
        }
    }
    return fd;
}

int
__wrap_open (const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start (args, flags);
        mode = va_arg (args, int);
        va_end (args);
    }
    return open_with_fault (path, flags, mode, FALSE);
}

int
__wrap_open64 (const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start (args, flags);
        mode = va_arg (args, int);
        va_end (args);
    }
    return open_with_fault (path, flags, mode, TRUE);
}

int __real_close (int);
int
__wrap_close (int fd)
{
    Item *payload = NULL;
    if (fixture.running && pull_case ()) {
        char *path = fd_path (fd);
        payload = path ? g_hash_table_lookup (fixture.backend_payloads, path) : NULL;
        g_free (path);
        for (guint i = 0; i < fixture.items->len; i++) {
            Item *item = g_ptr_array_index (fixture.items, i);
            if (fd == item->parent_fd)
                item->parent_closed = TRUE;
        }
    }
    int result = __real_close (fd);
    if (payload && result == 0)
        payload->closed = ++fixture.sequence;
    return result;
}

ssize_t __real_read (int, void *, size_t);
ssize_t
__wrap_read (int fd, void *buffer, size_t count)
{
    if (fixture.running) {
        char *path = fd_path (fd);
        Item *item = source_item (path);
        Item *stage = path ? g_hash_table_lookup (fixture.stages, path) : NULL;
        if (item) {
            g_assert_false (item->symlink);
            g_assert_cmpint (item->syncs, >, item->range);
            g_assert_cmpint (item->closed, >, item->syncs);
            item->checksum = ++fixture.sequence;
        } else if (stage) {
            g_assert_false (stage->symlink);
            g_assert_cmpint (stage->syncs, >, stage->range);
            g_assert_cmpint (stage->closed, >, stage->syncs);
            stage->stage_checksum = ++fixture.sequence;
        }
        g_free (path);
        if (stage && fixture.test->fault == CANCEL_VERIFY) {
            fixture.injections++;
            g_cancellable_cancel (fixture.cancel);
            return 0;
        }
        if ((item || stage) && fixture.test->fault == VERIFY_EOF) {
            fixture.injections++;
            return 0;
        }
    }
    return __real_read (fd, buffer, count);
}

GFileInfo *__real_g_file_query_info (GFile *, const char *, GFileQueryInfoFlags,
                                    GCancellable *, GError **);
GFileInfo *
__wrap_g_file_query_info (GFile *file, const char *attributes, GFileQueryInfoFlags flags,
                         GCancellable *cancel, GError **error)
{
    char *path = __real_g_file_get_path (file);
    if (fixture.running && fixture.test->fault == SCAN_FILE &&
        source_item (path) &&
        strcmp (attributes, G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                            G_FILE_ATTRIBUTE_STANDARD_SIZE) == 0) {
        fixture.injections++;
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                             "Injected pre-scan metadata failure");
        g_free (path);
        return NULL;
    }
    gboolean transaction = fixture.running && under (path, fixture.source_dir) &&
                           strstr (attributes, G_FILE_ATTRIBUTE_ID_FILE) &&
                           strstr (attributes, G_FILE_ATTRIBUTE_ETAG_VALUE);
    if (transaction) {
        g_assert_nonnull (cancel);
        g_assert_true ((flags & G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS) != 0);
        g_free (fixture.current_source);
        fixture.current_source = g_strdup (path);
    }
    GFileInfo *info = __real_g_file_query_info (file, attributes, flags, cancel, error);
    if (fixture.running && fixture.test->fault == FOLDER_RACE &&
        g_strcmp0 (path, fixture.raced_destination) == 0 &&
        strstr (attributes, G_FILE_ATTRIBUTE_ID_FILE) &&
        strstr (attributes, G_FILE_ATTRIBUTE_ETAG_VALUE) &&
        fixture.source_move_attempts >= 2 &&
        !info && error && g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
        fixture.injections == 0) {
        /* Keep the "not found" snapshot, but introduce a foreign file before
         * the directory transaction reports WOULD_RECURSE to its caller. */
        fixture.foreign = g_strdup (path);
        g_assert_true (g_file_set_contents (path, foreign_data, -1, NULL));
        fixture.injections++;
    }
    Item *item = source_item (path);
    if (fixture.running &&
        (pull_case () || fixture.test->fault == SOURCE_EOF || named ("crossfs-copy") ||
         named ("zero-byte-copy") || named ("zero-advertised-size-copy")) && info && item &&
        strstr (attributes, G_FILE_ATTRIBUTE_ID_FILESYSTEM))
        g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILESYSTEM,
                                          "nemo-test-pull-only-filesystem");
    if (transaction && info && item && named ("zero-advertised-size-copy"))
        g_file_info_set_size (info, 0);
    if (transaction && info && item && fixture.test->fault == OPTIONAL_METADATA) {
        g_file_info_remove_attribute (info, G_FILE_ATTRIBUTE_ETAG_VALUE);
        g_file_info_remove_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
        g_file_info_remove_attribute (info, G_FILE_ATTRIBUTE_ID_FILE);
        fixture.injections++;
    }
    if (transaction && info && item &&
        (fixture.test->fault == NO_VERSION || fixture.test->fault == SOURCE_CHANGED)) {
        g_file_info_remove_attribute (info, G_FILE_ATTRIBUTE_ETAG_VALUE);
        if (fixture.test->fault == NO_VERSION) {
            g_file_info_remove_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
            g_file_info_remove_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
        }
        fixture.injections++;
    }
    if (transaction && info && item && fixture.test->fault == MISSING_TYPE) {
        g_file_info_remove_attribute (info, G_FILE_ATTRIBUTE_STANDARD_TYPE);
        fixture.injections++;
    }
    if (transaction && info && item && item->writes &&
        fixture.test->fault == UNKNOWN_METADATA) {
        g_file_info_set_file_type (info, G_FILE_TYPE_UNKNOWN);
        fixture.injections++;
    }
    g_free (path);
    return info;
}

gboolean __real_g_file_make_directory (GFile *, GCancellable *, GError **);
gboolean
__wrap_g_file_make_directory (GFile *file, GCancellable *cancel, GError **error)
{
    char *path = __real_g_file_get_path (file);
    gboolean raced = fixture.running && fixture.test->fault == FOLDER_RACE &&
                     g_strcmp0 (path, fixture.raced_destination) == 0;
    gboolean ok = __real_g_file_make_directory (file, cancel, error);
    if (raced) {
        fixture.raced_mkdir_attempts++;
        g_assert_false (ok);
        g_assert_error (*error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    }
    g_free (path);
    return ok;
}

GFileInputStream *__real_g_file_read (GFile *, GCancellable *, GError **);
GFileInputStream *
__wrap_g_file_read (GFile *file, GCancellable *cancel, GError **error)
{
    char *path = __real_g_file_get_path (file);
    Item *item = source_item (path);
    g_free (path);
    if (fixture.running && fixture.test->fault == SOURCE_EOF && item) {
        GFileInputStream *stream = __real_g_file_read (file, cancel, error);
        if (stream != NULL)
            item->input_streams++;
        return stream;
    }
    if (fixture.running && pull_case () && item) {
        g_assert_nonnull (cancel);
        if (named ("pull-read-unsupported")) {
            GFileInputStream *stream = __real_g_file_read (file, cancel, error);
            g_assert_nonnull (stream);
            item->input_streams++;
            fixture.pull_input = G_INPUT_STREAM (stream);
            g_object_add_weak_pointer (G_OBJECT (stream), (gpointer *) &fixture.pull_input);
            return stream;
        }
        inject_io (error, G_IO_ERROR_NOT_SUPPORTED);
        return NULL;
    }
    return __real_g_file_read (file, cancel, error);
}

gssize __real_g_input_stream_read (GInputStream *, void *, gsize, GCancellable *, GError **);
gssize
__wrap_g_input_stream_read (GInputStream *stream, void *buffer, gsize count,
                            GCancellable *cancel, GError **error)
{
    if (fixture.running && fixture.test->fault == SOURCE_EOF &&
        (G_IS_UNIX_INPUT_STREAM (stream) || G_IS_FILE_DESCRIPTOR_BASED (stream))) {
        int fd = G_IS_UNIX_INPUT_STREAM (stream)
            ? g_unix_input_stream_get_fd (G_UNIX_INPUT_STREAM (stream))
            : g_file_descriptor_based_get_fd (G_FILE_DESCRIPTOR_BASED (stream));
        char *path = fd_path (fd);
        Item *item = source_item (path);
        g_free (path);
        if (item) {
            if (item->closed > 0)
                item->checksum = ++fixture.sequence;
            fixture.injections++;
            return 0;
        }
    }
    if (fixture.running && stream == fixture.pull_input) {
        g_assert_nonnull (cancel);
        inject_io (error, G_IO_ERROR_NOT_SUPPORTED);
        return -1;
    }
    return __real_g_input_stream_read (stream, buffer, count, cancel, error);
}

GFileOutputStream *__real_g_file_create (GFile *, GFileCreateFlags, GCancellable *, GError **);
GFileOutputStream *
__wrap_g_file_create (GFile *file, GFileCreateFlags flags, GCancellable *cancel, GError **error)
{
    char *path = __real_g_file_get_path (file);
    gboolean stage = fixture.running && under (path, fixture.dest_dir) &&
                     strstr (path, "/copy.nemo-partial-");
    if (stage)
        g_assert_true ((flags & G_FILE_CREATE_PRIVATE) != 0);
    if (stage && fixture.test->fault == COLLISION && !fixture.foreign) {
        fixture.foreign = g_strdup (path);
        g_assert_true (g_file_set_contents (path, foreign_data, -1, NULL));
        fixture.injections++;
    }
    GFileOutputStream *stream = __real_g_file_create (file, flags, cancel, error);
    if (stage) {
        fixture.attempts++;
        if (stream) {
            if (named ("folder-new-copy") || named ("folder-new-move"))
                g_assert_cmpint (fixture.ancestor_syncs, >, 0);
            Item *item = source_item (fixture.current_source);
            g_assert_nonnull (item);
            g_assert_true (G_IS_FILE_DESCRIPTOR_BASED (stream));
            item->writer = g_file_descriptor_based_get_fd (G_FILE_DESCRIPTOR_BASED (stream));
            struct stat st;
            g_assert_cmpint (fstat (item->writer, &st), ==, 0);
            g_assert_cmpuint (st.st_mode & 0777, ==, 0600);
            g_hash_table_insert (fixture.stages, g_strdup (path), item);
            g_set_object (&fixture.cancel, cancel);
            if (fixture.test->fault == CANCEL_CREATE) {
                fixture.injections++;
                g_cancellable_cancel (cancel);
            }
        } else if (fixture.test->fault == COLLISION && g_strcmp0 (path, fixture.foreign) == 0) {
            g_assert_error (*error, G_IO_ERROR, G_IO_ERROR_EXISTS);
        }
    }
    g_free (path);
    return stream;
}

gboolean __real_g_file_make_symbolic_link (GFile *, const char *, GCancellable *, GError **);
gboolean
__wrap_g_file_make_symbolic_link (GFile *file, const char *target,
                                 GCancellable *cancel, GError **error)
{
    gboolean ok = __real_g_file_make_symbolic_link (file, target, cancel, error);
    char *path = __real_g_file_get_path (file);
    if (fixture.running && ok && under (path, fixture.dest_dir)) {
        Item *item = source_item (fixture.current_source);
        g_assert_nonnull (item);
        g_assert_true (item->symlink);
        g_hash_table_insert (fixture.stages, g_strdup (path), item);
    }
    g_free (path);
    return ok;
}

gboolean __real_g_output_stream_write_all (GOutputStream *, const void *, gsize,
                                           gsize *, GCancellable *, GError **);
gboolean
__wrap_g_output_stream_write_all (GOutputStream *stream, const void *buffer, gsize count,
                                 gsize *written, GCancellable *cancel, GError **error)
{
    gboolean ok = __real_g_output_stream_write_all (stream, buffer, count, written, cancel, error);
    if (fixture.running && G_IS_FILE_DESCRIPTOR_BASED (stream)) {
        int fd = g_file_descriptor_based_get_fd (G_FILE_DESCRIPTOR_BASED (stream));
        char *path = fd_path (fd);
        Item *item = path ? g_hash_table_lookup (fixture.stages, path) : NULL;
        if (item && ok) {
            struct stat st;
            g_assert_cmpint (fstat (fd, &st), ==, 0);
            g_assert_cmpuint (st.st_mode & 0777, ==, 0600);
            item->writes++;
            if ((fixture.test->fault == CORRUPT || fixture.test->fault == CLEANUP ||
                 fixture.test->fault == VERIFY_EOF) &&
                !(named ("folder-partial-move") && g_str_has_suffix (item->source, "/good"))) {
                g_assert_cmpint (pwrite (fd, "CORRUPTED", 9, 0), ==, 9);
                fixture.injections++;
            } else if (fixture.test->fault == CANCEL_WRITE) {
                fixture.injections++;
                g_cancellable_cancel (cancel);
            }
        }
        g_free (path);
    }
    return ok;
}

GInputStream *__real_g_unix_input_stream_new (int, gboolean);
GInputStream *
__wrap_g_unix_input_stream_new (int fd, gboolean close_fd)
{
    if (fixture.running) {
        char *path = fd_path (fd);
        Item *item = source_item (path);
        if (item)
            item->input_streams++;
        g_free (path);
    }
    return __real_g_unix_input_stream_new (fd, close_fd);
}

gboolean __real_g_input_stream_close (GInputStream *, GCancellable *, GError **);
gboolean
__wrap_g_input_stream_close (GInputStream *stream, GCancellable *cancel, GError **error)
{
    int fd = G_IS_UNIX_INPUT_STREAM (stream) ?
             g_unix_input_stream_get_fd (G_UNIX_INPUT_STREAM (stream)) :
             G_IS_FILE_DESCRIPTOR_BASED (stream) ?
             g_file_descriptor_based_get_fd (G_FILE_DESCRIPTOR_BASED (stream)) : -1;
    if (fixture.running && fd >= 0) {
        char *path = fd_path (fd);
        Item *item = source_item (path);
        if (item) {
            g_assert_nonnull (cancel);
            g_assert_true (cancel == fixture.cancel);
            item->input_closes++;
        }
        g_free (path);
    }
    return __real_g_input_stream_close (stream, cancel, error);
}

gboolean __real_g_file_copy_attributes (GFile *, GFile *, GFileCopyFlags,
                                        GCancellable *, GError **);
gboolean
__wrap_g_file_copy_attributes (GFile *source, GFile *dest, GFileCopyFlags flags,
                               GCancellable *cancel, GError **error)
{
    char *path = __real_g_file_get_path (dest);
    Item *item = fixture.running && path ? g_hash_table_lookup (fixture.stages, path) : NULL;
    if (item) {
        g_assert_cmpint (item->syncs, ==, 0);
        g_assert_true ((flags & G_FILE_COPY_NOFOLLOW_SYMLINKS) != 0);
        item->attributes = ++fixture.sequence;
        if (fixture.test->fault == DEFAULT_PERMS) {
            /* A backend without unix::mode cannot widen the private staging
             * file's permissions. Retaining 0600 is the conservative fallback. */
            flags |= G_FILE_COPY_TARGET_DEFAULT_PERMS;
            fixture.injections++;
        }
        GIOErrorEnum code = fixture.test->fault == ATTR_UNSUPPORTED ? G_IO_ERROR_NOT_SUPPORTED :
                            fixture.test->fault == ATTR_DENIED ? G_IO_ERROR_PERMISSION_DENIED :
                            fixture.test->fault == ATTR_FAILED ? G_IO_ERROR_FAILED :
                            fixture.test->fault == ATTR_NO_SPACE ? G_IO_ERROR_NO_SPACE : 0;
        if (fixture.test->fault == ATTR_UNSUPPORTED || fixture.test->fault == ATTR_DENIED ||
            fixture.test->fault == ATTR_FAILED || fixture.test->fault == ATTR_NO_SPACE) {
            inject_io (error, code);
            g_free (path);
            return FALSE;
        }
    }
    g_free (path);
    return __real_g_file_copy_attributes (source, dest, flags, cancel, error);
}

gboolean __real_g_output_stream_close (GOutputStream *, GCancellable *, GError **);
gboolean
__wrap_g_output_stream_close (GOutputStream *stream, GCancellable *cancel, GError **error)
{
    Item *item = NULL;
    if (fixture.running && G_IS_FILE_DESCRIPTOR_BASED (stream)) {
        char *path = fd_path (g_file_descriptor_based_get_fd (G_FILE_DESCRIPTOR_BASED (stream)));
        item = path ? g_hash_table_lookup (fixture.stages, path) : NULL;
        g_free (path);
        if (item) {
            g_assert_nonnull (cancel);
            g_assert_true (cancel == fixture.cancel);
            item->output_closes++;
        }
    }
    gboolean ok = __real_g_output_stream_close (stream, cancel, error);
    if (item && ok)
        item->closed = ++fixture.sequence;
    if (item && ok && fixture.test->fault == SOURCE_CHANGED) {
        int fd = __real_open (item->source, O_WRONLY);
        g_assert_cmpint (fd, >=, 0);
        g_assert_cmpint (pwrite (fd, "UPDATED!!", 9, 0), ==, 9);
        struct timespec times[] = { item->source_mtime, item->source_mtime };
        times[1].tv_sec += 5;
        g_assert_cmpint (futimens (fd, times), ==, 0);
        g_assert_cmpint (__real_close (fd), ==, 0);
        fixture.injections++;
    }
    return ok;
}

int __real_fsync (int);
int
__wrap_fsync (int fd)
{
    char *path = fixture.running ? fd_path (fd) : NULL;
    Item *item = path ? g_hash_table_lookup (fixture.stages, path) : NULL;
    if (item) {
        struct stat st;
        g_assert_cmpint (fstat (fd, &st), ==, 0);
        g_assert_cmpint (fd, ==, item->writer);
        g_assert_cmpint (fcntl (fd, F_GETFL) & O_ACCMODE, !=, O_RDONLY);
        g_assert_cmpint (item->range, >, 0);
        g_assert_cmpint (item->attributes, >, 0);
        gboolean private_mode = fixture.test->fault == DEFAULT_PERMS ||
                                fixture.test->fault == ATTR_UNSUPPORTED ||
                                fixture.test->fault == ATTR_DENIED;
        mode_t expected_mode = private_mode ? 0600 : item->source_mode;
        g_assert_cmpuint (st.st_mode & 0777, ==, expected_mode);
        if (named ("metadata-preserved") || fixture.test->fault == DEFAULT_PERMS) {
            g_assert_cmpint (st.st_mtim.tv_sec, ==, item->source_mtime.tv_sec);
            g_assert_cmpint (st.st_mtim.tv_nsec, ==, item->source_mtime.tv_nsec);
        }
        if (fixture.test->fault == SYNC_EIO || fixture.test->fault == SYNC_ENOSPC ||
            (fixture.test->fault == SYNC_EINTR && fixture.injections == 0)) {
            fixture.injections++;
            errno = fixture.test->fault == SYNC_EIO ? EIO :
                    fixture.test->fault == SYNC_ENOSPC ? ENOSPC : EINTR;
            g_free (path);
            return -1;
        }
    }
    Item *backend = path ? g_hash_table_lookup (fixture.backend_payloads, path) : NULL;
    if (backend) {
        struct stat st;
        g_assert_cmpint (fstat (fd, &st), ==, 0);
        g_assert_true (S_ISREG (st.st_mode));
        g_assert_cmpint (fd, ==, backend->payload_fd);
        g_assert_cmpint (fcntl (fd, F_GETFL) & O_ACCMODE, ==, O_RDONLY);
        g_assert_cmpint (backend->payload_open, >, backend->filesystem_sync);
        if (fixture.test->fault == SYNC_EIO) {
            fixture.injections++;
            g_free (path);
            errno = EIO;
            return -1;
        }
    }
    gboolean directory = path && under (path, fixture.dest_dir) && !item && !backend;
    if (directory && fixture.test->fault == DIR_SYNC) {
        fixture.injections++;
        g_free (path);
        errno = EIO;
        return -1;
    }
    int result = __real_fsync (fd);
    if (result == 0 && item)
        item->syncs = ++fixture.sequence;
    if (result == 0 && backend)
        backend->syncs = ++fixture.sequence;
    if (result == 0 && directory) {
        fixture.ancestor_syncs++;
        for (guint i = 0; i < fixture.items->len; i++) {
            Item *published = g_ptr_array_index (fixture.items, i);
            if (published->publish && !published->dirsync)
                published->dirsync = ++fixture.sequence;
        }
        if (fixture.test->fault == CANCEL_DELETE) {
            fixture.injections++;
            g_cancellable_cancel (fixture.cancel);
        }
    }
    g_free (path);
    return result;
}

int __real_syncfs (int);
int
__wrap_syncfs (int fd)
{
    if (!fixture.running || !pull_case ())
        return __real_syncfs (fd);
    Item *item = source_item (fixture.current_source);
    g_assert_nonnull (item);
    g_assert_cmpint (fd, ==, item->parent_fd);
    g_assert_false (item->parent_closed);
    g_assert_cmpint (item->backend_copy, >, item->parent_open);
    char *path = fd_path (fd);
    g_assert_cmpstr (path, ==, fixture.dest_dir);
    g_free (path);
    item->syncfs_calls++;
    if (fixture.test->fault == SYNCFS_EIO ||
        (fixture.test->fault == SYNCFS_EINTR && item->syncfs_calls == 1)) {
        fixture.injections++;
        errno = fixture.test->fault == SYNCFS_EIO ? EIO : EINTR;
        return -1;
    }
    int result = __real_syncfs (fd);
    if (result == 0)
        item->filesystem_sync = ++fixture.sequence;
    return result;
}

gboolean __real_g_file_copy (GFile *, GFile *, GFileCopyFlags, GCancellable *,
                             GFileProgressCallback, gpointer, GError **);
gboolean
__wrap_g_file_copy (GFile *source, GFile *dest, GFileCopyFlags flags, GCancellable *cancel,
                    GFileProgressCallback progress, gpointer data, GError **error)
{
    char *src = __real_g_file_get_path (source);
    char *dst = __real_g_file_get_path (dest);
    Item *item = fixture.running && pull_case () ? source_item (src) : NULL;
    if (item) {
        g_assert_nonnull (dst);
        g_assert_true (under (dst, fixture.dest_dir));
        g_assert_true (g_str_has_suffix (dst, "/payload"));
        g_assert_false ((flags & G_FILE_COPY_OVERWRITE) != 0);
        g_assert_false (exists (dst));
        char *parent = g_path_get_dirname (dst);
        char *basename = g_path_get_basename (parent);
        struct stat st;
        g_assert_true (g_str_has_prefix (basename, "copy.nemo-partial-"));
        g_assert_cmpuint (strlen (basename), ==, strlen ("copy.nemo-partial-") + 36);
        g_assert_cmpint (lstat (parent, &st), ==, 0);
        g_assert_true (S_ISDIR (st.st_mode));
        g_assert_cmpuint (st.st_mode & 0777, ==, 0700);
        g_assert_cmpint (item->parent_open, >, 0);
        g_assert_false (item->parent_closed);
        g_assert_cmpint (item->output_closes, ==, 1);
        g_hash_table_insert (fixture.backend_payloads, g_strdup (dst), item);
        g_free (basename);
        g_free (parent);
    }
    gboolean ok = __real_g_file_copy (source, dest, flags, cancel, progress, data, error);
    if (item && ok) {
        item->backend_copy = ++fixture.sequence;
        if (fixture.test->fault == PULL_EOF) {
            g_assert_cmpint (truncate (dst, 8), ==, 0);
            fixture.injections++;
        } else if (fixture.test->fault == PULL_FAILED) {
            g_assert_true (exists (dst));
            inject_io (error, G_IO_ERROR_FAILED);
            ok = FALSE;
        } else if (fixture.test->fault == PULL_CANCEL) {
            g_assert_true (exists (dst));
            fixture.injections++;
            g_cancellable_cancel (cancel);
        }
    }
    g_free (src);
    g_free (dst);
    return ok;
}

int __real_sync_file_range (int, off64_t, off64_t, unsigned int);
int
__wrap_sync_file_range (int fd, off64_t offset, off64_t length, unsigned int flags)
{
    char *path = fixture.running ? fd_path (fd) : NULL;
    Item *item = path ? g_hash_table_lookup (fixture.stages, path) : NULL;
    if (!item && path)
        item = g_hash_table_lookup (fixture.backend_payloads, path);
    g_free (path);
    if (item) {
        g_assert_cmpuint (flags, ==, SYNC_FILE_RANGE_WAIT_BEFORE |
                                    SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
        if (length == 0) {
            g_assert_cmpint (offset, ==, 0);
            item->range = ++fixture.sequence;
            int err = fixture.test->fault == RANGE_EIO ? EIO :
                      fixture.test->fault == RANGE_EINVAL ? EINVAL :
                      fixture.test->fault == RANGE_ENOSYS ? ENOSYS :
                      fixture.test->fault == RANGE_EOPNOTSUPP ? EOPNOTSUPP : 0;
            if (err) {
                fixture.injections++;
                errno = err;
                return -1;
            }
        } else {
            item->periodic++;
            g_assert_cmpint (length, >=, 32 * 1024 * 1024);
        }
    }
    return __real_sync_file_range (fd, offset, length, flags);
}

gboolean __real_g_file_move (GFile *, GFile *, GFileCopyFlags, GCancellable *,
                             GFileProgressCallback, gpointer, GError **);
gboolean
__wrap_g_file_move (GFile *source, GFile *dest, GFileCopyFlags flags, GCancellable *cancel,
                    GFileProgressCallback progress, gpointer data, GError **error)
{
    char *src = __real_g_file_get_path (source);
    char *dst = __real_g_file_get_path (dest);
    gboolean ours = fixture.running && under (src, fixture.source_dir);
    if (ours)
        fixture.source_move_attempts++;
    Item *item = fixture.running && src ? g_hash_table_lookup (fixture.stages, src) : NULL;
    Item *backend = fixture.running && src ? g_hash_table_lookup (fixture.backend_payloads, src) : NULL;
    if (ours || item || backend)
        g_assert_true ((flags & G_FILE_COPY_NO_FALLBACK_FOR_MOVE) != 0);
    if (ours && fixture.test->fallback) {
        /* Match GIO's EXDEV result while preserving genuine conflict handling. */
        if (!(flags & G_FILE_COPY_OVERWRITE) && exists (dst))
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS, "File exists");
        else
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                 "Injected cross-filesystem rename (EXDEV)");
        g_free (src);
        g_free (dst);
        return FALSE;
    }
    if (item) {
        g_assert_true (exists (item->source));
        if (!item->symlink) {
            g_assert_cmpint (item->syncs, >, 0);
            if (fixture.test->move || fixture.test->verify) {
                assert_source_readback (item);
                g_assert_cmpint (item->stage_checksum, >, item->syncs);
            } else if (pull_case ()) {
                g_assert_cmpint (item->adopted, >, item->syncs);
            }
        }
    }
    if (backend) {
        struct stat st;
        g_assert_true (exists (backend->source));
        g_assert_true (g_hash_table_lookup (fixture.stages, dst) == backend);
        g_assert_cmpint (lstat (dst, &st), ==, 0);
        g_assert_true (S_ISREG (st.st_mode));
        g_assert_cmpint (st.st_size, ==, 0);
        g_assert_cmpint (backend->syncs, >, backend->filesystem_sync);
        g_assert_cmpint (backend->closed, >, backend->syncs);
    }
    if (item && fixture.test->fault == PUBLISH_RACE && !fixture.injections) {
        g_assert_false (exists (dst));
        g_assert_cmpint (g_mkdir (dst, 0700), ==, 0);
        fixture.foreign = g_build_filename (dst, "foreign-child", NULL);
        g_assert_true (g_file_set_contents (fixture.foreign, foreign_data, -1, NULL));
        fixture.injections++;
    }
    gboolean ok = __real_g_file_move (source, dest, flags, cancel, progress, data, error);
    if (backend && ok)
        backend->adopted = ++fixture.sequence;
    if (item && ok) {
        item->publish = ++fixture.sequence;
        if (fixture.test->fault == CANCEL_PUBLISH) {
            fixture.injections++;
            g_cancellable_cancel (cancel);
        }
    }
    g_free (src);
    g_free (dst);
    return ok;
}

gboolean __real_g_file_delete (GFile *, GCancellable *, GError **);
static void
record_source_delete (const char *path)
{
    Item *item = source_item (path);
    if (!item)
        return;
    if (!item->symlink) {
        assert_source_readback (item);
        g_assert_cmpint (item->stage_checksum, >, item->syncs);
        g_assert_cmpint (item->publish, >, item->stage_checksum);
    }
    g_assert_cmpint (item->dirsync, >, item->publish);
    g_assert_false (g_cancellable_is_cancelled (fixture.cancel));
    item->deleted = ++fixture.sequence;
}

int __real_remove (const char *);
int
__wrap_remove (const char *path)
{
    if (fixture.running) {
        g_assert_cmpstr (path, !=, fixture.foreign);
        record_source_delete (path);
    }
    return __real_remove (path);
}

gboolean
__wrap_g_file_delete (GFile *file, GCancellable *cancel, GError **error)
{
    char *path = __real_g_file_get_path (file);
    if (fixture.running) {
        g_assert_cmpstr (path, !=, fixture.foreign);
        record_source_delete (path);
        if (fixture.test->fault == CLEANUP && path &&
            g_hash_table_contains (fixture.stages, path)) {
            inject_io (error, G_IO_ERROR_PERMISSION_DENIED);
            g_free (path);
            return FALSE;
        }
    }
    g_free (path);
    return __real_g_file_delete (file, cancel, error);
}

GFileEnumerator *__real_g_file_enumerate_children (GFile *, const char *,
                                                  GFileQueryInfoFlags, GCancellable *, GError **);
GFileEnumerator *
__wrap_g_file_enumerate_children (GFile *file, const char *attributes,
                                  GFileQueryInfoFlags flags, GCancellable *cancel, GError **error)
{
    char *path = __real_g_file_get_path (file);
    gboolean scan = fixture.running && under (path, fixture.source_dir) &&
                    strstr (attributes, G_FILE_ATTRIBUTE_STANDARD_SIZE);
    g_free (path);
    if (scan && fixture.test->fault == SCAN_OPEN) {
        fixture.injections++;
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                             "Injected pre-scan directory failure");
        return NULL;
    }
    GFileEnumerator *enumerator = __real_g_file_enumerate_children (file, attributes, flags, cancel, error);
    if (scan && fixture.test->fault == SCAN_READ)
        fixture.scan_enumerator = enumerator;
    return enumerator;
}

GFileInfo *__real_g_file_enumerator_next_file (GFileEnumerator *, GCancellable *, GError **);
GFileInfo *
__wrap_g_file_enumerator_next_file (GFileEnumerator *enumerator, GCancellable *cancel, GError **error)
{
    if (fixture.running && fixture.test->fault == SCAN_READ &&
        enumerator == fixture.scan_enumerator) {
        fixture.scan_enumerator = NULL;
        fixture.injections++;
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                             "Injected pre-scan directory read failure");
        return NULL;
    }
    return __real_g_file_enumerator_next_file (enumerator, cancel, error);
}

static void
configure_conflict (GtkWidget *widget, gpointer unused)
{
    if (GTK_IS_ENTRY (widget) && named ("rename-conflict"))
        gtk_entry_set_text (GTK_ENTRY (widget), "renamed-payload");
    if (GTK_IS_CHECK_BUTTON (widget) &&
        (named ("replace-all") || named ("merge-all") || named ("skip-all")))
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (widget), TRUE);
    if (GTK_IS_CONTAINER (widget))
        gtk_container_foreach (GTK_CONTAINER (widget), configure_conflict, NULL);
}

static gboolean
respond (gpointer unused)
{
    GList *windows = gtk_window_list_toplevels ();
    for (GList *l = windows; l; l = l->next) {
        if (!GTK_IS_DIALOG (l->data) || !gtk_widget_get_visible (l->data))
            continue;
        if (NEMO_IS_FILE_CONFLICT_DIALOG (l->data)) {
            fixture.conflicts++;
            configure_conflict (l->data, NULL);
            gtk_dialog_response (l->data,
                                 (named ("conflict-skip") || named ("partial-conflict-copy")) ?
                                 CONFLICT_RESPONSE_SKIP : named ("rename-conflict") ?
                                 CONFLICT_RESPONSE_RENAME : CONFLICT_RESPONSE_REPLACE);
        } else {
            char *text = NULL;
            if (GTK_IS_MESSAGE_DIALOG (l->data))
                g_object_get (l->data, "text", &text, NULL);
            fixture.warnings++;
            if (text && strstr (text, "incomplete copy")) {
                fixture.cleanup_warnings++;
                gtk_dialog_response (l->data, 0);
            } else if (fixture.test->fault == FOLDER_RACE) {
                /* Folder creation offers Cancel/Skip/Retry, not Skip All. */
                gtk_dialog_response (l->data, 1);
            } else if (named ("prescan-skip-then-cancel") && fixture.warnings == 2) {
                gtk_dialog_response (l->data, 0);
            } else {
                gtk_dialog_response (l->data, named ("skip-all") ? 1 : 2);
            }
            g_free (text);
        }
    }
    g_list_free (windows);
    return G_SOURCE_CONTINUE;
}

static void
copied (GHashTable *debuting, gboolean success, gpointer unused)
{
    g_assert_false (fixture.done);
    fixture.done = TRUE;
    fixture.success = success;
    g_assert_nonnull (fixture.progress);
    g_assert_true (nemo_progress_info_get_result (fixture.progress, &fixture.result));
}

static void
progress_finished (NemoProgressInfo *info, gpointer unused)
{
    NemoProgressResult result;
    fixture.finished++;
    g_assert_true (fixture.done);
    g_assert_true (nemo_progress_info_get_is_finished (info));
    g_assert_true (nemo_progress_info_get_result (info, &result));
    g_assert_cmpint (result.outcome, ==, fixture.result.outcome);
    g_assert_cmpuint (result.completed_items, ==, fixture.result.completed_items);
    gtk_main_quit ();
}

static void
progress_created (NemoProgressInfoManager *manager, NemoProgressInfo *info, gpointer unused)
{
    g_assert_null (fixture.progress);
    fixture.progress = g_object_ref (info);
    g_assert_false (nemo_progress_info_get_result (info, &fixture.result));
    g_signal_connect (info, "finished", G_CALLBACK (progress_finished), NULL);
}

static gboolean
deadline (gpointer unused)
{
    fixture.timed_out = TRUE;
    gtk_main_quit ();
    return G_SOURCE_REMOVE;
}

static void
remove_fixture (const char *path)
{
    struct stat st;
    g_assert_cmpint (lstat (path, &st), ==, 0);
    if (S_ISDIR (st.st_mode)) {
        GDir *dir = g_dir_open (path, 0, NULL);
        g_assert_nonnull (dir);
        const char *name;
        while ((name = g_dir_read_name (dir))) {
            char *child = g_build_filename (path, name, NULL);
            remove_fixture (child);
            g_free (child);
        }
        g_dir_close (dir);
        g_assert_cmpint (g_rmdir (path), ==, 0);
    } else {
        g_assert_cmpint (g_unlink (path), ==, 0);
    }
}

static void
assert_contents (const char *path, const char *expected)
{
    char *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    g_assert_true (g_file_get_contents (path, &contents, &length, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (length, ==, strlen (expected));
    g_assert_true (memcmp (contents, expected, length) == 0);
    g_free (contents);
}

static Item *
add_item (const char *source, const char *dest, gboolean symlink)
{
    Item *item = g_new0 (Item, 1);
    item->source = g_strdup (source);
    item->dest = g_strdup (dest);
    item->symlink = symlink;
    struct stat st;
    g_assert_cmpint (lstat (source, &st), ==, 0);
    item->source_mode = st.st_mode & 0777;
    item->source_mtime = st.st_mtim;
    item->writer = -1;
    item->parent_fd = -1;
    item->payload_fd = -1;
    g_ptr_array_add (fixture.items, item);
    return item;
}

static void
free_item (gpointer data)
{
    Item *item = data;
    g_free (item->source);
    g_free (item->dest);
    g_free (item);
}

static gboolean
successful_case (void)
{
    Fault fault = fixture.test->fault;
    return (fault == NONE || fault == SYNC_EINTR || fault == RANGE_EINVAL ||
            fault == RANGE_ENOSYS || fault == RANGE_EOPNOTSUPP || fault == COLLISION ||
            fault == DEFAULT_PERMS || fault == SYNCFS_EINTR ||
            fault == ATTR_UNSUPPORTED || fault == ATTR_DENIED || fault == OPTIONAL_METADATA ||
            fault == NO_VERSION) &&
           !file_over_folder () && !folder_over_file () && !nested_type_conflict () &&
           !named ("conflict-skip") && !named ("partial-conflict-copy");
}

static void
assert_staging (const char *root)
{
    GDir *dir = g_dir_open (root, 0, NULL);
    const char *name;
    g_assert_nonnull (dir);
    while ((name = g_dir_read_name (dir))) {
        char *path = g_build_filename (root, name, NULL);
        struct stat st;
        g_assert_cmpint (lstat (path, &st), ==, 0);
        if (strstr (name, ".nemo-partial-"))
            g_assert_true (g_strcmp0 (path, fixture.foreign) == 0 ||
                           fixture.test->fault == CLEANUP);
        if (S_ISDIR (st.st_mode))
            assert_staging (path);
        g_free (path);
    }
    g_dir_close (dir);
}

static void
test_copy_integrity (void)
{
    GList *sources = NULL;
    gboolean empty_folder = named ("empty-folder-copy") || named ("empty-folder-move");
    gboolean new_folder = named ("folder-new-copy") || named ("folder-new-move") ||
                          named ("folder-parent-fsync") || fixture.test->fault == FOLDER_RACE ||
                          empty_folder || fixture.test->fault == SCAN_OPEN ||
                          fixture.test->fault == SCAN_READ;
    gboolean folder = new_folder || named ("folder-copy") || named ("folder-move") ||
                      named ("merge-all") || named ("folder-partial-move") ||
                      nested_type_conflict ();
    gboolean is_link = named ("broken-symlink") || named ("fifo-symlink") ||
                       named ("symlink-attributes-eio");
    gboolean type_conflict = file_over_folder () || folder_over_file () || nested_type_conflict ();
    guint count = named ("skip-all") || named ("replace-all") || named ("merge-all") ? 3 :
                  named ("partial-conflict-copy") || named ("prescan-skip-then-cancel") ? 2 : 1;
    fixture.contents = named ("chunked-copy") || named ("pull-chunked-copy") ?
                       g_strnfill (33 * 1024 * 1024, 'x') :
                       g_strdup (named ("zero-byte-copy") ? "" : payload);
    fixture.source_dir = g_build_filename (fixture.root, "source", NULL);
    fixture.dest_dir = g_build_filename (fixture.root, "destination", NULL);
    g_assert_cmpint (g_mkdir (fixture.source_dir, 0700), ==, 0);
    if (named ("real-crossfs-move")) {
        /* Opt in to a disposable second filesystem without using global /tmp. */
        const char *root = g_getenv ("NEMO_TEST_CROSS_FS_ROOT");
        if (!root) {
            g_test_skip ("Set NEMO_TEST_CROSS_FS_ROOT to a writable second filesystem");
            remove_fixture (fixture.root);
            goto out;
        }
        g_free (fixture.dest_dir);
        fixture.dest_dir = g_build_filename (root, "nemo-copy-integrity-XXXXXX", NULL);
        g_assert_nonnull (g_mkdtemp (fixture.dest_dir));
        struct stat src_stat, dst_stat;
        g_assert_cmpint (stat (fixture.source_dir, &src_stat), ==, 0);
        g_assert_cmpint (stat (fixture.dest_dir, &dst_stat), ==, 0);
        if (src_stat.st_dev == dst_stat.st_dev) {
            g_test_skip ("Source and destination are on the same filesystem");
            remove_fixture (fixture.dest_dir);
            remove_fixture (fixture.root);
            goto out;
        }
    } else {
        g_assert_cmpint (g_mkdir (fixture.dest_dir, 0700), ==, 0);
    }
    fixture.items = g_ptr_array_new_with_free_func (free_item);
    fixture.stages = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    fixture.backend_payloads = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < count; i++) {
        char *name = named ("long-name") ? g_strnfill (240, 'a') : g_strdup_printf ("payload-%u", i);
        char *src = g_build_filename (fixture.source_dir, name, NULL);
        char *dst = g_build_filename (fixture.dest_dir, name, NULL);
        if (fixture.test->fault == FOLDER_RACE)
            fixture.raced_destination = g_strdup (dst);
        if (folder) {
            g_assert_cmpint (g_mkdir (src, 0700), ==, 0);
            if (!new_folder) {
                g_assert_cmpint (g_mkdir (dst, 0700), ==, 0);
                char *existing = g_build_filename (dst, "existing", NULL);
                g_assert_true (g_file_set_contents (existing, previous, -1, NULL));
                g_free (existing);
            }
            const char *child_name = named ("folder-partial-move") ? "bad" : "child";
            char *child_src = g_build_filename (src, child_name, NULL);
            char *child_dst = g_build_filename (dst, child_name, NULL);
            if (named ("nested-move-folder-over-file")) {
                g_assert_cmpint (g_mkdir (child_src, 0700), ==, 0);
                g_assert_true (g_file_set_contents (child_dst, previous, -1, NULL));
                fixture.foreign = g_strdup (child_dst);
                char *grandchild_src = g_build_filename (child_src, "grandchild", NULL);
                char *grandchild_dst = g_build_filename (child_dst, "grandchild", NULL);
                g_assert_true (g_file_set_contents (grandchild_src, fixture.contents, -1, NULL));
                add_item (grandchild_src, grandchild_dst, FALSE);
                g_free (grandchild_src);
                g_free (grandchild_dst);
            } else if (!empty_folder) {
                g_assert_true (g_file_set_contents (child_src, fixture.contents, -1, NULL));
                add_item (child_src, child_dst, FALSE);
                if (named ("nested-move-file-over-folder")) {
                    g_assert_cmpint (g_mkdir (child_dst, 0700), ==, 0);
                    fixture.foreign = g_build_filename (child_dst, "foreign-child", NULL);
                    g_assert_true (g_file_set_contents (fixture.foreign, previous, -1, NULL));
                }
            }
            if (named ("folder-partial-move")) {
                g_assert_true (g_file_set_contents (child_dst, previous, -1, NULL));
                char *good_src = g_build_filename (src, "good", NULL);
                char *good_dst = g_build_filename (dst, "good", NULL);
                g_assert_true (g_file_set_contents (good_src, fixture.contents, -1, NULL));
                add_item (good_src, good_dst, FALSE);
                g_free (good_src);
                g_free (good_dst);
            }
            g_free (child_src);
            g_free (child_dst);
        } else if (folder_over_file ()) {
            g_assert_cmpint (g_mkdir (src, 0700), ==, 0);
            char *child = g_build_filename (src, "child", NULL);
            g_assert_true (g_file_set_contents (child, fixture.contents, -1, NULL));
            g_assert_true (g_file_set_contents (dst, previous, -1, NULL));
            g_free (child);
        } else {
            if (is_link) {
                char *target = g_build_filename (fixture.root, "fifo-target", NULL);
                if (named ("fifo-symlink") || named ("symlink-attributes-eio"))
                    g_assert_cmpint (mkfifo (target, 0600), ==, 0);
                g_assert_cmpint (symlink (target, src), ==, 0);
                g_free (target);
            } else {
                g_assert_true (g_file_set_contents (src, fixture.contents, -1, NULL));
                if (named ("private-source"))
                    g_assert_cmpint (g_chmod (src, 0600), ==, 0);
                if (named ("metadata-preserved") || fixture.test->fault == DEFAULT_PERMS) {
                    struct timespec times[2] = {
                        { .tv_sec = 1234567800, .tv_nsec = 654321000 },
                        { .tv_sec = 1234567890, .tv_nsec = 123456000 },
                    };
                    g_assert_cmpint (g_chmod (src, 0640), ==, 0);
                    g_assert_cmpint (utimensat (AT_FDCWD, src, times, AT_SYMLINK_NOFOLLOW), ==, 0);
                }
            }
            if (file_over_folder ()) {
                g_assert_cmpint (g_mkdir (dst, 0700), ==, 0);
                char *child = g_build_filename (dst, "existing", NULL);
                g_assert_true (g_file_set_contents (child, previous, -1, NULL));
                g_free (child);
            } else if (fixture.test->replace && (!named ("partial-conflict-copy") || i == 0)) {
                g_assert_true (g_file_set_contents (dst, previous, -1, NULL));
            }
            if (named ("rename-conflict")) {
                g_free (dst);
                dst = g_build_filename (fixture.dest_dir, "renamed-payload", NULL);
            }
            add_item (src, dst, is_link);
        }
        sources = g_list_append (sources, g_file_new_for_path (src));
        g_free (src);
        g_free (dst);
        g_free (name);
    }

    nemo_file_operations_set_verify_copies (fixture.test->verify);
    g_settings_set_boolean (nemo_preferences, "safe-cross-fs-copy", !named ("fallback-move"));
    GFile *dest = g_file_new_for_path (fixture.dest_dir);
    NemoProgressInfoManager *manager = nemo_progress_info_manager_new ();
    g_signal_connect (manager, "new-progress-info", G_CALLBACK (progress_created), NULL);
    guint responder = g_timeout_add (10, respond, NULL);
    guint timeout = g_timeout_add_seconds (20, deadline, NULL);
    fixture.running = TRUE;
    if (fixture.test->move)
        nemo_file_operations_move (sources, NULL, dest, NULL, copied, NULL);
    else
        nemo_file_operations_copy (sources, NULL, dest, NULL, copied, NULL);
    gtk_main ();
    fixture.running = FALSE;
    g_source_remove (responder);
    if (!fixture.timed_out)
        g_source_remove (timeout);
    g_assert_false (fixture.timed_out);
    g_assert_true (fixture.done);
    g_assert_cmpuint (fixture.finished, ==, 1);
    g_assert_cmpint (fixture.power_warnings, ==, 1);
    g_assert_cmpint (fixture.success, ==, successful_case ());
    g_assert_cmpint (fixture.result.operation, ==, fixture.test->move ?
                     NEMO_PROGRESS_OPERATION_MOVE : NEMO_PROGRESS_OPERATION_COPY);
    g_assert_cmpint (fixture.result.verification_requested, ==, fixture.test->verify);
    GCancellable *progress_cancel = nemo_progress_info_get_cancellable (fixture.progress);
    gboolean cancelled = g_cancellable_is_cancelled (progress_cancel);
    g_object_unref (progress_cancel);
    if (named ("prescan-skip-then-cancel"))
        g_assert_true (cancelled);
    gboolean partial = named ("folder-partial-move") || named ("partial-conflict-copy") ||
                       named ("conflict-skip") || fixture.test->fault == SCAN_READ;
    NemoProgressOutcome expected_outcome = cancelled ? NEMO_PROGRESS_OUTCOME_CANCELLED :
        successful_case () ? NEMO_PROGRESS_OUTCOME_SUCCESS :
        partial ? NEMO_PROGRESS_OUTCOME_PARTIAL : NEMO_PROGRESS_OUTCOME_FAILED;
    g_assert_cmpint (fixture.result.outcome, ==, expected_outcome);
    if (!successful_case () && !partial) {
        g_assert_cmpuint (fixture.result.completed_items, ==, 0);
        g_assert_cmpuint (fixture.result.completed_regular_files, ==, 0);
        g_assert_cmpuint (fixture.result.completed_symlinks, ==, 0);
        g_assert_cmpuint (fixture.result.checksum_verified_files, ==, 0);
        g_assert_cmpuint (fixture.result.verified_symlinks, ==, 0);
    }
    if (successful_case ()) {
        g_assert_cmpuint (fixture.result.completed_items, >, 0);
        g_assert_cmpuint (fixture.result.completed_items, ==,
                         fixture.result.completed_regular_files + fixture.result.completed_symlinks +
                         fixture.result.completed_directories + fixture.result.atomic_moves);
        g_assert_cmpuint (fixture.result.skipped_items, ==, 0);
        g_assert_cmpuint (fixture.result.failed_items, ==, 0);
    }
    if (named ("copy") || named ("samefs-move") || named ("samefs-verified-move") || empty_folder)
        g_assert_cmpuint (fixture.result.checksum_verified_files, ==, 0);
    if (named ("samefs-move") || named ("samefs-verified-move")) {
        g_assert_cmpuint (fixture.result.atomic_moves, ==, 1);
        g_assert_cmpuint (fixture.result.completed_items, ==, 1);
        g_assert_cmpuint (fixture.result.completed_regular_files, ==, 0);
    }
    if (named ("verified-copy") || named ("fallback-move") || named ("folder-partial-move") ||
        named ("partial-conflict-copy") || fixture.test->fault == SCAN_READ) {
        g_assert_cmpuint (fixture.result.completed_regular_files, ==, 1);
        g_assert_cmpuint (fixture.result.checksum_verified_files, ==, 1);
    }
    if (is_link && successful_case ()) {
        g_assert_cmpuint (fixture.result.checksum_verified_files, ==, 0);
        g_assert_cmpuint (fixture.result.verified_symlinks, ==, 1);
        g_assert_cmpuint (fixture.result.completed_symlinks, ==, 1);
    }
    if (empty_folder) {
        g_assert_cmpuint (fixture.result.completed_items, ==, 1);
        g_assert_cmpuint (fixture.result.completed_directories, ==, 1);
        g_assert_cmpuint (fixture.result.verified_symlinks, ==, 0);
    }
    if (named ("conflict-skip") || named ("partial-conflict-copy")) {
        g_assert_cmpuint (fixture.result.skipped_items, ==, 1);
        g_assert_cmpuint (fixture.result.failed_items, ==, 0);
        g_assert_cmpuint (fixture.result.completed_items, ==, named ("partial-conflict-copy") ? 1 : 0);
    }
    if (fixture.test->fault == SCAN_FILE || fixture.test->fault == SCAN_OPEN ||
        fixture.test->fault == SCAN_READ)
        g_assert_cmpuint (fixture.result.failed_items, ==, 1);
    char *completion = nemo_progress_info_get_completion_text (fixture.progress);
    g_assert_true (g_str_has_prefix (completion, fixture.test->move ? "Move " : "Copy "));
    g_assert_null (strstr (completion, "Waiting"));
    g_assert_null (strstr (completion, "Preparing"));
    char *source_name = g_path_get_basename (fixture.source_dir);
    char *destination_name = g_path_get_basename (fixture.dest_dir);
    g_assert_nonnull (strstr (completion, "From: "));
    g_assert_nonnull (strstr (completion, "To: "));
    g_assert_nonnull (strstr (completion, source_name));
    g_assert_nonnull (strstr (completion, destination_name));
    g_free (source_name);
    g_free (destination_name);
    if (!successful_case ())
        g_assert_null (strstr (completion, " completed."));
    if (!successful_case () || fixture.result.atomic_moves > 0)
        g_assert_null (strstr (completion, "All copied regular files"));
    if (fixture.result.checksum_verified_files == 0) {
        const char *expected = "No completed files were checksum verified.";
        if (successful_case () && !fixture.test->move && !fixture.test->verify)
            expected = "Content verification was not requested.";
        else if (successful_case () && fixture.result.completed_regular_files == 0)
            expected = "No regular files were copied; checksum verification does not apply.";
        g_assert_nonnull (strstr (completion, expected));
    }
    if (named ("verified-copy") || named ("fallback-move"))
        g_assert_nonnull (strstr (completion, "All copied regular files were SHA-256 verified."));
    g_free (completion);
    if (fixture.test->fault != NONE)
        g_assert_cmpint (fixture.injections, >, 0);
    if (fixture.test->fault == CLEANUP) {
        g_assert_cmpint (fixture.cleanup_warnings, ==, 1);
        g_assert_cmpint (fixture.cleanup_logs, ==, 1);
    }
    if (fixture.test->fault == CORRUPT || fixture.test->fault == CLEANUP)
        g_assert_cmpint (fixture.mismatch_warnings, ==, count);
    if (fixture.test->fault == READBACK || fixture.test->fault == VERIFY_EOF)
        g_assert_cmpint (fixture.readback_warnings, ==, 1);
    if (named ("skip-all"))
        g_assert_cmpint (fixture.warnings, ==, 1);
    if (named ("replace-all") || named ("merge-all") || named ("rename-conflict"))
        g_assert_cmpint (fixture.conflicts, ==, 1);
    if (fixture.test->fault == COLLISION) {
        g_assert_cmpint (fixture.attempts, >=, 2);
        assert_contents (fixture.foreign, foreign_data);
    }
    if (fixture.test->fault == FOLDER_RACE) {
        g_assert_cmpint (fixture.raced_mkdir_attempts, ==, 1);
        assert_contents (fixture.foreign, foreign_data);
    }
    if (nested_type_conflict ()) {
        g_assert_false (fixture.test->fallback);
        g_assert_cmpint (fixture.source_move_attempts, >=, 2);
        g_assert_cmpint (fixture.injections, ==, 0);
        assert_contents (fixture.foreign, previous);
    }
    if (fixture.test->fault == MISSING_TYPE)
        g_assert_cmpint (fixture.attempts, ==, 0);
    for (guint i = 0; i < fixture.items->len; i++) {
        Item *item = g_ptr_array_index (fixture.items, i);
        gboolean completed = successful_case () ||
                             (named ("folder-partial-move") && g_str_has_suffix (item->source, "/good")) ||
                             (named ("partial-conflict-copy") && i == 1) ||
                             fixture.test->fault == SCAN_READ;
        g_assert_cmpint (item->input_closes, ==, item->input_streams);
        if (item->writer >= 0)
            g_assert_cmpint (item->output_closes, >, 0);
        g_assert_cmpint (exists (item->source), ==, !completed || !fixture.test->move);
        if (fixture.test->fault == OPTIONAL_METADATA) {
            assert_source_readback (item);
            g_assert_cmpint (item->stage_checksum, >, item->syncs);
            g_assert_cmpint (item->publish, >, item->stage_checksum);
        } else if (fixture.test->fault == MISSING_TYPE) {
            g_assert_cmpint (item->writes, ==, 0);
            g_assert_cmpint (item->publish, ==, 0);
        }
        if (pull_case ()) {
            g_assert_cmpint (fixture.injections, >, 0);
            g_assert_cmpint (item->backend_copy, >, item->parent_open);
            if (fixture.test->fault == PULL_FAILED || fixture.test->fault == PULL_CANCEL)
                g_assert_cmpint (item->syncfs_calls, ==, 0);
            else
                g_assert_cmpint (item->syncfs_calls, >, 0);
            g_assert_cmpint (item->writes, ==, 0);
            g_assert_cmpint (item->checksum, ==, 0);
            if (fixture.test->fault == SYNCFS_EINTR)
                g_assert_cmpint (item->syncfs_calls, ==, 2);
            if (completed) {
                g_assert_cmpint (item->filesystem_sync, >, item->backend_copy);
                g_assert_cmpint (item->syncs, >, item->filesystem_sync);
                g_assert_cmpint (item->adopted, >, item->syncs);
                g_assert_cmpint (item->publish, >, item->adopted);
                g_assert_cmpint (item->stage_checksum, ==, 0);
            } else {
                g_assert_cmpint (item->publish, ==, 0);
                g_assert_cmpint (item->deleted, ==, 0);
            }
        }
        if (item->symlink) {
            char *expected = g_build_filename (fixture.root, "fifo-target", NULL);
            if (exists (item->source)) {
                char *target = g_file_read_link (item->source, NULL);
                g_assert_cmpstr (target, ==, expected);
                g_free (target);
            }
            if (completed || item->publish) {
                char *target = g_file_read_link (item->dest, NULL);
                g_assert_cmpstr (target, ==, expected);
                g_free (target);
            } else if (fixture.test->replace) {
                assert_contents (item->dest, previous);
            } else {
                g_assert_false (exists (item->dest));
            }
            if (named ("fifo-symlink") || named ("symlink-attributes-eio")) {
                struct stat st;
                g_assert_cmpint (lstat (expected, &st), ==, 0);
                g_assert_true (S_ISFIFO (st.st_mode));
            }
            g_assert_cmpint (item->writes, ==, 0);
            g_assert_cmpint (item->syncs, ==, 0);
            g_assert_cmpint (item->checksum, ==, 0);
            g_free (expected);
        } else if (!type_conflict) {
            if (exists (item->source)) {
                if (fixture.test->fault == SOURCE_CHANGED) {
                    char *changed = g_strdup (fixture.contents);
                    memcpy (changed, "UPDATED!!", 9);
                    assert_contents (item->source, changed);
                    g_free (changed);
                } else {
                    assert_contents (item->source, fixture.contents);
                }
            }
            if (completed || item->publish)
                assert_contents (item->dest, fixture.contents);
            else if (fixture.test->fault == PUBLISH_RACE) {
                struct stat st;
                g_assert_cmpint (lstat (item->dest, &st), ==, 0);
                g_assert_true (S_ISDIR (st.st_mode));
                assert_contents (fixture.foreign, foreign_data);
            } else if (fixture.test->replace)
                assert_contents (item->dest, previous);
            else
                g_assert_false (exists (item->dest));
            if (completed && !named ("copy") && !named ("samefs-move") &&
                !named ("samefs-verified-move") && !pull_case () &&
                (fixture.test->move || fixture.test->verify)) {
                g_assert_cmpint (item->writes, >, 0);
                assert_source_readback (item);
                g_assert_cmpint (item->publish, >, item->stage_checksum);
                g_assert_cmpint (item->input_streams, ==, 1);
                if (fixture.test->move)
                    g_assert_cmpint (item->deleted, >, item->dirsync);
            }
            if (named ("copy") || named ("samefs-move") || named ("samefs-verified-move")) {
                g_assert_cmpint (item->writes, ==, 0);
                g_assert_cmpint (item->checksum, ==, 0);
            }
            if (named ("chunked-copy") || named ("pull-chunked-copy"))
                g_assert_cmpint (item->periodic, >, 0);
            if (named ("private-source")) {
                struct stat st;
                g_assert_cmpint (lstat (item->dest, &st), ==, 0);
                g_assert_cmpuint (st.st_mode & 0777, ==, 0600);
            }
            if (named ("metadata-preserved") || fixture.test->fault == DEFAULT_PERMS) {
                struct stat st;
                g_assert_cmpint (lstat (item->dest, &st), ==, 0);
                mode_t expected_mode = fixture.test->fault == DEFAULT_PERMS ? 0600 : 0640;
                g_assert_cmpuint (st.st_mode & 0777, ==, expected_mode);
                /* Access times may change during the mandatory checksum reads. */
                g_assert_cmpint (st.st_mtim.tv_sec, ==, item->source_mtime.tv_sec);
                g_assert_cmpint (st.st_mtim.tv_nsec, ==, item->source_mtime.tv_nsec);
            }
            if (!completed && (fixture.test->fault == CORRUPT || fixture.test->fault == CLEANUP)) {
                g_assert_cmpint (item->writes, >, 0);
                assert_source_readback (item);
                g_assert_cmpint (item->stage_checksum, >, item->syncs);
                g_assert_cmpint (item->publish, ==, 0);
                g_assert_cmpint (item->deleted, ==, 0);
            }
        }
    }
    if (folder || type_conflict || named ("rename-conflict")) {
        for (guint i = 0; i < count; i++) {
            char *name = g_strdup_printf ("payload-%u", i);
            char *src = g_build_filename (fixture.source_dir, name, NULL);
            char *dst = g_build_filename (fixture.dest_dir, name, NULL);
            if ((folder && !new_folder) || file_over_folder ()) {
                char *existing = g_build_filename (dst, "existing", NULL);
                assert_contents (existing, previous);
                g_free (existing);
            } else if (!folder) {
                assert_contents (dst, previous);
            }
            if (folder_over_file ()) {
                char *child = g_build_filename (src, "child", NULL);
                assert_contents (child, fixture.contents);
                g_free (child);
            } else if (file_over_folder ()) {
                assert_contents (src, fixture.contents);
            } else if (folder) {
                g_assert_cmpint (exists (src), ==, !fixture.test->move || !successful_case ());
            }
            if (nested_type_conflict ()) {
                char *child_src = g_build_filename (src, "child", NULL);
                char *child_dst = g_build_filename (dst, "child", NULL);
                struct stat st;
                if (named ("nested-move-file-over-folder")) {
                    assert_contents (child_src, fixture.contents);
                    g_assert_cmpint (lstat (child_dst, &st), ==, 0);
                    g_assert_true (S_ISDIR (st.st_mode));
                } else {
                    g_assert_cmpint (lstat (child_src, &st), ==, 0);
                    g_assert_true (S_ISDIR (st.st_mode));
                    char *grandchild = g_build_filename (child_src, "grandchild", NULL);
                    assert_contents (grandchild, fixture.contents);
                    g_free (grandchild);
                    assert_contents (child_dst, previous);
                }
                g_free (child_src);
                g_free (child_dst);
            }
            g_free (name);
            g_free (src);
            g_free (dst);
        }
    }
    assert_staging (fixture.dest_dir);
    GHashTableIter iter;
    gpointer path;
    g_hash_table_iter_init (&iter, fixture.stages);
    while (g_hash_table_iter_next (&iter, &path, NULL))
        g_assert_cmpint (exists (path), ==, fixture.test->fault == CLEANUP);
    g_hash_table_iter_init (&iter, fixture.backend_payloads);
    while (g_hash_table_iter_next (&iter, &path, NULL)) {
        g_assert_false (exists (path));
        char *container = g_path_get_dirname (path);
        g_assert_false (exists (container));
        g_free (container);
    }
    g_list_free_full (sources, g_object_unref);
    g_object_unref (dest);
    g_clear_object (&fixture.cancel);
    g_clear_object (&fixture.progress);
    g_object_unref (manager);
    g_hash_table_unref (fixture.stages);
    g_hash_table_unref (fixture.backend_payloads);
    g_ptr_array_unref (fixture.items);
    if (named ("real-crossfs-move"))
        remove_fixture (fixture.dest_dir);
    remove_fixture (fixture.root);
out:
    g_free (fixture.root);
    g_free (fixture.source_dir);
    g_free (fixture.dest_dir);
    g_free (fixture.foreign);
    g_free (fixture.current_source);
    g_free (fixture.contents);
    g_free (fixture.raced_destination);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
        g_printerr ("Run copy-integrity tests through the isolated regression runner "
                    "or Meson copy-integrity suite; direct desktop execution is disabled "
                    "(NEMO_TEST_ISOLATED=1 is required).\n");
        return 77;
    }
    g_assert_cmpint (argc, >=, 2);
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
        if (strcmp (argv[1], cases[i].name) == 0) {
            fixture.test = &cases[i];
            break;
        }
    }
    g_assert_nonnull (fixture.test);
    umask (0022);
    char *cwd = g_get_current_dir ();
    const char *profile = g_getenv ("NEMO_TEST_PROFILE");
    struct stat profile_stat;
    g_assert_nonnull (profile);
    g_assert_cmpint (lstat (profile, &profile_stat), ==, 0);
    g_assert_true (S_ISDIR (profile_stat.st_mode));
    g_assert_cmpuint (profile_stat.st_mode & 0777, ==, 0700);
    g_assert_cmpuint (profile_stat.st_uid, ==, getuid ());
    const char *xdg[] = {
        "HOME", "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
        "XDG_STATE_HOME", "XDG_RUNTIME_DIR"
    };
    const char *directories[] = { "home", "cache", "config", "data", "state", "runtime" };
    for (guint i = 0; i < G_N_ELEMENTS (xdg); i++) {
        char *path = g_build_filename (profile, directories[i], NULL);
        struct stat st;
        g_assert_cmpstr (g_getenv (xdg[i]), ==, path);
        g_assert_cmpint (lstat (path, &st), ==, 0);
        g_assert_true (S_ISDIR (st.st_mode));
        g_assert_cmpuint (st.st_mode & 0777, ==, 0700);
        g_free (path);
    }
    g_assert_cmpstr (g_getenv ("GDK_BACKEND"), ==, "x11");
    g_assert_null (g_getenv ("WAYLAND_DISPLAY"));
    g_assert_null (g_getenv ("AT_SPI_BUS_ADDRESS"));
    g_assert_null (g_getenv ("DBUS_STARTER_ADDRESS"));
    fixture.root = g_build_filename (cwd, "copy-integrity-XXXXXX", NULL);
    g_free (cwd);
    g_assert_nonnull (g_mkdtemp (fixture.root));
    argv[1] = argv[0];
    argv++;
    argc--;
    g_test_init (&argc, &argv, NULL);
    /* Match only deliberately injected warnings, without putting unrelated
     * asynchronous GTK messages in g_test_expect_message's ordered queue.
     * Criticals, assertions, and every other Nemo warning remain fatal. */
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    g_log_set_handler ("Nemo", G_LOG_LEVEL_WARNING, expected_warning, NULL);
    gtk_init (&argc, &argv);
    GdkDisplay *display = gdk_display_get_default ();
    g_assert_true (GDK_IS_X11_DISPLAY (display));
    g_assert_nonnull (g_getenv ("DISPLAY"));
    g_assert_cmpstr (gdk_display_get_name (display), ==, g_getenv ("DISPLAY"));
    nemo_global_preferences_init ();
    nemo_file_undo_manager_get ();
    char *path = g_strconcat ("/copy-integrity/", fixture.test->name, NULL);
    g_test_add_func (path, test_copy_integrity);
    g_free (path);
    return g_test_run ();
}
