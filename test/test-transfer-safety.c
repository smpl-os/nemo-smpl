/* End-to-end transfer/undo regressions. Faults are injected at the actual
 * descriptor-relative kernel boundary, never instead of the public job API. */
#define _GNU_SOURCE
#include <config.h>
#include <gtk/gtk.h>
#include <gio/gfiledescriptorbased.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <stdarg.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "libnemo-private/nemo-file-operations.h"
#include "libnemo-private/nemo-file-conflict-dialog.h"
#include "libnemo-private/nemo-file-undo-manager.h"
#include "libnemo-private/nemo-global-preferences.h"
#include "libnemo-private/nemo-progress-info-manager.h"
#include "libnemo-private/nemo-transfer-safety.h"

typedef enum {
    FAULT_NONE, BACKEND_NFS, BACKEND_FUSE, BACKEND_TMPFS, BACKEND_UNKNOWN,
    REMOTE_DEST, REMOTE_SOURCE, REMOVAL_INFLIGHT, MATCH_FSYNC, SOURCE_FSYNC, TREE_FSYNC,
    PUBLISH_FSYNC, RECOVERY_FSYNC, WRITER_CLOSE, DIRECTORY_CLOSE,
    NEW_FILE_RACE, NEW_DIRECTORY_RACE, NEW_SYMLINK_RACE, EXCHANGE_RACE,
    EXCHANGE_UNSUPPORTED, RENAME_EINTR, RENAME_EIO, CAPTURE_SWAP,
    CAPTURE_CHANGED_OCCUPIED, CAPTURE_FAILURE, CAPTURE_CANCEL, DELETE_FAILURE, DELETE_DIR_FSYNC,
    PROMPT_TARGET_MOVE, COPYING_TARGET_MOVE,
    ROOT_SWAP, ANCESTOR_SWAP, SOURCE_PARENT_SWAP, MOUNT_SWAP,
    UNDO_TARGET_RACE, UNDO_RESTORE_RACE, UNDO_FSYNC, UNDO_POSTCAPTURE_FSYNC,
    CAPTURE_RESULT_EIO, PUBLISH_RESULT_EIO, PUBLISH_RESULT_EINTR, FINAL_ANCHOR_CLOSE,
    UNDO_CANCEL_PREFLIGHT, UNDO_CANCEL_CAPTURE, RECOVERY_READABLE, RECOVERY_CLOSE, LINK_SYNCFS,
    LARGE_WRITE_SYNC, RECOVERY_UNKNOWN, EXCHANGE_EINVAL, EXCHANGE_EOPNOTSUPP,
    BACKEND_EXCHANGE_EINVAL, BACKEND_EXCHANGE_EOPNOTSUPP, BACKEND_EXCHANGE_ENOSYS,
    RECOVERY_RESERVED, RECOVERY_GROUP_WRITABLE, RECOVERY_OTHER_WRITABLE,
    MARKER_INODE, MARKER_METADATA, MARKER_CONTENT,
    RECORD_INODE, RECORD_METADATA, RECORD_CONTENT, RECORD_NLINK, RECORD_UID,
    RECOVERY_FOREIGN_PAYLOAD, RECOVERY_FOREIGN_CHILD
} Fault;

typedef enum { COPY, COPY_FILE, DUPLICATE, MOVE } Operation;

typedef struct {
    const char *name;
    Fault fault;
    Operation operation;
    gboolean crossfs, replace, directory, symlink;
} Scenario;

static const Scenario scenarios[] = {
    { "mandatory-stage", FAULT_NONE, COPY },
    { "default-copy", FAULT_NONE, COPY },
    { "default-copy-file", FAULT_NONE, COPY_FILE },
    { "default-duplicate", FAULT_NONE, DUPLICATE },
    { "default-move-consumption", FAULT_NONE, MOVE },
    { "explicit-true-consumption", FAULT_NONE, COPY },
    { "explicit-true-copy-file", FAULT_NONE, COPY_FILE },
    { "explicit-true-duplicate", FAULT_NONE, DUPLICATE },
    { "explicit-true-move", FAULT_NONE, MOVE },
    { "backend-nfs", BACKEND_NFS, COPY },
    { "backend-fuse", BACKEND_FUSE, COPY },
    { "backend-tmpfs", BACKEND_TMPFS, COPY },
    { "real-tmpfs-destination", FAULT_NONE, COPY },
    { "backend-unknown", BACKEND_UNKNOWN, COPY },
    { "remote-destination", REMOTE_DEST, COPY },
    { "remote-source-move", REMOTE_SOURCE, MOVE },
    { "remote-source-copy", REMOTE_SOURCE, COPY },
    { "trash-source-copy", REMOTE_SOURCE, COPY },
    { "removal-inflight-copy", REMOVAL_INFLIGHT, COPY },
    { "removal-inflight-move", REMOVAL_INFLIGHT, MOVE },
    { "existing-match-fsync", MATCH_FSYNC, COPY, FALSE, TRUE },
    { "publish-new-file-race", NEW_FILE_RACE, COPY },
    { "publish-new-directory-race", NEW_DIRECTORY_RACE, COPY },
    { "publish-new-symlink-race", NEW_SYMLINK_RACE, COPY, FALSE, FALSE, FALSE, TRUE },
    { "publish-exchange-race", EXCHANGE_RACE, COPY, FALSE, TRUE },
    { "exchange-unsupported", EXCHANGE_UNSUPPORTED, COPY, FALSE, TRUE },
    { "exchange-einval", EXCHANGE_EINVAL, COPY, FALSE, TRUE },
    { "exchange-eopnotsupp", EXCHANGE_EOPNOTSUPP, COPY, FALSE, TRUE },
    { "backend-exchange-einval-copy", BACKEND_EXCHANGE_EINVAL, COPY, FALSE, TRUE },
    { "backend-exchange-eopnotsupp-copy", BACKEND_EXCHANGE_EOPNOTSUPP, COPY, FALSE, TRUE },
    { "backend-exchange-enosys-copy", BACKEND_EXCHANGE_ENOSYS, COPY, FALSE, TRUE },
    { "backend-exchange-einval-native", BACKEND_EXCHANGE_EINVAL, MOVE, FALSE, TRUE },
    { "backend-exchange-eopnotsupp-native", BACKEND_EXCHANGE_EOPNOTSUPP, MOVE, FALSE, TRUE },
    { "backend-exchange-enosys-native", BACKEND_EXCHANGE_ENOSYS, MOVE, FALSE, TRUE },
    { "backend-exchange-einval-crossfs", BACKEND_EXCHANGE_EINVAL, MOVE, TRUE, TRUE },
    { "backend-exchange-eopnotsupp-crossfs", BACKEND_EXCHANGE_EOPNOTSUPP, MOVE, TRUE, TRUE },
    { "backend-exchange-enosys-crossfs", BACKEND_EXCHANGE_ENOSYS, MOVE, TRUE, TRUE },
    { "publish-eintr", RENAME_EINTR, COPY },
    { "publish-eio", RENAME_EIO, COPY },
    { "copy-publish-fsync", PUBLISH_FSYNC, COPY },
    { "recovery-fsync", RECOVERY_FSYNC, COPY },
    { "readable-recovery-details", RECOVERY_READABLE, COPY, FALSE, TRUE },
    { "copied-recovery-close", RECOVERY_CLOSE, MOVE, TRUE },
    { "symlink-syncfs", LINK_SYNCFS, COPY, FALSE, FALSE, FALSE, TRUE },
    { "copied-symlink-syncfs", LINK_SYNCFS, MOVE, TRUE, FALSE, FALSE, TRUE },
    { "writer-close", WRITER_CLOSE, COPY },
    { "directory-close", DIRECTORY_CLOSE, COPY },
    { "native-file", FAULT_NONE, MOVE },
    { "native-undo-roundtrip", FAULT_NONE, MOVE },
    { "native-symlink", FAULT_NONE, MOVE, FALSE, FALSE, FALSE, TRUE },
    { "native-directory", FAULT_NONE, MOVE, FALSE, FALSE, TRUE },
    { "native-replacement", FAULT_NONE, MOVE, FALSE, TRUE },
    { "native-source-fsync", SOURCE_FSYNC, MOVE },
    { "native-tree-fsync", TREE_FSYNC, MOVE, FALSE, FALSE, TRUE },
    { "native-publish-fsync", PUBLISH_FSYNC, MOVE },
    { "native-publish-eintr", RENAME_EINTR, MOVE },
    { "native-new-file-race", NEW_FILE_RACE, MOVE },
    { "native-directory-race", NEW_DIRECTORY_RACE, MOVE, FALSE, FALSE, TRUE },
    { "native-capture-swap", CAPTURE_SWAP, MOVE },
    { "native-capture-result-eio", CAPTURE_RESULT_EIO, MOVE },
    { "copied-capture-result-eio", CAPTURE_RESULT_EIO, MOVE, TRUE },
    { "copy-publish-result-eio", PUBLISH_RESULT_EIO, COPY },
    { "native-publish-result-eio", PUBLISH_RESULT_EIO, MOVE },
    { "exchange-result-eio", PUBLISH_RESULT_EIO, COPY, FALSE, TRUE },
    { "native-exchange-result-eio", PUBLISH_RESULT_EIO, MOVE, FALSE, TRUE },
    { "copy-publish-result-eintr", PUBLISH_RESULT_EINTR, COPY },
    { "exchange-result-eintr", PUBLISH_RESULT_EINTR, COPY, FALSE, TRUE },
    { "copy-final-anchor-close", FINAL_ANCHOR_CLOSE, COPY },
    { "native-final-anchor-close", FINAL_ANCHOR_CLOSE, MOVE },
    { "copied-source-reclaimed", FAULT_NONE, MOVE, TRUE },
    { "copied-source-swap", CAPTURE_SWAP, MOVE, TRUE },
    { "copied-source-capture-failure", CAPTURE_FAILURE, MOVE, TRUE },
    { "copied-source-capture-cancel", CAPTURE_CANCEL, MOVE, TRUE },
    { "copied-source-delete-failure", DELETE_FAILURE, MOVE, TRUE },
    { "copied-source-delete-fsync", DELETE_DIR_FSYNC, MOVE, TRUE },
    { "copied-source-occupied-restore", CAPTURE_CHANGED_OCCUPIED, MOVE, TRUE },
    { "move-target-prompt-change", PROMPT_TARGET_MOVE, MOVE, TRUE, TRUE },
    { "move-target-copy-change", COPYING_TARGET_MOVE, MOVE, TRUE, TRUE },
    { "enqueue-root-swap", ROOT_SWAP, COPY },
    { "enqueue-ancestor-swap", ANCESTOR_SWAP, COPY },
    { "enqueue-source-parent-swap", SOURCE_PARENT_SWAP, MOVE, TRUE },
    { "enqueue-mount-swap", MOUNT_SWAP, COPY },
    { "custom-name-tree", FAULT_NONE, COPY_FILE, FALSE, FALSE, TRUE },
    { "recovery-finalization", FAULT_NONE, COPY, FALSE, TRUE },
    { "recovery-entry-details", FAULT_NONE, COPY, FALSE, TRUE },
    { "history-releases-anchors", FAULT_NONE, COPY, FALSE, TRUE },
    { "large-copy-low-fd", FAULT_NONE, COPY, FALSE, FALSE, TRUE },
    { "large-move-low-fd", FAULT_NONE, MOVE, TRUE, FALSE, TRUE },
    { "large-move-low-fd-failure", LARGE_WRITE_SYNC, MOVE, TRUE, FALSE, TRUE },
    { "bulk-copy-recovery-bounded", FAULT_NONE, COPY, FALSE, FALSE, TRUE },
    { "bulk-copy-recovery-preserved", RECOVERY_UNKNOWN, COPY, FALSE, TRUE, TRUE },
    { "namespace-copy", FAULT_NONE, COPY },
    { "namespace-native-move", FAULT_NONE, MOVE },
    { "namespace-copied-move", FAULT_NONE, MOVE, TRUE },
    { "recovery-undo-check-no-mutation", FAULT_NONE, COPY },
    { "recovery-reserved-container", RECOVERY_RESERVED, COPY },
    { "recovery-group-writable", RECOVERY_GROUP_WRITABLE, COPY },
    { "recovery-other-writable", RECOVERY_OTHER_WRITABLE, COPY },
    { "recovery-marker-inode", MARKER_INODE, COPY },
    { "recovery-marker-metadata", MARKER_METADATA, COPY },
    { "recovery-marker-content", MARKER_CONTENT, COPY },
    { "recovery-record-inode", RECORD_INODE, COPY },
    { "recovery-record-metadata", RECORD_METADATA, COPY },
    { "recovery-record-content", RECORD_CONTENT, COPY },
    { "recovery-record-nlink", RECORD_NLINK, COPY },
    { "recovery-record-uid", RECORD_UID, COPY },
    { "recovery-foreign-payload", RECOVERY_FOREIGN_PAYLOAD, COPY },
    { "recovery-foreign-child", RECOVERY_FOREIGN_CHILD, COPY },
    { "undo-replacement", FAULT_NONE, COPY, FALSE, TRUE },
    { "undo-newer-target", FAULT_NONE, COPY },
    { "undo-newer-source", FAULT_NONE, MOVE },
    { "undo-capture-race", UNDO_TARGET_RACE, COPY },
    { "undo-restore-race", UNDO_RESTORE_RACE, COPY, FALSE, TRUE },
    { "undo-fsync", UNDO_FSYNC, COPY },
    { "undo-postcapture-fsync", UNDO_POSTCAPTURE_FSYNC, COPY },
    { "undo-retry-then-postcapture", UNDO_POSTCAPTURE_FSYNC, COPY },
    { "undo-cancel-before-execution", UNDO_CANCEL_PREFLIGHT, COPY },
    { "undo-cancel-after-capture", UNDO_CANCEL_CAPTURE, COPY },
    { "undo-new-child", FAULT_NONE, COPY, FALSE, FALSE, TRUE },
    { "undo-merged-directory", FAULT_NONE, COPY, FALSE, TRUE, TRUE },
    { "undo-merge-new-subdirectory", FAULT_NONE, COPY, FALSE, TRUE, TRUE },
    { "undo-folder-refused", FAULT_NONE, COPY, FALSE, FALSE, TRUE },
    { "undo-native-folder-refused", FAULT_NONE, MOVE, FALSE, FALSE, TRUE },
    { "undo-crossfs-refused", FAULT_NONE, MOVE, TRUE },
    { "undo-mixed-file-folder", FAULT_NONE, MOVE },
    { "undo-mixed-folder-file", FAULT_NONE, MOVE },
    { "undo-no-record-refused", FAULT_NONE, COPY },
    { "undo-reopened-root-swap", FAULT_NONE, COPY },
    { "redo-newer-target", FAULT_NONE, COPY },
};

static const char payload[] = "Verified payload owned by this transfer.\n";
static const char previous[] = "Prior target: recovery must outlive undo history.\n";
static const char foreign[] = "Concurrent writer: do not overwrite or delete.\n";
static const char *
secondary_fs_root (void)
{
    const char *root = g_getenv ("NEMO_TEST_CROSS_FS_ROOT");
    return root != NULL ? root : "/dev/shm";
}

static const Scenario *scenario;
typedef struct {
    char *source, *destination, *destination_parent, *contents;
    guint published_at, synced_at, deleted_at;
} LargeFile;

static struct {
    char *root, *source_dir, *dest_dir, *source, *dest, *saved, *capture;
    char *extra_source, *extra_dest, *undo_capture;
    GThread *main_thread;
    GMutex lock;
    GCond ready;
    gboolean queued, active, undoing, done, success, finished, timed_out;
    guint callbacks, finished_count, injections, source_flushes, tree_flushes;
    guint mutations, publications, exchanges, captures, deletes, writes, writer_flushes;
    guint source_reads, destination_flushes, conflicts, sequence, published_at, synced_at;
    guint source_queries;
    guint record_flushes;
    guint undo_captures, manager_events;
    guint capture_calls, publication_calls;
    guint exchange_attempts, exchange_probes, stage_creations;
    GHashTable *writers;
    GHashTable *anchors;
    GHashTable *undo_targets;
    guint undo_entries, safe_undo_records;
    gboolean adding_transfer;
    int root_anchor_fd;
    gboolean anchor_closed;
    int recovery_fd;
    gboolean recovery_closed;
    struct stat source_identity;
    struct stat destination_identity;
    struct stat merged_root_identity, merged_nested_identity;
    NemoProgressInfo *progress;
    NemoProgressInfo *undo_progress;
    NemoProgressResult result;
    NemoProgressInfoManager *manager;
    GPtrArray *large_files;
    GHashTable *large_destinations, *large_sources, *large_captures;
    guint large_directories, large_publications;
    char *unknown_recovery_entry;
    GString *dialog_text;
    NemoTransferUndo *preflight_record;
    char *protected_path, *retained_record;
    GString *protected_snapshot;
    char *trash_root, *trash_original;
    GString *trash_snapshot;
    guint trash_uris, trash_metadata, trash_records;
    uid_t record_uid;
    guint record_uid_fd_reads, record_uid_named_reads;
} f;

static gboolean
named (const char *name)
{
    return strcmp (scenario->name, name) == 0;
}

static gboolean
large_case (void)
{
    return g_str_has_prefix (scenario->name, "large-") ||
           g_str_has_prefix (scenario->name, "bulk-");
}

static int
exchange_backend_errno (void)
{
    switch (scenario->fault) {
    case BACKEND_EXCHANGE_EINVAL: return EINVAL;
    case BACKEND_EXCHANGE_EOPNOTSUPP: return EOPNOTSUPP;
    case BACKEND_EXCHANGE_ENOSYS: return ENOSYS;
    default: return 0;
    }
}

static gboolean
changed_marker (void)
{
    return scenario->fault == MARKER_INODE || scenario->fault == MARKER_METADATA ||
           scenario->fault == MARKER_CONTENT;
}

static gboolean
changed_record (void)
{
    return scenario->fault == RECORD_INODE || scenario->fault == RECORD_METADATA ||
           scenario->fault == RECORD_CONTENT || scenario->fault == RECORD_NLINK ||
           scenario->fault == RECORD_UID;
}

static gboolean
retained_recovery_entry (void)
{
    return changed_record () || scenario->fault == RECOVERY_FOREIGN_PAYLOAD ||
           scenario->fault == RECOVERY_FOREIGN_CHILD;
}

static gboolean
low_fd_case (void)
{
    return g_str_has_prefix (scenario->name, "large-");
}

static gboolean
mixed_undo (void)
{
    return named ("undo-mixed-file-folder") || named ("undo-mixed-folder-file");
}

static gboolean
checked_no_backup_move (void)
{
    return named ("native-file") || named ("copied-source-reclaimed") ||
           named ("native-undo-roundtrip");
}

static gboolean
under (const char *path, const char *root)
{
    return path && root && g_str_has_prefix (path, root) &&
           (path[strlen (root)] == '/' || path[strlen (root)] == '\0');
}

static gboolean
destination_path (const char *path)
{
    return under (path, f.dest_dir) ||
           (scenario->operation == DUPLICATE && under (path, f.source_dir));
}

static gboolean
exists (const char *path)
{
    struct stat st;
    return path && lstat (path, &st) == 0;
}

static char *
fd_path (int fd)
{
    char proc[64], path[4096];
    g_snprintf (proc, sizeof proc, "/proc/self/fd/%d", fd);
    ssize_t len = readlink (proc, path, sizeof path - 1);
    if (len < 0)
        return NULL;
    path[len] = '\0';
    return g_strdup (path);
}

static char *
resolved (int dirfd, const char *path)
{
    if (g_str_has_prefix (path, "/proc/self/fd/")) {
        char *end;
        int fd = strtol (path + strlen ("/proc/self/fd/"), &end, 10);
        g_autofree char *parent = fd_path (fd);
        return parent ? g_build_filename (parent, *end == '/' ? end + 1 : "", NULL) :
                        g_strdup (path);
    }
    if (g_path_is_absolute (path))
        return g_strdup (path);
    g_autofree char *parent = dirfd == AT_FDCWD ? g_get_current_dir () : fd_path (dirfd);
    return parent ? g_build_filename (parent, path, NULL) : g_strdup (path);
}

static gboolean
queue_fault (void)
{
    return scenario->fault == ROOT_SWAP || scenario->fault == ANCESTOR_SWAP ||
           scenario->fault == SOURCE_PARENT_SWAP || scenario->fault == MOUNT_SWAP;
}

static void
worker_barrier (void)
{
    if (!f.active || !queue_fault () || g_thread_self () == f.main_thread)
        return;
    g_mutex_lock (&f.lock);
    while (!f.queued)
        g_cond_wait (&f.ready, &f.lock);
    g_mutex_unlock (&f.lock);
}

static void
assert_contents (const char *path, const char *expected)
{
    g_autofree char *contents = NULL;
    gsize length;
    g_assert_true (g_file_get_contents (path, &contents, &length, NULL));
    g_assert_cmpuint (length, ==, strlen (expected));
    g_assert_cmpstr (contents, ==, expected);
}

static void
write_contents (const char *path, const char *contents)
{
    g_assert_true (g_file_set_contents (path, contents, -1, NULL));
}

static void
large_file_free (gpointer data)
{
    LargeFile *file = data;
    g_free (file->source);
    g_free (file->destination);
    g_free (file->destination_parent);
    g_free (file->contents);
    g_free (file);
}

static void
add_large_file (const char *relative)
{
    LargeFile *file = g_new0 (LargeFile, 1);
    file->source = g_build_filename (f.source, relative, NULL);
    file->destination = g_build_filename (f.dest, relative, NULL);
    file->destination_parent = g_path_get_dirname (file->destination);
    file->contents = g_strdup_printf ("Large transfer fixture: %s\n", relative);
    write_contents (file->source, file->contents);
    g_hash_table_insert (f.large_destinations, g_strdup (file->destination), file);
    g_hash_table_insert (f.large_sources, g_strdup (file->source), file);
    g_ptr_array_add (f.large_files, file);
}

static void
create_large_fixture (void)
{
    f.large_files = g_ptr_array_new_with_free_func (large_file_free);
    f.large_destinations = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    f.large_sources = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    f.large_captures = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_assert_cmpint (g_mkdir (f.source, 0700), ==, 0);
    f.large_directories = 1;
    if (low_fd_case ()) {
        for (guint group = 0; group < 16; group++) {
            g_autofree char *group_name = g_strdup_printf ("group-%02u", group);
            g_autofree char *group_path = g_build_filename (f.source, group_name, NULL);
            g_assert_cmpint (g_mkdir (group_path, 0700), ==, 0);
            f.large_directories++;
            for (guint leaf = 0; leaf < 36; leaf++) {
                g_autofree char *leaf_name = g_strdup_printf ("leaf-%02u", leaf);
                g_autofree char *leaf_path = g_build_filename (group_path, leaf_name, NULL);
                g_assert_cmpint (g_mkdir (leaf_path, 0700), ==, 0);
                f.large_directories++;
                g_autofree char *relative = g_build_filename (group_name, leaf_name, "data", NULL);
                add_large_file (relative);
            }
        }
        g_assert_cmpuint (f.large_directories, ==, 593);
        g_assert_cmpuint (f.large_files->len, ==, 576);
    } else {
        for (guint i = 0; i < 512; i++) {
            g_autofree char *relative = g_strdup_printf ("file-%04u", i);
            add_large_file (relative);
        }
    }
    if (scenario->replace) {
        g_assert_cmpint (g_mkdir (f.dest, 0700), ==, 0);
        LargeFile *first = g_ptr_array_index (f.large_files, 0);
        write_contents (first->destination, previous);
    }
}

static void
check_large_contents (void)
{
    for (guint i = 0; i < f.large_files->len; i++) {
        LargeFile *file = g_ptr_array_index (f.large_files, i);
        gboolean source_exists = exists (file->source);
        gboolean destination_exists = exists (file->destination);
        g_assert_true (source_exists || destination_exists);
        if (source_exists)
            assert_contents (file->source, file->contents);
        if (destination_exists)
            assert_contents (file->destination, file->contents);
        if (scenario->operation == COPY || scenario->fault == LARGE_WRITE_SYNC)
            g_assert_true (source_exists);
        if (f.success) {
            g_assert_true (destination_exists);
            if (scenario->operation == MOVE) {
                g_assert_false (source_exists);
                g_assert_cmpuint (file->synced_at, >, file->published_at);
                g_assert_cmpuint (file->deleted_at, >, file->published_at);
            }
        }
        if (scenario->fault == LARGE_WRITE_SYNC)
            g_assert_false (destination_exists);
    }
    g_test_message ("%u/%u large-fixture payloads published; %u source directories",
                    f.large_publications, f.large_files->len, f.large_directories);
}

static guint
count_root_recovery_directories (const char *root)
{
    GDir *directory = g_dir_open (root, 0, NULL);
    g_assert_nonnull (directory);
    guint count = 0;
    const char *name;
    while ((name = g_dir_read_name (directory))) {
        if (g_str_has_prefix (name, ".nemo-recovery-")) {
            g_autofree char *path = g_build_filename (root, name, NULL);
            struct stat st;
            g_assert_cmpint (lstat (path, &st), ==, 0);
            g_assert_true (S_ISDIR (st.st_mode));
            count++;
        }
    }
    g_dir_close (directory);
    return count;
}

typedef struct {
    guint entries, buckets, owners, transactions, other_directories, other_files;
} RecoveryNamespaceCounts;

static void
count_recovery_namespace (const char *path, guint depth, gboolean marker_directory,
                          RecoveryNamespaceCounts *counts)
{
    struct stat identity;
    g_assert_cmpint (lstat (path, &identity), ==, 0);
    g_assert_true (S_ISDIR (identity.st_mode));
    g_assert_cmpuint (identity.st_uid, ==, geteuid ());
    g_assert_cmpuint (identity.st_mode & 0022, ==, 0);
    GDir *directory = g_dir_open (path, 0, NULL);
    g_assert_nonnull (directory);
    const char *name;
    guint owners = 0;
    while ((name = g_dir_read_name (directory))) {
        g_autofree char *child = g_build_filename (path, name, NULL);
        struct stat st;
        g_assert_cmpint (lstat (child, &st), ==, 0);
        counts->entries++;
        if (S_ISDIR (st.st_mode)) {
            gboolean bucket = depth == 0 && strlen (name) == 2 &&
                              strspn (name, "0123456789abcdef") == 2;
            if (g_str_has_prefix (name, "transaction-"))
                counts->transactions++;
            if (bucket)
                counts->buckets++;
            else
                counts->other_directories++;
            count_recovery_namespace (child, depth + 1, bucket, counts);
        } else if (marker_directory && S_ISREG (st.st_mode) && strcmp (name, "owner") == 0) {
            g_assert_cmpuint (st.st_uid, ==, geteuid ());
            g_assert_cmpuint (st.st_mode & 0022, ==, 0);
            counts->owners++;
            owners++;
        } else {
            counts->other_files++;
        }
    }
    g_dir_close (directory);
    if (marker_directory)
        g_assert_cmpuint (owners, ==, 1);
}

static guint
assert_completed_recovery_namespace (const char *root)
{
    GDir *directory = g_dir_open (root, 0, NULL);
    g_assert_nonnull (directory);
    g_autofree char *expected = g_strdup_printf (".nemo-recovery-%" G_GUINT64_FORMAT,
                                                (guint64) geteuid ());
    const char *name;
    guint containers = 0, entries = 0;
    while ((name = g_dir_read_name (directory))) {
        if (!g_str_has_prefix (name, ".nemo-recovery-"))
            continue;
        g_assert_cmpstr (name, ==, expected);
        containers++;
        g_autofree char *path = g_build_filename (root, name, NULL);
        RecoveryNamespaceCounts counts = { 0 };
        count_recovery_namespace (path, 0, TRUE, &counts);
        g_test_message ("Completed recovery namespace: %u transactions, %u owners, %u buckets, %u entries",
                        counts.transactions, counts.owners, counts.buckets, counts.entries);
        g_assert_cmpuint (counts.transactions, ==, 0);
        g_assert_cmpuint (counts.other_directories, ==, 0);
        g_assert_cmpuint (counts.other_files, ==, 0);
        g_assert_cmpuint (counts.buckets, <=, 256);
        g_assert_cmpuint (counts.owners, ==, 1 + counts.buckets);
        g_assert_cmpuint (counts.entries, ==, 1 + 2 * counts.buckets);
        g_assert_cmpuint (counts.entries, <=, 513);
        entries += 1 + counts.entries;
    }
    g_dir_close (directory);
    g_assert_cmpuint (containers, <=, 1);
    return entries;
}

static void
snapshot_namespace (const char *path, GString *snapshot)
{
    struct stat st;
    g_assert_cmpint (lstat (path, &st), ==, 0);
    g_string_append_printf (snapshot, "%s:%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT
                            ":%o:%u:%u:%" G_GINT64_FORMAT ":%" G_GUINT64_FORMAT
                            ":%" G_GINT64_FORMAT ".%ld:%" G_GINT64_FORMAT ".%ld\n",
                            path, (guint64) st.st_dev, (guint64) st.st_ino, st.st_mode,
                            st.st_uid, st.st_gid, (gint64) st.st_size, (guint64) st.st_nlink,
                            (gint64) st.st_mtim.tv_sec, st.st_mtim.tv_nsec,
                            (gint64) st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    if (S_ISDIR (st.st_mode)) {
        GDir *directory = g_dir_open (path, 0, NULL);
        g_assert_nonnull (directory);
        GList *names = NULL;
        const char *name;
        while ((name = g_dir_read_name (directory)))
            names = g_list_prepend (names, g_strdup (name));
        g_dir_close (directory);
        names = g_list_sort (names, (GCompareFunc) g_strcmp0);
        for (GList *l = names; l; l = l->next) {
            g_autofree char *child = g_build_filename (path, l->data, NULL);
            snapshot_namespace (child, snapshot);
        }
        g_list_free_full (names, g_free);
    } else if (S_ISREG (st.st_mode)) {
        g_autofree char *contents = NULL;
        gsize length;
        g_assert_true (g_file_get_contents (path, &contents, &length, NULL));
        g_autofree char *checksum = g_compute_checksum_for_data (
            G_CHECKSUM_SHA256, (const guchar *) contents, length);
        g_string_append_printf (snapshot, "%s\n", checksum);
    } else {
        g_assert_true (S_ISLNK (st.st_mode));
        g_autofree char *target = g_file_read_link (path, NULL);
        g_assert_nonnull (target);
        g_string_append_printf (snapshot, "%s\n", target);
    }
}

static char *
recovery_container_path (void)
{
    g_autofree char *name = g_strdup_printf (".nemo-recovery-%" G_GUINT64_FORMAT,
                                           (guint64) geteuid ());
    return g_build_filename (f.dest_dir, name, NULL);
}

static void
protect_entry (const char *path)
{
    g_assert_null (f.protected_path);
    f.protected_path = g_strdup (path);
    f.protected_snapshot = g_string_new (NULL);
    snapshot_namespace (path, f.protected_snapshot);
}

static void
assert_protected_entry (void)
{
    g_assert_nonnull (f.protected_path);
    GString *current = g_string_new (NULL);
    snapshot_namespace (f.protected_path, current);
    g_assert_cmpstr (current->str, ==, f.protected_snapshot->str);
    g_string_free (current, TRUE);
    if (f.retained_record)
        g_assert_true (exists (f.retained_record));
}

static void
change_recovery_entry (int recovery_fd)
{
    g_autofree char *directory = fd_path (recovery_fd);
    g_assert_nonnull (directory);
    f.retained_record = g_build_filename (directory, "record-prepared", NULL);
    g_assert_true (exists (f.retained_record));
    g_autofree char *path = NULL;
    if (changed_marker ()) {
        g_autofree char *container = recovery_container_path ();
        path = g_build_filename (container, "owner", NULL);
    } else if (changed_record ())
        path = g_strdup (f.retained_record);
    else
        path = g_build_filename (directory, scenario->fault == RECOVERY_FOREIGN_PAYLOAD ?
                                            "payload" : "foreign-child", NULL);
    if (changed_marker () || changed_record ()) {
        struct stat before, after;
        g_assert_cmpint (lstat (path, &before), ==, 0);
        g_assert_true (S_ISREG (before.st_mode));
        g_autofree char *contents = NULL;
        gsize length;
        g_assert_true (g_file_get_contents (path, &contents, &length, NULL));
        if (scenario->fault == MARKER_INODE || scenario->fault == RECORD_INODE) {
            /* Keep the namespace otherwise unchanged, so retention cannot be
             * attributed to an extra unknown sibling instead of the new inode. */
            write_contents (f.saved, contents);
            g_assert_cmpint (rename (f.saved, path), ==, 0);
        } else if (scenario->fault == MARKER_METADATA || scenario->fault == RECORD_METADATA) {
            g_assert_cmpint (chmod (path, (before.st_mode & 0777) ^ S_IRGRP), ==, 0);
        } else if (scenario->fault == RECORD_NLINK) {
            g_assert_cmpint (link (path, f.saved), ==, 0);
        } else if (scenario->fault == RECORD_UID) {
            f.record_uid = before.st_uid;
            g_assert_cmpuint (f.record_uid, ==, geteuid ());
        } else {
            g_assert_cmpuint (length, >, 2);
            char byte = contents[length - 2] == 'a' ? 'b' : 'a';
            int fd = open (path, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
            g_assert_cmpint (fd, >=, 0);
            g_assert_cmpint (pwrite (fd, &byte, 1, length - 2), ==, 1);
            const struct timespec original_times[] = { before.st_atim, before.st_mtim };
            g_assert_cmpint (futimens (fd, original_times), ==, 0);
            g_assert_cmpint (fsync (fd), ==, 0);
            g_assert_cmpint (close (fd), ==, 0);
        }
        g_assert_cmpint (lstat (path, &after), ==, 0);
        if (scenario->fault == MARKER_INODE || scenario->fault == RECORD_INODE) {
            g_assert_cmpuint (before.st_ino, !=, after.st_ino);
            assert_contents (path, contents);
        } else {
            g_assert_cmpuint (before.st_ino, ==, after.st_ino);
            if (scenario->fault == MARKER_METADATA || scenario->fault == RECORD_METADATA) {
                g_assert_cmpuint (before.st_mode, !=, after.st_mode);
                assert_contents (path, contents);
            } else if (scenario->fault == RECORD_NLINK) {
                g_assert_cmpuint (after.st_nlink, ==, before.st_nlink + 1);
                assert_contents (path, contents);
                assert_contents (f.saved, contents);
            } else if (scenario->fault == RECORD_UID) {
                g_assert_cmpuint (after.st_uid, ==, before.st_uid);
                assert_contents (path, contents);
            } else {
                g_autofree char *altered = NULL;
                g_assert_true (g_file_get_contents (path, &altered, NULL, NULL));
                g_assert_cmpstr (altered, !=, contents);
                g_assert_cmpint (after.st_mtim.tv_sec, ==, before.st_mtim.tv_sec);
                g_assert_cmpint (after.st_mtim.tv_nsec, ==, before.st_mtim.tv_nsec);
            }
        }
    } else if (scenario->fault == RECOVERY_FOREIGN_PAYLOAD) {
        g_assert_false (exists (path));
        write_contents (path, foreign);
    } else {
        g_assert_cmpint (g_mkdir (path, 0700), ==, 0);
        g_autofree char *child = g_build_filename (path, "untouched", NULL);
        write_contents (child, foreign);
    }
    protect_entry (path);
    f.injections++;
}

static void
lower_fd_limit (struct rlimit *saved)
{
    g_assert_cmpint (getrlimit (RLIMIT_NOFILE, saved), ==, 0);
    g_assert_cmpuint (saved->rlim_cur, >=, 64);
    struct rlimit limited = *saved;
    limited.rlim_cur = 64;
    g_assert_cmpint (setrlimit (RLIMIT_NOFILE, &limited), ==, 0);
    struct rlimit current;
    g_assert_cmpint (getrlimit (RLIMIT_NOFILE, &current), ==, 0);
    g_assert_cmpuint (current.rlim_cur, ==, 64);
    g_test_message ("RLIMIT_NOFILE=64 for %u directories and %u files",
                    f.large_directories, f.large_files->len);
}

int __real_fstatfs (int, struct statfs *);
int
__wrap_fstatfs (int fd, struct statfs *st)
{
    worker_barrier ();
    int ret = __real_fstatfs (fd, st);
    g_autofree char *path = f.active ? fd_path (fd) : NULL;
    if (ret == 0 && under (path, f.dest_dir)) {
        long type = scenario->fault == BACKEND_NFS ? NFS_SUPER_MAGIC :
                    scenario->fault == BACKEND_FUSE ? FUSE_SUPER_MAGIC :
                    scenario->fault == BACKEND_TMPFS ? TMPFS_MAGIC :
                    scenario->fault == BACKEND_UNKNOWN ? 0x1234 : 0;
        if (type) {
            st->f_type = type;
            f.injections++;
        }
    }
    return ret;
}

static void
report_record_uid_change (const char *path, uid_t *uid, gboolean descriptor)
{
    if (!f.active || scenario->fault != RECORD_UID || !f.protected_path ||
        g_strcmp0 (path, f.protected_path) != 0)
        return;
    /* Exercise observed ownership changes without privileged chown operations. */
    g_assert_cmpuint (*uid, ==, f.record_uid);
    *uid ^= 1;
    if (descriptor)
        f.record_uid_fd_reads++;
    else
        f.record_uid_named_reads++;
}

int __real_fstat64 (int, struct stat64 *);
int
__wrap_fstat64 (int fd, struct stat64 *st)
{
    int result = __real_fstat64 (fd, st);
    if (result == 0 && f.active && scenario->fault == RECORD_UID) {
        g_autofree char *path = fd_path (fd);
        report_record_uid_change (path, &st->st_uid, TRUE);
    }
    return result;
}

int __real_fstatat64 (int, const char *, struct stat64 *, int);
int
__wrap_fstatat64 (int fd, const char *name, struct stat64 *st, int flags)
{
    int result = __real_fstatat64 (fd, name, st, flags);
    if (result == 0 && f.active && scenario->fault == RECORD_UID) {
        g_autofree char *path = resolved (fd, name);
        report_record_uid_change (path, &st->st_uid, FALSE);
    }
    return result;
}

int __real_fstatfs64 (int, struct statfs64 *);
int
__wrap_fstatfs64 (int fd, struct statfs64 *st)
{
    worker_barrier ();
    int ret = __real_fstatfs64 (fd, st);
    g_autofree char *path = f.active ? fd_path (fd) : NULL;
    if (ret == 0 && under (path, f.dest_dir)) {
        long type = scenario->fault == BACKEND_NFS ? NFS_SUPER_MAGIC :
                    scenario->fault == BACKEND_FUSE ? FUSE_SUPER_MAGIC :
                    scenario->fault == BACKEND_TMPFS ? TMPFS_MAGIC :
                    scenario->fault == BACKEND_UNKNOWN ? 0x1234 : 0;
        if (type) {
            st->f_type = type;
            f.injections++;
        }
    }
    return ret;
}

int __real_statx (int, const char *, int, unsigned int, struct statx *);
int
__wrap_statx (int fd, const char *path, int flags, unsigned int mask, struct statx *st)
{
    worker_barrier ();
    int ret = __real_statx (fd, path, flags, mask, st);
    g_autofree char *actual = f.active ? resolved (fd, path) : NULL;
    if (ret == 0 && f.active && (flags & AT_EMPTY_PATH) && !*path && fd >= 0 &&
        (g_thread_self () == f.main_thread || f.undoing) &&
        (g_strcmp0 (actual, f.dest_dir) == 0 || g_strcmp0 (actual, f.source_dir) == 0)) {
        g_hash_table_insert (f.anchors, GINT_TO_POINTER (fd + 1), g_strdup (actual));
        if (g_strcmp0 (actual, f.dest_dir) == 0 && f.root_anchor_fd < 0)
            f.root_anchor_fd = fd;
    }
    if (ret == 0 && f.active && scenario->fault == MOUNT_SWAP &&
        g_thread_self () != f.main_thread && under (actual, f.dest_dir)) {
        st->stx_mnt_id++;
        f.injections++;
    }
    return ret;
}

char *__real_g_file_get_path (GFile *);
char *__real_g_file_get_uri (GFile *);
char *
__wrap_g_file_get_uri (GFile *file)
{
    g_autofree char *path = __real_g_file_get_path (file);
    if (f.active && named ("trash-source-copy") && under (path, f.source_dir)) {
        f.trash_uris++;
        return g_strdup (strcmp (path, f.source) == 0 ? "trash:///payload" : "trash:///");
    }
    return __real_g_file_get_uri (file);
}

gboolean __real_g_file_has_uri_scheme (GFile *, const char *);
gboolean
__wrap_g_file_has_uri_scheme (GFile *file, const char *scheme)
{
    g_autofree char *path = __real_g_file_get_path (file);
    if (f.active && named ("trash-source-copy") && under (path, f.source_dir))
        return g_ascii_strcasecmp (scheme, "trash") == 0;
    return __real_g_file_has_uri_scheme (file, scheme);
}

char *
__wrap_g_file_get_path (GFile *file)
{
    char *path = __real_g_file_get_path (file);
    if (f.active && scenario->fault == REMOTE_SOURCE && under (path, f.source_dir)) {
        g_clear_pointer (&path, g_free);
        f.injections++;
    }
    return path;
}

gboolean __real_g_file_is_native (GFile *);
gboolean
__wrap_g_file_is_native (GFile *file)
{
    g_autofree char *path = __real_g_file_get_path (file);
    if (f.active && ((scenario->fault == REMOTE_DEST && under (path, f.dest_dir)) ||
                     (scenario->fault == REMOTE_SOURCE && under (path, f.source_dir)))) {
        f.injections++;
        return FALSE;
    }
    return __real_g_file_is_native (file);
}

gboolean __real_nemo_mount_operation_is_removing (void);
gboolean
__wrap_nemo_mount_operation_is_removing (void)
{
    if (f.active && scenario->fault == REMOVAL_INFLIGHT) {
        f.injections++;
        return TRUE;
    }
    return __real_nemo_mount_operation_is_removing ();
}

GFileInfo *__real_g_file_query_info (GFile *, const char *, GFileQueryInfoFlags,
                                    GCancellable *, GError **);
GFileInfo *
__wrap_g_file_query_info (GFile *file, const char *attributes, GFileQueryInfoFlags flags,
                         GCancellable *cancel, GError **error)
{
    worker_barrier ();
    g_autofree char *path = f.active ? __real_g_file_get_path (file) : NULL;
    if (under (path, f.source_dir))
        f.source_queries++;
    GFileInfo *info = __real_g_file_query_info (file, attributes, flags, cancel, error);
    if (info && f.active && named ("trash-source-copy") && g_strcmp0 (path, f.source) == 0) {
        g_autofree char *target_uri = __real_g_file_get_uri (file);
        g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, target_uri);
        g_file_info_set_attribute_byte_string (info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH,
                                              f.trash_original);
        f.trash_metadata++;
    }
    return info;
}

static void
record_undo_target (GFile *target, gboolean safe)
{
    if (!f.active || f.undoing)
        return;
    g_autofree char *path = __real_g_file_get_path (target);
    g_autofree char *actual = path ? resolved (AT_FDCWD, path) : NULL;
    if (!destination_path (actual))
        return;
    f.undo_entries++;
    if (safe)
        f.safe_undo_records++;
    g_hash_table_add (f.undo_targets, g_strdup (actual));
}

void __real_nemo_file_undo_info_ext_add_origin_target_pair (NemoFileUndoInfoExt *, GFile *, GFile *);
void
__wrap_nemo_file_undo_info_ext_add_origin_target_pair (NemoFileUndoInfoExt *info,
                                                     GFile *origin, GFile *target)
{
    if (!f.adding_transfer)
        record_undo_target (target, FALSE);
    __real_nemo_file_undo_info_ext_add_origin_target_pair (info, origin, target);
}

void __real_nemo_file_undo_info_ext_add_transfer (NemoFileUndoInfoExt *, GFile *, GFile *,
                                                NemoTransferUndo *);
void
__wrap_nemo_file_undo_info_ext_add_transfer (NemoFileUndoInfoExt *info, GFile *origin,
                                           GFile *target, NemoTransferUndo *transfer)
{
    record_undo_target (target, transfer != NULL);
    if ((named ("recovery-undo-check-no-mutation") || named ("native-undo-roundtrip")) &&
        !f.preflight_record) {
        g_assert_nonnull (transfer);
        f.preflight_record = nemo_transfer_undo_ref (transfer);
    }
    gboolean nested = f.adding_transfer;
    f.adding_transfer = TRUE;
    __real_nemo_file_undo_info_ext_add_transfer (info, origin, target, transfer);
    f.adding_transfer = nested;
}

int __real_openat (int, const char *, int, ...);
int
__wrap_openat (int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start (args, flags);
        mode = va_arg (args, int);
        va_end (args);
    }
    worker_barrier ();
    int fd = __real_openat (dirfd, path, flags, mode);
    if (f.active && fd >= 0 && (flags & O_DIRECTORY) && f.recovery_fd < 0) {
        g_autofree char *actual = resolved (dirfd, path);
        if (under (actual, f.dest_dir) && strstr (actual, "/.nemo-recovery-"))
            f.recovery_fd = fd;
    }
    if (f.active && fd >= 0 && (flags & O_DIRECTORY) && scenario->fault == RECOVERY_RESERVED) {
        g_autofree char *actual = resolved (dirfd, path);
        if (g_strcmp0 (actual, f.protected_path) == 0)
            f.injections++;
    }
    if (f.active && fd >= 0 && (flags & O_CREAT)) {
        g_autofree char *actual = resolved (dirfd, path);
        if (destination_path (actual)) {
            f.mutations++;
            if (strstr (actual, ".nemo") && g_str_has_suffix (actual, "/payload") &&
                !(flags & O_DIRECTORY)) {
                f.stage_creations++;
                g_hash_table_insert (f.writers, GINT_TO_POINTER (fd + 1), g_strdup (actual));
            }
        }
    }
    return fd;
}

int
__wrap_openat64 (int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start (args, flags);
        mode = va_arg (args, int);
        va_end (args);
    }
    return __wrap_openat (dirfd, path, flags, mode);
}

int __real_mkdirat (int, const char *, mode_t);
int
__wrap_mkdirat (int dirfd, const char *name, mode_t mode)
{
    worker_barrier ();
    int result = __real_mkdirat (dirfd, name, mode);
    g_autofree char *path = resolved (dirfd, name);
    if (f.active && result == 0 && destination_path (path))
        f.mutations++;
    if (f.active && result == 0 && scenario->fault == RECOVERY_READABLE &&
        under (path, f.dest_dir) && strstr (path, "/.nemo-recovery-")) {
        g_assert_cmpint (fchmodat (dirfd, name, 0755, 0), ==, 0);
        f.injections++;
    }
    if (f.active && result == 0 && under (path, f.dest_dir) &&
        g_str_has_prefix (name, ".nemo-recovery-") &&
        (scenario->fault == RECOVERY_GROUP_WRITABLE || scenario->fault == RECOVERY_OTHER_WRITABLE)) {
        g_assert_cmpint (fchmodat (dirfd, name,
            scenario->fault == RECOVERY_GROUP_WRITABLE ? 0720 : 0702, 0), ==, 0);
        protect_entry (path);
        f.injections++;
    }
    return result;
}

GFileOutputStream *__real_g_file_create (GFile *, GFileCreateFlags, GCancellable *, GError **);
GFileOutputStream *
__wrap_g_file_create (GFile *file, GFileCreateFlags flags, GCancellable *cancel, GError **error)
{
    worker_barrier ();
    GFileOutputStream *stream = __real_g_file_create (file, flags, cancel, error);
    g_autofree char *path = g_file_get_path (file);
    g_autofree char *actual = path ? resolved (AT_FDCWD, path) : NULL;
    if (f.active && stream && destination_path (actual)) {
        f.mutations++;
        g_assert_cmpstr (actual, !=, f.dest);
        g_assert_true (G_IS_FILE_DESCRIPTOR_BASED (stream));
        if (strstr (actual, ".nemo") && g_str_has_suffix (actual, "/payload"))
            f.stage_creations++;
        g_hash_table_insert (f.writers, GINT_TO_POINTER (
            g_file_descriptor_based_get_fd (G_FILE_DESCRIPTOR_BASED (stream)) + 1), g_strdup (actual));
    }
    return stream;
}

gboolean __real_g_file_copy (GFile *, GFile *, GFileCopyFlags, GCancellable *,
                             GFileProgressCallback, gpointer, GError **);
gboolean
__wrap_g_file_copy (GFile *source, GFile *dest, GFileCopyFlags flags, GCancellable *cancel,
                    GFileProgressCallback progress, gpointer data, GError **error)
{
    g_autofree char *path = g_file_get_path (dest);
    g_autofree char *actual = path ? resolved (AT_FDCWD, path) : NULL;
    if (f.active && under (actual, f.dest_dir)) {
        /* Ordinary GIO overwrite must never see a public target. */
        g_assert_cmpstr (actual, !=, f.dest);
        g_assert_nonnull (strstr (actual, ".nemo"));
        f.writes++;
    }
    return __real_g_file_copy (source, dest, flags, cancel, progress, data, error);
}

gboolean __real_g_output_stream_write_all (GOutputStream *, const void *, gsize,
                                           gsize *, GCancellable *, GError **);
gboolean
__wrap_g_output_stream_write_all (GOutputStream *stream, const void *buffer, gsize count,
                                 gsize *written, GCancellable *cancel, GError **error)
{
    if (f.active)
        f.writes++;
    gboolean ok = __real_g_output_stream_write_all (stream, buffer, count, written, cancel, error);
    if (f.active && ok && scenario->fault == COPYING_TARGET_MOVE && !f.injections) {
        write_contents (f.dest, foreign);
        f.injections++;
    }
    return ok;
}

gboolean __real_g_output_stream_close (GOutputStream *, GCancellable *, GError **);
gboolean
__wrap_g_output_stream_close (GOutputStream *stream, GCancellable *cancel, GError **error)
{
    int fd = G_IS_FILE_DESCRIPTOR_BASED (stream) ?
        g_file_descriptor_based_get_fd (G_FILE_DESCRIPTOR_BASED (stream)) : -1;
    gboolean writer = f.active && fd >= 0 &&
        g_hash_table_contains (f.writers, GINT_TO_POINTER (fd + 1));
    gboolean ok = __real_g_output_stream_close (stream, cancel, error);
    if (writer)
        g_hash_table_remove (f.writers, GINT_TO_POINTER (fd + 1));
    if (writer && ok && scenario->fault == WRITER_CLOSE && f.injections == 0) {
        f.injections++;
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Injected writer close EIO");
        return FALSE;
    }
    return ok;
}

int __real_close (int);
int
__wrap_close (int fd)
{
    g_autofree char *path = f.active ? fd_path (fd) : NULL;
    struct stat st;
    gboolean directory = path && fstat (fd, &st) == 0 && S_ISDIR (st.st_mode);
    gboolean fail = directory && under (path, f.dest_dir) &&
                    scenario->fault == DIRECTORY_CLOSE && f.injections == 0;
    gboolean root_anchor = f.active && !f.anchor_closed && fd == f.root_anchor_fd &&
                           g_strcmp0 (path, f.dest_dir) == 0;
    if (root_anchor && scenario->fault == FINAL_ANCHOR_CLOSE && f.publications) {
        g_assert_cmpuint (f.synced_at, >, f.published_at);
        fail = TRUE;
    }
    gboolean recovery = f.active && !f.recovery_closed && fd == f.recovery_fd &&
                        path && strstr (path, "/.nemo-recovery-");
    if (recovery && scenario->fault == RECOVERY_CLOSE && f.publications)
        fail = TRUE;
    int result = __real_close (fd);
    if (f.active) {
        g_hash_table_remove (f.writers, GINT_TO_POINTER (fd + 1));
        g_hash_table_remove (f.anchors, GINT_TO_POINTER (fd + 1));
    }
    if (root_anchor)
        f.anchor_closed = TRUE;
    if (recovery)
        f.recovery_closed = TRUE;
    if (result == 0 && fail) {
        f.injections++;
        errno = EIO;
        return -1;
    }
    return result;
}

ssize_t __real_read (int, void *, size_t);
ssize_t
__wrap_read (int fd, void *buffer, size_t count)
{
    g_autofree char *path = f.active ? fd_path (fd) : NULL;
    if (g_strcmp0 (path, f.source) == 0)
        f.source_reads++;
    return __real_read (fd, buffer, count);
}

int __real_fsync (int);
int
__wrap_fsync (int fd)
{
    g_autofree char *path = f.active ? fd_path (fd) : NULL;
    struct stat st;
    gboolean regular = path && fstat (fd, &st) == 0 && S_ISREG (st.st_mode);
    gboolean source = g_strcmp0 (path, f.source) == 0;
    gboolean target = g_strcmp0 (path, f.dest) == 0;
    gboolean tree = regular && under (path, f.source) && !source;
    gboolean directory = path && !regular && destination_path (path);
    gboolean fail = f.active && (
        (scenario->fault == SOURCE_FSYNC && source) ||
        (scenario->fault == TREE_FSYNC && tree) ||
        (scenario->fault == MATCH_FSYNC && target) ||
        (scenario->fault == PUBLISH_FSYNC && directory && f.publications) ||
        (scenario->fault == RECOVERY_FSYNC && directory && !f.publications &&
         !f.writes && strstr (path, "/.nemo-recovery-")) ||
        (scenario->fault == DELETE_DIR_FSYNC && f.deletes &&
         under (path, f.source_dir) && !regular) ||
        (scenario->fault == UNDO_FSYNC && f.undoing && (directory || target)));
    fail |= f.active && scenario->fault == LARGE_WRITE_SYNC && !f.injections &&
            regular && (fcntl (fd, F_GETFL) & O_ACCMODE) != O_RDONLY &&
            g_strcmp0 (path, g_hash_table_lookup (f.writers, GINT_TO_POINTER (fd + 1))) == 0;
    fail |= f.active && f.undoing && scenario->fault == UNDO_POSTCAPTURE_FSYNC &&
            f.undo_captures > 0 && directory;
    if (fail) {
        f.injections++;
        errno = EIO;
        return -1;
    }
    int result = __real_fsync (fd);
    if (f.active && result == 0) {
        if (source && regular)
            f.source_flushes++;
        if (tree)
            f.tree_flushes++;
        if (regular && (fcntl (fd, F_GETFL) & O_ACCMODE) != O_RDONLY &&
            g_strcmp0 (path, g_hash_table_lookup (f.writers, GINT_TO_POINTER (fd + 1))) == 0)
            f.writer_flushes++;
        if (regular && strstr (path, "/.nemo-recovery-") &&
            !g_str_has_suffix (path, "/payload") && !g_str_has_suffix (path, "/captured-source"))
            f.record_flushes++;
        if (directory) {
            f.destination_flushes++;
            if (f.publications)
                f.synced_at = ++f.sequence;
            for (guint i = 0; f.large_files && i < f.large_files->len; i++) {
                LargeFile *file = g_ptr_array_index (f.large_files, i);
                if (file->published_at && strcmp (path, file->destination_parent) == 0)
                    file->synced_at = ++f.sequence;
            }
        }
    }
    return result;
}

int __real_renameat2 (int, const char *, int, const char *, unsigned int);

int __real_syncfs (int);
int
__wrap_syncfs (int fd)
{
    g_autofree char *path = f.active ? fd_path (fd) : NULL;
    if (f.active && scenario->fault == LINK_SYNCFS && under (path, f.dest_dir)) {
        struct stat st;
        g_assert_cmpint (fstat (fd, &st), ==, 0);
        g_assert_true (S_ISDIR (st.st_mode));
        f.injections++;
        errno = EIO;
        return -1;
    }
    return __real_syncfs (fd);
}

static int
rename_boundary (int oldfd, const char *old, int newfd, const char *new, unsigned int flags)
{
    worker_barrier ();
    g_autofree char *src = resolved (oldfd, old);
    g_autofree char *dst = resolved (newfd, new);
    gboolean ours = f.active && (under (src, f.source_dir) || under (src, f.dest_dir));
    if (f.active && flags == RENAME_EXCHANGE) {
        f.exchange_attempts++;
        if (strcmp (old, "probe-a") == 0 && strcmp (new, "probe-b") == 0)
            f.exchange_probes++;
        int unsupported = exchange_backend_errno ();
        if (unsupported) {
            g_assert_true (ours);
            f.injections++;
            errno = unsupported;
            return -1;
        }
    }
    gboolean publish = ours && g_strcmp0 (dst, f.dest) == 0 && !f.undoing;
    gboolean capture = ours && g_strcmp0 (src, f.source) == 0;
    LargeFile *large_capture = ours && f.large_sources ?
                              g_hash_table_lookup (f.large_sources, src) : NULL;
    if (large_capture) {
        g_assert_cmpuint (large_capture->published_at, >, 0);
        g_assert_cmpuint (large_capture->synced_at, >, large_capture->published_at);
        g_assert_cmpuint (flags, ==, RENAME_NOREPLACE);
    }
    gboolean undo_capture = ours && f.undoing &&
        (g_strcmp0 (src, f.dest) == 0 || g_hash_table_contains (f.undo_targets, src));
    gboolean undo_restore = ours && f.undoing && g_strcmp0 (dst, f.dest) == 0;
    if (capture)
        f.capture_calls++;
    if (publish)
        f.publication_calls++;
    if (ours) {
        g_assert_true (flags == RENAME_NOREPLACE || flags == RENAME_EXCHANGE);
        g_assert_cmpint (oldfd, !=, AT_FDCWD);
        g_assert_cmpint (newfd, !=, AT_FDCWD);
    }
    if (publish && f.injections == 0) {
        g_assert_cmpuint (f.record_flushes, >, 0);
        if (scenario->fault == EXCHANGE_RACE)
            g_assert_cmpint (rename (dst, f.saved), ==, 0);
        if (scenario->fault == NEW_FILE_RACE || scenario->fault == EXCHANGE_RACE)
            write_contents (dst, foreign);
        else if (scenario->fault == NEW_DIRECTORY_RACE) {
            g_assert_cmpint (g_mkdir (dst, 0700), ==, 0);
            g_autofree char *child = g_build_filename (dst, "new-child", NULL);
            write_contents (child, foreign);
        } else if (scenario->fault == NEW_SYMLINK_RACE)
            g_assert_cmpint (symlink ("foreign-link-text", dst), ==, 0);
        if (scenario->fault == NEW_FILE_RACE || scenario->fault == NEW_DIRECTORY_RACE ||
            scenario->fault == NEW_SYMLINK_RACE || scenario->fault == EXCHANGE_RACE)
            f.injections++;
        if (scenario->fault == EXCHANGE_UNSUPPORTED || scenario->fault == EXCHANGE_EINVAL ||
            scenario->fault == EXCHANGE_EOPNOTSUPP || scenario->fault == RENAME_EINTR ||
            scenario->fault == RENAME_EIO) {
            f.injections++;
            errno = scenario->fault == EXCHANGE_EINVAL ? EINVAL :
                    (scenario->fault == EXCHANGE_UNSUPPORTED ||
                     scenario->fault == EXCHANGE_EOPNOTSUPP) ? EOPNOTSUPP :
                    scenario->fault == RENAME_EINTR ? EINTR : EIO;
            if (scenario->fault == EXCHANGE_EINVAL || scenario->fault == EXCHANGE_EOPNOTSUPP)
                g_assert_cmpuint (flags, ==, RENAME_EXCHANGE);
            return -1;
        }
    }
    if (publish && named ("trash-source-copy")) {
        g_autofree char *directory = fd_path (oldfd);
        g_autofree char *path = g_build_filename (directory, "record-prepared", NULL);
        g_autoptr (GKeyFile) record = g_key_file_new ();
        g_assert_true (g_key_file_load_from_file (record, path, G_KEY_FILE_NONE, NULL));
        g_autofree char *encoded = g_key_file_get_string (record, "Transfer",
                                                        "source-uri-base64", NULL);
        g_assert_nonnull (encoded);
        gsize length;
        g_autofree guchar *uri = g_base64_decode (encoded, &length);
        g_assert_cmpmem (uri, length, "trash:///payload", strlen ("trash:///payload"));
        f.trash_records++;
    }
    if (capture && f.injections == 0 && scenario->fault == CAPTURE_SWAP) {
        g_assert_cmpint (rename (src, f.saved), ==, 0);
        write_contents (src, foreign);
        f.injections++;
    }
    if (capture && scenario->fault == CAPTURE_FAILURE) {
        f.injections++;
        errno = EIO;
        return -1;
    }
    if (undo_capture && scenario->fault == UNDO_TARGET_RACE && !f.injections) {
        write_contents (src, foreign);
        f.injections++;
    }
    if (undo_restore && scenario->fault == UNDO_RESTORE_RACE && !f.injections) {
        write_contents (dst, foreign);
        f.injections++;
    }
    int ret = __real_renameat2 (oldfd, old, newfd, new, flags);
    if (ours && ret == 0) {
        f.mutations++;
        if (publish && !f.injections && (changed_marker () || retained_recovery_entry ())) {
            g_assert_cmpuint (flags, ==, RENAME_NOREPLACE);
            change_recovery_entry (oldfd);
        }
        if (!f.undoing && f.large_destinations && g_hash_table_contains (f.large_destinations, dst)) {
            LargeFile *file = g_hash_table_lookup (f.large_destinations, dst);
            g_assert_cmpuint (file->published_at, ==, 0);
            file->published_at = ++f.sequence;
            f.large_publications++;
            if (scenario->fault == RECOVERY_UNKNOWN && flags == RENAME_NOREPLACE &&
                !f.unknown_recovery_entry) {
                g_autofree char *directory = fd_path (oldfd);
                g_assert_nonnull (directory);
                g_assert_true (under (directory, f.dest_dir));
                f.unknown_recovery_entry = g_build_filename (directory, "external-note", NULL);
                g_assert_false (exists (f.unknown_recovery_entry));
                write_contents (f.unknown_recovery_entry, foreign);
                f.injections++;
            }
        }
        if (large_capture)
            g_hash_table_insert (f.large_captures, g_strdup (dst), large_capture);
        if (undo_capture) {
            f.undo_captures++;
            g_free (f.undo_capture);
            f.undo_capture = g_strdup (dst);
            if (scenario->fault == UNDO_CANCEL_CAPTURE && !f.injections) {
                g_autoptr (GCancellable) cancel = nemo_progress_info_get_cancellable (f.undo_progress);
                g_cancellable_cancel (cancel);
                f.injections++;
            }
        }
        if (publish) {
            f.publications++;
            f.published_at = ++f.sequence;
            if (flags == RENAME_EXCHANGE)
                f.exchanges++;
        }
        if (capture) {
            f.captures++;
            g_free (f.capture);
            f.capture = g_strdup (dst);
            if (scenario->fault == CAPTURE_CHANGED_OCCUPIED && f.injections == 0) {
                write_contents (dst, foreign);
                write_contents (src, "New occupant of the original source name.\n");
                f.injections++;
            }
            if (scenario->fault == CAPTURE_CANCEL && f.injections == 0) {
                g_autoptr (GCancellable) cancel = nemo_progress_info_get_cancellable (f.progress);
                g_cancellable_cancel (cancel);
                f.injections++;
            }
        }
    }
    if (ret == 0 && f.injections == 0 &&
        ((capture && scenario->fault == CAPTURE_RESULT_EIO) ||
         (publish && (scenario->fault == PUBLISH_RESULT_EIO ||
                      scenario->fault == PUBLISH_RESULT_EINTR)))) {
        f.injections++;
        errno = scenario->fault == PUBLISH_RESULT_EINTR ? EINTR : EIO;
        return -1;
    }
    return ret;
}

int
__wrap_renameat2 (int oldfd, const char *old, int newfd, const char *new, unsigned int flags)
{
    return rename_boundary (oldfd, old, newfd, new, flags);
}

long __real_syscall (long, ...);
long
__wrap_syscall (long number, ...)
{
    va_list args;
    va_start (args, number);
    long result;
    if (number == SYS_renameat2) {
        int oldfd = va_arg (args, int);
        const char *old = va_arg (args, const char *);
        int newfd = va_arg (args, int);
        const char *new = va_arg (args, const char *);
        unsigned int flags = va_arg (args, unsigned int);
        result = rename_boundary (oldfd, old, newfd, new, flags);
    } else if (number == SYS_statx) {
        int fd = va_arg (args, int);
        const char *path = va_arg (args, const char *);
        int flags = va_arg (args, int);
        unsigned int mask = va_arg (args, unsigned int);
        struct statx *st = va_arg (args, struct statx *);
        result = __wrap_statx (fd, path, flags, mask, st);
    } else {
        g_error ("Add explicit test forwarding for syscall %ld", number);
    }
    va_end (args);
    return result;
}

int __real_unlinkat (int, const char *, int);
int
__wrap_unlinkat (int dirfd, const char *path, int flags)
{
    g_autofree char *actual = resolved (dirfd, path);
    gboolean capture = f.active && f.capture && g_strcmp0 (actual, f.capture) == 0;
    LargeFile *large_capture = f.active && f.large_captures ?
                              g_hash_table_lookup (f.large_captures, actual) : NULL;
    if (large_capture) {
        g_assert_cmpint (flags, ==, 0);
        g_assert_cmpuint (large_capture->published_at, >, 0);
        g_assert_cmpuint (large_capture->synced_at, >, large_capture->published_at);
    }
    if (capture && scenario->crossfs) {
        if (f.large_files) {
            g_assert_cmpint (flags, ==, AT_REMOVEDIR);
            g_assert_cmpuint (f.large_publications, ==, f.large_files->len);
            for (guint i = 0; i < f.large_files->len; i++) {
                LargeFile *file = g_ptr_array_index (f.large_files, i);
                g_assert_cmpuint (file->deleted_at, >, file->published_at);
            }
        } else {
            g_assert_cmpuint (f.publications, ==, 1);
            g_assert_cmpuint (f.synced_at, >, f.published_at);
        }
        if (scenario->fault == DELETE_FAILURE) {
            f.injections++;
            errno = EACCES;
            return -1;
        }
    }
    int ret = __real_unlinkat (dirfd, path, flags);
    if (large_capture && ret == 0)
        large_capture->deleted_at = ++f.sequence;
    if (capture && ret == 0)
        f.deletes++;
    return ret;
}

static void
remove_fixture (const char *path)
{
    struct stat st;
    g_assert_cmpint (lstat (path, &st), ==, 0);
    if (S_ISDIR (st.st_mode)) {
        GDir *dir = g_dir_open (path, 0, NULL);
        const char *name;
        g_assert_nonnull (dir);
        while ((name = g_dir_read_name (dir))) {
            g_autofree char *child = g_build_filename (path, name, NULL);
            remove_fixture (child);
        }
        g_dir_close (dir);
        g_assert_cmpint (g_rmdir (path), ==, 0);
    } else
        g_assert_cmpint (g_unlink (path), ==, 0);
}

static char *
find_contents (const char *root, const char *contents, gboolean recovery_only)
{
    GDir *dir = g_dir_open (root, 0, NULL);
    const char *name;
    char *found = NULL;
    if (!dir)
        return NULL;
    while (!found && (name = g_dir_read_name (dir))) {
        g_autofree char *path = g_build_filename (root, name, NULL);
        struct stat st;
        g_assert_cmpint (lstat (path, &st), ==, 0);
        if (S_ISDIR (st.st_mode))
            found = find_contents (path, contents, recovery_only);
        else if (S_ISREG (st.st_mode) && (!recovery_only || strstr (path, ".nemo"))) {
            g_autofree char *actual = NULL;
            if (g_file_get_contents (path, &actual, NULL, NULL) && strcmp (actual, contents) == 0)
                found = g_strdup (path);
        }
    }
    g_dir_close (dir);
    return found;
}

static void
assert_no_fixture_directory_handles (void)
{
    GDir *fds = g_dir_open ("/proc/self/fd", 0, NULL);
    g_assert_nonnull (fds);
    const char *name;
    while ((name = g_dir_read_name (fds))) {
        char *end;
        int fd = strtol (name, &end, 10);
        if (*end)
            continue;
        g_autofree char *path = fd_path (fd);
        struct stat st;
        if ((under (path, f.root) || under (path, f.source_dir)) &&
            fstat (fd, &st) == 0 && S_ISDIR (st.st_mode)) {
            g_test_message ("Fixture directory still pinned by fd %d: %s", fd, path);
            g_assert_not_reached ();
        }
    }
    g_dir_close (fds);
}

static void
collect_dialog_text (GtkWidget *widget, gpointer unused)
{
    if (GTK_IS_LABEL (widget))
        g_string_append_printf (f.dialog_text, "%s\n", gtk_label_get_text (GTK_LABEL (widget)));
    if (GTK_IS_EXPANDER (widget))
        gtk_expander_set_expanded (GTK_EXPANDER (widget), TRUE);
    if (GTK_IS_CONTAINER (widget))
        gtk_container_foreach (GTK_CONTAINER (widget), collect_dialog_text, NULL);
}

static gboolean
respond (gpointer unused)
{
    GList *windows = gtk_window_list_toplevels ();
    for (GList *l = windows; l; l = l->next) {
        if (!GTK_IS_DIALOG (l->data) || !gtk_widget_get_visible (l->data))
            continue;
        if (NEMO_IS_FILE_CONFLICT_DIALOG (l->data)) {
            f.conflicts++;
            if (scenario->fault == PROMPT_TARGET_MOVE && !f.injections) {
                write_contents (f.dest, foreign);
                f.injections++;
                gtk_dialog_response (l->data, CONFLICT_RESPONSE_REPLACE);
                continue;
            }
            gtk_dialog_response (l->data, f.injections && scenario->fault != RECOVERY_UNKNOWN ?
                                                       CONFLICT_RESPONSE_SKIP :
                                                       CONFLICT_RESPONSE_REPLACE);
        } else {
            if (GTK_IS_MESSAGE_DIALOG (l->data)) {
                collect_dialog_text (gtk_message_dialog_get_message_area (l->data), NULL);
            }
            gtk_dialog_response (l->data, 0);
        }
    }
    g_list_free (windows);
    return G_SOURCE_CONTINUE;
}

static void
copied (GHashTable *debuting, gboolean success, gpointer unused)
{
    f.callbacks++;
    g_assert_false (f.done);
    f.done = TRUE;
    f.success = success;
    g_assert_nonnull (f.progress);
    g_assert_true (nemo_progress_info_get_result (f.progress, &f.result));
}

static void
progress_finished (NemoProgressInfo *info, gpointer unused)
{
    f.finished_count++;
    f.finished = TRUE;
    g_assert_true (f.done);
    g_assert_true (nemo_progress_info_get_is_finished (info));
    gtk_main_quit ();
}

static void
progress_created (NemoProgressInfoManager *manager, NemoProgressInfo *info, gpointer unused)
{
    if (f.undoing) {
        g_set_object (&f.undo_progress, info);
        if (scenario->fault == UNDO_CANCEL_PREFLIGHT && !f.injections) {
            g_autoptr (GCancellable) cancel = nemo_progress_info_get_cancellable (info);
            g_cancellable_cancel (cancel);
            f.injections++;
        }
        return;
    }
    g_assert_null (f.progress);
    f.progress = g_object_ref (info);
    g_signal_connect (info, "finished", G_CALLBACK (progress_finished), NULL);
}

static gboolean
deadline (gpointer unused)
{
    f.timed_out = TRUE;
    gtk_main_quit ();
    return G_SOURCE_REMOVE;
}

static void
wait_for_operation (void)
{
    guint timeout = g_timeout_add_seconds (large_case () ? 150 : 15, deadline, NULL);
    guint responder = g_timeout_add (10, respond, NULL);
    gtk_main ();
    g_source_remove (responder);
    if (!f.timed_out)
        g_source_remove (timeout);
    g_assert_false (f.timed_out);
    g_assert_true (f.done);
    g_assert_cmpuint (f.callbacks, ==, 1);
}

static void
enqueue_swap (void)
{
    if (scenario->fault == ROOT_SWAP || scenario->fault == SOURCE_PARENT_SWAP) {
        const char *path = scenario->fault == ROOT_SWAP ? f.dest_dir : f.source_dir;
        g_assert_cmpint (rename (path, f.saved), ==, 0);
        g_assert_cmpint (g_mkdir (path, 0700), ==, 0);
        if (scenario->fault == SOURCE_PARENT_SWAP)
            write_contents (f.source, foreign);
        f.injections++;
    } else if (scenario->fault == ANCESTOR_SWAP) {
        g_autofree char *parent = g_path_get_dirname (f.dest_dir);
        g_assert_cmpint (rename (parent, f.saved), ==, 0);
        g_autofree char *shadow = g_build_filename (f.root, "shadow", NULL);
        g_assert_cmpint (g_mkdir (shadow, 0700), ==, 0);
        g_autofree char *dest = g_build_filename (shadow, "destination", NULL);
        g_assert_cmpint (g_mkdir (dest, 0700), ==, 0);
        g_assert_cmpint (symlink (shadow, parent), ==, 0);
        f.injections++;
    }
    g_mutex_lock (&f.lock);
    f.queued = TRUE;
    g_cond_broadcast (&f.ready);
    g_mutex_unlock (&f.lock);
}

static void
run_operation (Operation operation, int verify_override)
{
    f.done = f.finished = f.success = FALSE;
    f.callbacks = f.finished_count = 0;
    g_clear_object (&f.progress);
    if (verify_override >= 0)
        nemo_file_operations_set_verify_copies (verify_override);
    g_autoptr (GFile) source = g_file_new_for_path (f.source);
    g_autoptr (GFile) destination = g_file_new_for_path (f.dest_dir);
    GList files = { source, NULL, NULL };
    g_autoptr (GFile) extra = f.extra_source ? g_file_new_for_path (f.extra_source) : NULL;
    GList second = { extra, NULL, NULL };
    GList *sources = &files;
    if (extra) {
        if (named ("undo-mixed-folder-file")) {
            second.next = &files;
            files.prev = &second;
            sources = &second;
        } else {
            files.next = &second;
            second.prev = &files;
        }
    }
    f.active = TRUE;
    switch (operation) {
    case COPY:
        nemo_file_operations_copy (sources, NULL, destination, NULL, copied, NULL);
        break;
    case COPY_FILE:
    {
        g_autofree char *name = g_path_get_basename (f.dest);
        nemo_file_operations_copy_file (source, destination, NULL,
                                         name, NULL, copied, NULL);
        break;
    }
    case DUPLICATE:
        nemo_file_operations_duplicate (sources, NULL, NULL, copied, NULL);
        break;
    case MOVE:
        nemo_file_operations_move (sources, NULL, destination, NULL, copied, NULL);
        break;
    }
    enqueue_swap ();
    wait_for_operation ();
    f.active = FALSE;
    g_assert_true (f.finished);
    g_assert_cmpuint (f.finished_count, ==, 1);
    g_assert_cmpint (f.success, ==, f.result.outcome == NEMO_PROGRESS_OUTCOME_SUCCESS);
    assert_no_fixture_directory_handles ();
}

static void
undo_done (GObject *object, GAsyncResult *result, gpointer unused)
{
    gboolean cancelled = FALSE;
    g_autoptr (GError) error = NULL;
    f.callbacks++;
    f.success = nemo_file_undo_info_apply_finish (NEMO_FILE_UNDO_INFO (object), result,
                                                &cancelled, &error);
    if (error)
        g_test_message ("Undo/redo error: %s", error->message);
    f.done = TRUE;
    gtk_main_quit ();
}

static void
run_undo (NemoFileUndoInfo *info, gboolean undo)
{
    f.done = FALSE;
    f.callbacks = 0;
    f.active = f.undoing = TRUE;
    nemo_file_undo_info_apply_async (info, undo, NULL, undo_done, NULL);
    wait_for_operation ();
    f.active = f.undoing = FALSE;
    assert_no_fixture_directory_handles ();
}

static void
manager_changed (NemoFileUndoManager *manager, gpointer unused)
{
    f.manager_events++;
    g_assert_cmpuint (f.manager_events, <=, 2);
    if (f.manager_events == 2) {
        f.callbacks++;
        f.done = TRUE;
        gtk_main_quit ();
    }
}

static void
run_manager_apply (NemoFileUndoInfo *info, gboolean undo)
{
    NemoFileUndoManager *manager = nemo_file_undo_manager_get ();
    g_assert_true (nemo_file_undo_manager_get_action () == info);
    g_assert_cmpint (nemo_file_undo_manager_get_state (), ==,
                    undo ? NEMO_FILE_UNDO_MANAGER_STATE_UNDO : NEMO_FILE_UNDO_MANAGER_STATE_REDO);
    f.done = FALSE;
    f.callbacks = f.manager_events = 0;
    g_clear_object (&f.undo_progress);
    f.active = f.undoing = TRUE;
    gulong handler = g_signal_connect (manager, "undo-changed", G_CALLBACK (manager_changed), NULL);
    if (undo)
        nemo_file_undo_manager_undo (NULL);
    else
        nemo_file_undo_manager_redo (NULL);
    wait_for_operation ();
    g_signal_handler_disconnect (manager, handler);
    f.active = f.undoing = FALSE;
    f.success = nemo_file_undo_manager_get_state () ==
        (undo ? NEMO_FILE_UNDO_MANAGER_STATE_REDO : NEMO_FILE_UNDO_MANAGER_STATE_UNDO);
    g_assert_cmpuint (f.manager_events, ==, 2);
    g_assert_nonnull (f.undo_progress);
    assert_no_fixture_directory_handles ();
}

static void
run_manager_undo (NemoFileUndoInfo *info)
{
    run_manager_apply (info, TRUE);
}

static void
assert_retryable_refusal (NemoFileUndoInfo *info, guint mutations_before)
{
    NemoProgressResult result;
    g_assert_false (f.success);
    g_assert_true (nemo_file_undo_info_can_retry (info));
    g_assert_false (nemo_file_undo_info_may_have_changed (info));
    g_assert_true (nemo_file_undo_manager_get_action () == info);
    g_assert_cmpint (nemo_file_undo_manager_get_state (), ==, NEMO_FILE_UNDO_MANAGER_STATE_UNDO);
    g_assert_cmpuint (f.mutations, ==, mutations_before);
    g_assert_cmpuint (f.undo_captures, ==, 0);
    g_assert_true (nemo_progress_info_get_result (f.undo_progress, &result));
    g_assert_cmpuint (result.completed_items, ==, 0);
    g_assert_cmpint (result.outcome, ==, NEMO_PROGRESS_OUTCOME_FAILED);
    g_autofree char *details = nemo_progress_info_get_completion_text (f.undo_progress);
    g_test_message ("Undo refusal: %s", details);
}

static void
setup_fixture (void)
{
    g_autofree char *cwd = g_get_current_dir ();
    f.root = g_build_filename (cwd, "transfer-safety-XXXXXX", NULL);
    g_assert_nonnull (g_mkdtemp (f.root));
    f.source_dir = scenario->crossfs ?
        g_build_filename (secondary_fs_root (), "nemo-transfer-XXXXXX", NULL) :
        g_build_filename (f.root, "source", NULL);
    if (named ("trash-source-copy")) {
        f.trash_root = g_build_filename (f.root, "controlled-trash", NULL);
        g_assert_cmpint (g_mkdir (f.trash_root, 0700), ==, 0);
        g_free (f.source_dir);
        f.source_dir = g_build_filename (f.trash_root, "files", NULL);
        g_autofree char *info = g_build_filename (f.trash_root, "info", NULL);
        g_assert_cmpint (g_mkdir (info, 0700), ==, 0);
        f.trash_original = g_build_filename (f.root, "original-location", "payload", NULL);
        g_autofree char *escaped = g_uri_escape_string (f.trash_original,
                                                       G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, FALSE);
        g_autofree char *metadata = g_strdup_printf (
            "[Trash Info]\nPath=%s\nDeletionDate=2026-01-01T00:00:00\n", escaped);
        g_autofree char *trashinfo = g_build_filename (info, "payload.trashinfo", NULL);
        write_contents (trashinfo, metadata);
        g_assert_cmpint (chmod (trashinfo, 0600), ==, 0);
    }
    if (scenario->crossfs)
        g_assert_nonnull (g_mkdtemp (f.source_dir));
    else
        g_assert_cmpint (g_mkdir (f.source_dir, 0700), ==, 0);
    g_autofree char *ancestor = g_build_filename (f.root, "ancestor", NULL);
    g_assert_cmpint (g_mkdir (ancestor, 0700), ==, 0);
    f.dest_dir = g_build_filename (ancestor, "destination", NULL);
    if (named ("real-tmpfs-destination")) {
        g_free (f.dest_dir);
        f.dest_dir = g_build_filename (secondary_fs_root (),
                                      "nemo-refused-destination-XXXXXX", NULL);
        g_assert_nonnull (g_mkdtemp (f.dest_dir));
    } else
        g_assert_cmpint (g_mkdir (f.dest_dir, 0700), ==, 0);
    f.source = g_build_filename (f.source_dir, "payload", NULL);
    f.dest = g_build_filename (f.dest_dir,
        scenario->directory && scenario->operation == COPY_FILE ? "custom-root" : "payload", NULL);
    f.saved = g_build_filename (scenario->crossfs ? f.source_dir : f.root, "saved", NULL);
    if (scenario->fault == SOURCE_PARENT_SWAP) {
        g_free (f.saved);
        f.saved = g_strconcat (f.source_dir, "-saved", NULL);
    }
    if (large_case ()) {
        create_large_fixture ();
    } else if (scenario->directory) {
        g_assert_cmpint (g_mkdir (f.source, 0700), ==, 0);
        g_autofree char *child = g_build_filename (f.source, "first", NULL);
        write_contents (child, payload);
        g_autofree char *nested = g_build_filename (f.source, "nested", NULL);
        g_assert_cmpint (g_mkdir (nested, 0700), ==, 0);
        g_autofree char *second = g_build_filename (nested, "second", NULL);
        write_contents (second, previous);
        if (scenario->replace) {
            g_assert_cmpint (g_mkdir (f.dest, 0700), ==, 0);
            g_autofree char *existing = g_build_filename (f.dest, "existing", NULL);
            write_contents (existing, foreign);
            if (named ("undo-merged-directory") || named ("undo-merge-new-subdirectory")) {
                g_assert_cmpint (lstat (f.dest, &f.merged_root_identity), ==, 0);
                g_autofree char *dest_nested = g_build_filename (f.dest, "nested", NULL);
                g_assert_false (exists (dest_nested));
                if (named ("undo-merged-directory")) {
                    g_assert_cmpint (g_mkdir (dest_nested, 0700), ==, 0);
                    g_assert_cmpint (lstat (dest_nested, &f.merged_nested_identity), ==, 0);
                    g_autofree char *nested_existing = g_build_filename (dest_nested, "existing", NULL);
                    write_contents (nested_existing, foreign);
                }
            }
        }
    } else if (scenario->symlink) {
        g_assert_cmpint (symlink ("relative-link-target", f.source), ==, 0);
    } else
        write_contents (f.source, payload);
    if (scenario->replace && !scenario->directory)
        write_contents (f.dest, scenario->fault == MATCH_FSYNC ? payload : previous);
    if (scenario->fault == RECOVERY_RESERVED) {
        g_autofree char *container = recovery_container_path ();
        g_assert_cmpint (g_mkdir (container, 0700), ==, 0);
        g_autofree char *owner = g_build_filename (container, "owner", NULL);
        write_contents (owner, foreign);
        protect_entry (container);
    }
    if (mixed_undo ()) {
        f.extra_source = g_build_filename (f.source_dir, "folder", NULL);
        f.extra_dest = g_build_filename (f.dest_dir, "folder", NULL);
        g_assert_cmpint (g_mkdir (f.extra_source, 0700), ==, 0);
        g_autofree char *child = g_build_filename (f.extra_source, "child", NULL);
        write_contents (child, previous);
    }
    g_assert_cmpint (lstat (f.source, &f.source_identity), ==, 0);
    if (f.trash_root) {
        f.trash_snapshot = g_string_new (NULL);
        snapshot_namespace (f.trash_root, f.trash_snapshot);
    }
    if (exchange_backend_errno ())
        g_assert_cmpint (lstat (f.dest, &f.destination_identity), ==, 0);
    if (scenario->crossfs) {
        struct stat source, dest;
        g_assert_cmpint (stat (f.source_dir, &source), ==, 0);
        g_assert_cmpint (stat (f.dest_dir, &dest), ==, 0);
        g_assert_cmpuint (source.st_dev, !=, dest.st_dev);
    }
    f.main_thread = g_thread_self ();
    g_mutex_init (&f.lock);
    g_cond_init (&f.ready);
    f.writers = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
    f.anchors = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
    f.undo_targets = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    f.root_anchor_fd = -1;
    f.recovery_fd = -1;
    f.manager = nemo_progress_info_manager_new ();
    f.dialog_text = g_string_new (NULL);
    g_signal_connect (f.manager, "new-progress-info", G_CALLBACK (progress_created), NULL);
    g_assert_true (g_settings_set_boolean (nemo_preferences, "safe-cross-fs-copy", FALSE));
    g_assert_true (g_settings_set_boolean (nemo_preferences, "verify-file-copies", TRUE));
    g_test_message ("Owned fixture: root=%s source-root=%s destination-root=%s",
                    f.root, f.source_dir, f.dest_dir);
}

static void
assert_success (void)
{
    g_assert_true (f.success);
    g_assert_cmpuint (f.result.completed_items, >, 0);
    g_assert_cmpuint (f.result.failed_items, ==, 0);
    g_assert_cmpuint (f.result.skipped_items, ==, 0);
}

static void
assert_failure (void)
{
    g_assert_false (f.success);
    g_assert_cmpuint (f.result.completed_items, ==, 0);
    g_assert_cmpuint (f.result.checksum_verified_files, ==, 0);
    g_autofree char *text = nemo_progress_info_get_completion_text (f.progress);
    g_assert_nonnull (text);
    g_assert_nonnull (strstr (text, "From: "));
    g_assert_nonnull (strstr (text, "To: "));
}

static void
check_namespace_reuse (void)
{
    for (guint i = 0; i < 32; i++) {
        g_autofree char *name = g_strdup_printf ("repeat-%02u", i);
        g_autofree char *contents = g_strdup_printf ("Independent transfer %u\n", i);
        g_free (f.source);
        g_free (f.dest);
        f.source = g_build_filename (f.source_dir, name, NULL);
        f.dest = g_build_filename (f.dest_dir, name, NULL);
        write_contents (f.source, contents);
        f.publications = f.captures = f.deletes = 0;
        f.capture_calls = f.publication_calls = f.stage_creations = f.writes = 0;
        f.published_at = f.synced_at = 0;
        run_operation (scenario->operation, 1);
        assert_success ();
        assert_contents (f.dest, contents);
        g_assert_cmpuint (f.publications, ==, 1);
        gboolean native = scenario->operation == MOVE && !scenario->crossfs;
        g_assert_cmpuint (f.result.completed_regular_files, ==, native ? 0 : 1);
        g_assert_cmpuint (f.result.completed_items, ==, 1);
        g_assert_cmpuint (f.result.atomic_moves, ==, native ? 1 : 0);
        g_assert_cmpuint (f.result.checksum_verified_files, ==, native ? 0 : 1);
        if (scenario->operation == COPY)
            assert_contents (f.source, contents);
        else {
            g_assert_false (exists (f.source));
            g_assert_cmpuint (f.captures, ==, 1);
            g_assert_cmpuint (f.deletes, ==, scenario->crossfs ? 1 : 0);
        }
        if (native) {
            g_assert_cmpuint (f.stage_creations, ==, 0);
            g_assert_cmpuint (f.writes, ==, 0);
        }
        g_assert_cmpuint (count_root_recovery_directories (f.dest_dir), ==, 1);
        g_assert_cmpuint (count_root_recovery_directories (f.source_dir), <=, 1);
        assert_completed_recovery_namespace (f.dest_dir);
        assert_completed_recovery_namespace (f.source_dir);
    }
}

static void
check_undo_preflight_namespace (void)
{
    g_assert_nonnull (f.preflight_record);
    NemoFileUndoInfo *info = g_object_ref (nemo_file_undo_manager_get_action ());
    for (guint i = 0; i < 3; i++) {
        GString *before = g_string_new (NULL);
        snapshot_namespace (f.root, before);
        g_autoptr (GError) error = NULL;
        g_autoptr (GCancellable) cancel = g_cancellable_new ();
        guint mutations = f.mutations;
        f.active = TRUE;
        g_assert_true (nemo_transfer_undo_check (f.preflight_record, FALSE, cancel, &error));
        g_assert_no_error (error);
        g_assert_true (nemo_transfer_undo_release (f.preflight_record, &error));
        g_assert_no_error (error);
        f.active = FALSE;
        g_assert_cmpuint (f.mutations, ==, mutations);
        GString *after = g_string_new (NULL);
        snapshot_namespace (f.root, after);
        g_assert_cmpstr (before->str, ==, after->str);
        g_string_free (before, TRUE);
        g_string_free (after, TRUE);
        assert_no_fixture_directory_handles ();
        g_assert_true (nemo_file_undo_manager_get_action () == info);
    }
    run_manager_undo (info);
    g_assert_true (f.success);
    g_assert_false (exists (f.dest));
    assert_contents (f.source, payload);
    if (scenario->operation == MOVE) {
        struct stat restored;
        g_assert_cmpint (lstat (f.source, &restored), ==, 0);
        g_assert_cmpuint (restored.st_dev, ==, f.source_identity.st_dev);
        g_assert_cmpuint (restored.st_ino, ==, f.source_identity.st_ino);
        assert_completed_recovery_namespace (f.dest_dir);
        assert_completed_recovery_namespace (f.source_dir);
    }
    run_manager_apply (info, FALSE);
    g_assert_true (f.success);
    assert_contents (f.dest, payload);
    if (scenario->operation == MOVE) {
        struct stat installed;
        g_assert_false (exists (f.source));
        g_assert_cmpint (lstat (f.dest, &installed), ==, 0);
        g_assert_cmpuint (installed.st_dev, ==, f.source_identity.st_dev);
        g_assert_cmpuint (installed.st_ino, ==, f.source_identity.st_ino);
        assert_completed_recovery_namespace (f.dest_dir);
        assert_completed_recovery_namespace (f.source_dir);
    }
    g_object_unref (info);
}

static void
test_transfer (void)
{
    setup_fixture ();
    gboolean explicit_true = g_str_has_prefix (scenario->name, "explicit-true-");
    gboolean preferences = g_str_has_prefix (scenario->name, "default-") || explicit_true;
    gboolean undo = g_str_has_prefix (scenario->name, "undo-") ||
                    g_str_has_prefix (scenario->name, "redo-") ||
                    named ("history-releases-anchors");
    int override = named ("mandatory-stage") || preferences ? 0 : 1;
    if (explicit_true) {
        g_settings_set_boolean (nemo_preferences, "verify-file-copies", FALSE);
        override = 1;
    }
    if (named ("copied-source-reclaimed")) {
        g_settings_set_boolean (nemo_preferences, "verify-file-copies", FALSE);
        override = 0;
    }
    struct rlimit original_limit;
    if (low_fd_case ())
        lower_fd_limit (&original_limit);
    run_operation (scenario->operation, override);
    if (low_fd_case ())
        g_assert_cmpint (setrlimit (RLIMIT_NOFILE, &original_limit), ==, 0);
    if (large_case ())
        check_large_contents ();
    gboolean should_succeed = (scenario->fault == FAULT_NONE &&
                               !named ("real-tmpfs-destination")) || scenario->fault == RENAME_EINTR ||
        (scenario->fault == REMOTE_SOURCE && scenario->operation == COPY) ||
        scenario->fault == UNDO_TARGET_RACE || scenario->fault == UNDO_RESTORE_RACE ||
        scenario->fault == UNDO_FSYNC || scenario->fault == UNDO_POSTCAPTURE_FSYNC ||
        scenario->fault == UNDO_CANCEL_PREFLIGHT || scenario->fault == UNDO_CANCEL_CAPTURE ||
        scenario->fault == RECOVERY_READABLE || scenario->fault == RECOVERY_UNKNOWN ||
        retained_recovery_entry ();
    if (should_succeed)
        assert_success ();
    else if (scenario->fault == FINAL_ANCHOR_CLOSE) {
        g_assert_true (f.anchor_closed);
        g_assert_cmpuint (f.injections, ==, 1);
        g_assert_false (f.success);
        g_assert_cmpint (f.result.outcome, !=, NEMO_PROGRESS_OUTCOME_SUCCESS);
    } else
        assert_failure ();
    if (scenario->fault != FAULT_NONE && !undo)
        g_assert_cmpuint (f.injections, >, 0);
    if (checked_no_backup_move ()) {
        assert_completed_recovery_namespace (f.dest_dir);
        assert_completed_recovery_namespace (f.source_dir);
    }
    if (large_case ()) {
        if (scenario->fault == LARGE_WRITE_SYNC) {
            g_assert_cmpuint (f.injections, ==, 1);
            g_assert_cmpuint (f.large_publications, ==, 0);
            g_assert_true (exists (f.source));
        } else {
            g_assert_cmpuint (f.large_publications, ==, f.large_files->len);
            g_assert_cmpuint (f.result.completed_regular_files, ==, f.large_files->len);
            g_assert_cmpuint (f.result.atomic_moves, ==, 0);
            g_assert_cmpuint (f.result.checksum_verified_files, ==, f.large_files->len);
            guint recoveries = count_root_recovery_directories (f.dest_dir);
            g_test_message ("Destination ROOT recovery directories after %u files: %u",
                            f.large_files->len, recoveries);
            g_assert_cmpuint (recoveries, <=, scenario->fault == RECOVERY_UNKNOWN ? 3 : 1);
            if (scenario->operation == MOVE) {
                g_assert_false (exists (f.source));
                g_assert_cmpuint (count_root_recovery_directories (f.source_dir), <=, 1);
            }
            if (scenario->fault == FAULT_NONE) {
                assert_completed_recovery_namespace (f.dest_dir);
                assert_completed_recovery_namespace (f.source_dir);
            }
        }
    }

    if (named ("mandatory-stage") || preferences) {
        g_assert_cmpint (f.result.verification_requested, ==, override == 1);
        if (scenario->operation != MOVE) {
            g_assert_cmpuint (f.writer_flushes, >, 0);
            g_assert_cmpuint (f.destination_flushes, >, 0);
            g_assert_cmpuint (f.result.checksum_verified_files, ==, override == 1 ? 1 : 0);
        }
        if (preferences) {
            /* The first constructor must consume its explicit override, even
             * a move constructor that does not copy file payloads. */
            g_free (f.source);
            f.source = g_build_filename (f.source_dir, "next", NULL);
            write_contents (f.source, payload);
            g_free (f.dest);
            f.dest = g_build_filename (f.dest_dir, "next", NULL);
            run_operation (scenario->operation, -1);
            assert_success ();
            g_assert_cmpint (f.result.verification_requested, ==, !explicit_true);
            g_assert_cmpuint (f.result.checksum_verified_files, ==,
                             explicit_true || scenario->operation == MOVE ? 0 : 1);
        }
    }

    if (scenario->operation == MOVE && !scenario->crossfs && should_succeed && !preferences) {
        struct stat installed;
        g_assert_cmpint (lstat (f.dest, &installed), ==, 0);
        g_assert_cmpuint (installed.st_dev, ==, f.source_identity.st_dev);
        g_assert_cmpuint (installed.st_ino, ==, f.source_identity.st_ino);
        g_assert_cmpuint (f.result.atomic_moves, ==, mixed_undo () ? 2 : 1);
        g_assert_cmpuint (f.result.checksum_verified_files, ==, 0);
        g_assert_cmpuint (f.writes, ==, 0);
        g_assert_cmpuint (f.publications, ==, 1);
        g_assert_cmpuint (f.captures, ==, 1);
        g_assert_false (exists (f.source));
        if (!scenario->symlink && !scenario->directory)
            g_assert_cmpuint (f.source_flushes, >, 0);
        if (scenario->directory)
            g_assert_cmpuint (f.tree_flushes, >=, 2);
    }
    if (named ("copied-source-reclaimed")) {
        g_assert_false (exists (f.source));
        g_assert_cmpuint (f.captures, ==, 1);
        g_assert_cmpuint (f.deletes, ==, 1);
        g_assert_cmpuint (f.result.atomic_moves, ==, 0);
        g_assert_cmpuint (f.result.checksum_verified_files, ==, 1);
        assert_contents (f.dest, payload);
        g_autofree char *retained = find_contents (f.source_dir, payload, FALSE);
        g_assert_null (retained);
    }
    if (scenario->fault == CAPTURE_RESULT_EIO) {
        g_assert_cmpuint (f.capture_calls, ==, 1);
        g_assert_cmpuint (f.captures, ==, 1);
        g_assert_cmpuint (f.deletes, ==, 0);
        g_autofree char *details = nemo_progress_info_get_completion_text (f.progress);
        if (exists (f.source))
            assert_contents (f.source, payload);
        else {
            g_assert_nonnull (f.capture);
            assert_contents (f.capture, payload);
            g_test_message ("Capture-error completion: %s", details);
            g_assert_nonnull (strstr (details, f.capture));
        }
        if (scenario->crossfs) {
            assert_contents (f.dest, payload);
            g_assert_cmpuint (f.publications, ==, 1);
        } else {
            g_assert_cmpuint (f.writes, ==, 0);
            g_assert_cmpuint (f.publications, ==, 0);
            g_assert_false (exists (f.dest));
        }
    }
    if (scenario->fault == PUBLISH_RESULT_EIO || scenario->fault == PUBLISH_RESULT_EINTR) {
        g_assert_cmpuint (f.publication_calls, ==, 1);
        g_assert_cmpuint (f.publications, ==, 1);
        g_assert_cmpuint (f.deletes, ==, 0);
        assert_contents (f.dest, payload);
        if (scenario->operation == MOVE) {
            g_assert_false (exists (f.source));
            g_assert_cmpuint (f.writes, ==, 0);
        } else
            assert_contents (f.source, payload);
        g_autofree char *retained = NULL;
        if (scenario->replace) {
            retained = find_contents (f.dest_dir, previous, TRUE);
            g_assert_nonnull (retained);
        }
        g_autofree char *details = nemo_progress_info_get_completion_text (f.progress);
        g_test_message ("Publication-error completion: %s", details);
        g_assert_nonnull (strstr (details, f.dest));
        if (retained)
            g_assert_nonnull (strstr (details, retained));
    }
    if (scenario->fault == FINAL_ANCHOR_CLOSE) {
        assert_contents (f.dest, payload);
        if (scenario->operation == MOVE) {
            g_assert_false (exists (f.source));
            g_autofree char *details = nemo_progress_info_get_completion_text (f.progress);
            g_assert_nonnull (strstr (details, f.dest));
        } else
            assert_contents (f.source, payload);
    }
    if (named ("history-releases-anchors")) {
        g_assert_nonnull (nemo_file_undo_manager_get_action ());
        g_assert_true (f.anchor_closed);
        g_assert_cmpuint (g_hash_table_size (f.anchors), ==, 0);
    }
    if ((scenario->fault >= BACKEND_NFS && scenario->fault <= REMOTE_DEST) ||
        named ("real-tmpfs-destination")) {
        g_assert_cmpuint (f.mutations, ==, 0);
        g_assert_false (exists (f.dest));
        assert_contents (f.source, payload);
    }
    if (scenario->fault == REMOTE_SOURCE && scenario->operation == MOVE) {
        g_assert_cmpuint (f.mutations, ==, 0);
        g_assert_cmpuint (f.source_queries, ==, 0);
        assert_contents (f.source, payload);
    }
    if (named ("trash-source-copy")) {
        g_assert_cmpuint (f.trash_uris, >, 0);
        g_assert_cmpuint (f.trash_metadata, >, 0);
        g_assert_cmpuint (f.trash_records, ==, 1);
        g_assert_cmpuint (f.stage_creations, >, 0);
        g_assert_cmpuint (f.publications, ==, 1);
        g_assert_cmpuint (f.captures, ==, 0);
        g_assert_cmpuint (f.deletes, ==, 0);
        g_assert_cmpuint (f.result.completed_regular_files, ==, 1);
        g_assert_cmpuint (f.result.checksum_verified_files, ==, 1);
        assert_contents (f.dest, payload);
        assert_contents (f.source, payload);
    }
    if (scenario->fault == REMOVAL_INFLIGHT) {
        g_assert_cmpuint (f.mutations, ==, 0);
        assert_contents (f.source, payload);
        g_assert_false (exists (f.dest));
        g_autofree char *text = nemo_progress_info_get_completion_text (f.progress);
        g_assert_nonnull (strstr (text, "retained"));
    }
    if (scenario->fault == MATCH_FSYNC) {
        assert_contents (f.source, payload);
        assert_contents (f.dest, payload);
        g_assert_null (nemo_file_undo_manager_get_action ());
    }
    if (scenario->fault == RECOVERY_CLOSE) {
        g_assert_true (f.recovery_closed);
        g_assert_cmpuint (f.publications, ==, 1);
        g_assert_cmpuint (f.captures, ==, 0);
        g_assert_cmpuint (f.deletes, ==, 0);
        assert_contents (f.source, payload);
        assert_contents (f.dest, payload);
    }
    if (f.protected_path) {
        assert_protected_entry ();
        assert_contents (f.source, payload);
        if (changed_marker () || retained_recovery_entry ()) {
            g_assert_cmpuint (f.publications, ==, 1);
            assert_contents (f.dest, payload);
        } else {
            g_assert_cmpuint (f.publications, ==, 0);
            g_assert_cmpuint (f.stage_creations, ==, 0);
            g_assert_cmpuint (f.captures, ==, 0);
            g_assert_false (exists (f.dest));
        }
    }
    if (scenario->fault == RECORD_UID) {
        g_assert_cmpuint (f.record_uid_fd_reads, >, 0);
        g_assert_cmpuint (f.record_uid_named_reads, >, 0);
    }
    if (scenario->fault == LINK_SYNCFS) {
        g_assert_cmpuint (f.publications, ==, 0);
        g_assert_cmpuint (f.captures, ==, 0);
        g_assert_cmpuint (f.deletes, ==, 0);
        g_assert_false (exists (f.dest));
        g_autofree char *link = g_file_read_link (f.source, NULL);
        g_assert_cmpstr (link, ==, "relative-link-target");
    }
    if (scenario->fault == SOURCE_FSYNC || scenario->fault == TREE_FSYNC ||
        scenario->fault == WRITER_CLOSE || scenario->fault == DIRECTORY_CLOSE ||
        scenario->fault == RENAME_EIO) {
        g_assert_cmpuint (f.publications, ==, 0);
        g_assert_cmpuint (f.captures, ==, 0);
        g_assert_false (exists (f.dest));
        if (scenario->directory) {
            g_autofree char *first = g_build_filename (f.source, "first", NULL);
            g_autofree char *second = g_build_filename (f.source, "nested", "second", NULL);
            assert_contents (first, payload);
            assert_contents (second, previous);
        } else
            assert_contents (f.source, payload);
    }
    if (scenario->fault == NEW_FILE_RACE)
        assert_contents (f.dest, foreign);
    if (scenario->fault == NEW_DIRECTORY_RACE) {
        g_autofree char *child = g_build_filename (f.dest, "new-child", NULL);
        assert_contents (child, foreign);
    }
    if (scenario->fault == NEW_SYMLINK_RACE) {
        g_autofree char *link = g_file_read_link (f.dest, NULL);
        g_assert_cmpstr (link, ==, "foreign-link-text");
        g_autofree char *source_link = g_file_read_link (f.source, NULL);
        g_assert_cmpstr (source_link, ==, "relative-link-target");
    }
    if (scenario->fault == NEW_FILE_RACE || scenario->fault == NEW_DIRECTORY_RACE) {
        if (scenario->operation == MOVE) {
            const char *retained = exists (f.source) ? f.source : f.capture;
            g_assert_nonnull (retained);
            if (scenario->directory) {
                g_autofree char *first = g_build_filename (retained, "first", NULL);
                g_autofree char *second = g_build_filename (retained, "nested", "second", NULL);
                assert_contents (first, payload);
                assert_contents (second, previous);
            } else
                assert_contents (retained, payload);
        } else
            assert_contents (f.source, payload);
    }
    if (scenario->fault == EXCHANGE_RACE) {
        assert_contents (f.saved, previous);
        assert_contents (f.source, payload);
        assert_contents (f.dest, payload);
        g_autofree char *retained = find_contents (f.dest_dir, foreign, TRUE);
        g_assert_nonnull (retained);
        g_autofree char *text = nemo_progress_info_get_completion_text (f.progress);
        g_autofree char *recovery = g_path_get_dirname (retained);
        g_assert_nonnull (strstr (text, recovery));
    }
    if (scenario->fault == EXCHANGE_UNSUPPORTED || scenario->fault == EXCHANGE_EINVAL ||
        scenario->fault == EXCHANGE_EOPNOTSUPP || exchange_backend_errno ()) {
        assert_contents (f.source, payload);
        assert_contents (f.dest, previous);
        g_assert_cmpuint (f.publications, ==, 0);
        if (scenario->fault != EXCHANGE_UNSUPPORTED) {
            g_autofree char *text = g_utf8_strdown (f.dialog_text->str, -1);
            g_test_message ("Unsupported replacement message: %s", f.dialog_text->str);
            g_assert_nonnull (strstr (text, "support"));
            g_assert_true (strstr (text, "another name") || strstr (text, "different name"));
            g_assert_null (strstr (text, "invalid argument"));
        }
        if (exchange_backend_errno ()) {
            g_assert_cmpuint (f.exchange_attempts, ==, 1);
            g_assert_cmpuint (f.exchange_probes, ==, 1);
            g_assert_cmpuint (f.injections, ==, 1);
            g_assert_cmpuint (f.stage_creations, ==, 0);
            g_assert_cmpuint (f.writes, ==, 0);
            g_assert_cmpuint (f.capture_calls, ==, 0);
            g_assert_cmpuint (f.captures, ==, 0);
            g_assert_cmpuint (f.deletes, ==, 0);
            g_assert_cmpuint (f.publication_calls, ==, 0);
            g_assert_cmpuint (f.result.completed_regular_files, ==, 0);
            g_assert_cmpuint (f.result.atomic_moves, ==, 0);
            struct stat source, destination;
            g_assert_cmpint (lstat (f.source, &source), ==, 0);
            g_assert_cmpint (lstat (f.dest, &destination), ==, 0);
            g_assert_cmpuint (source.st_dev, ==, f.source_identity.st_dev);
            g_assert_cmpuint (source.st_ino, ==, f.source_identity.st_ino);
            g_assert_cmpuint (destination.st_dev, ==, f.destination_identity.st_dev);
            g_assert_cmpuint (destination.st_ino, ==, f.destination_identity.st_ino);
        } else if (scenario->fault != EXCHANGE_UNSUPPORTED) {
            g_assert_cmpuint (f.exchange_probes, >, 0);
            g_assert_cmpuint (f.stage_creations, >, 0);
            g_assert_cmpuint (f.publication_calls, ==, 1);
        }
    }
    if (scenario->fault == CAPTURE_SWAP) {
        assert_contents (f.saved, payload);
        g_autofree char *retained = find_contents (f.source_dir, foreign, FALSE);
        g_assert_nonnull (retained);
    }
    if (scenario->fault == CAPTURE_CHANGED_OCCUPIED) {
        assert_contents (f.source, "New occupant of the original source name.\n");
        g_assert_nonnull (f.capture);
        assert_contents (f.capture, foreign);
        g_autofree char *text = nemo_progress_info_get_completion_text (f.progress);
        g_test_message ("Completion: %s", text);
        g_assert_nonnull (strstr (text, f.capture));
    }
    if (scenario->fault == DELETE_FAILURE || scenario->fault == CAPTURE_FAILURE ||
        scenario->fault == CAPTURE_CANCEL) {
        assert_contents (f.dest, payload);
        g_autofree char *retained = find_contents (f.source_dir, payload, FALSE);
        g_assert_nonnull (retained);
        if (!exists (f.source)) {
            g_autofree char *text = nemo_progress_info_get_completion_text (f.progress);
            g_assert_nonnull (strstr (text, retained));
        }
        if (scenario->fault == PROMPT_TARGET_MOVE || scenario->fault == COPYING_TARGET_MOVE) {
            assert_contents (f.source, payload);
            assert_contents (f.dest, foreign);
            g_assert_cmpuint (f.publications, ==, 0);
        }
        if (scenario->fault == DELETE_DIR_FSYNC) {
            g_assert_false (exists (f.source));
            g_assert_false (exists (f.capture));
            assert_contents (f.dest, payload);
            g_assert_cmpuint (f.deletes, ==, 1);
            g_autofree char *text = nemo_progress_info_get_completion_text (f.progress);
            g_assert_nonnull (strstr (text, "source was removed"));
        }
    }
    if (scenario->fault == PUBLISH_FSYNC) {
        g_assert_cmpuint (f.publications, ==, 1);
        g_assert_cmpint (exists (f.source), ==, scenario->operation != MOVE);
        assert_contents (f.dest, payload);
        g_autofree char *text = nemo_progress_info_get_completion_text (f.progress);
        g_test_message ("Completion: %s", text);
        if (scenario->operation == MOVE)
            g_assert_nonnull (strstr (text, f.dest));
    }
    if (scenario->fault == RECOVERY_FSYNC) {
        g_assert_cmpuint (f.publications, ==, 0);
        g_assert_cmpuint (f.writes, ==, 0);
        g_assert_cmpuint (f.writer_flushes, ==, 0);
        assert_contents (f.source, payload);
        g_assert_false (exists (f.dest));
    }
    if (queue_fault ()) {
        g_assert_cmpuint (f.publications, ==, 0);
        g_assert_false (exists (f.dest));
        if (scenario->fault == SOURCE_PARENT_SWAP)
            assert_contents (f.source, foreign);
        else
            assert_contents (f.source, payload);
    }
    if (named ("custom-name-tree")) {
        g_autofree char *first = g_build_filename (f.dest, "first", NULL);
        g_autofree char *second = g_build_filename (f.dest, "nested", "second", NULL);
        assert_contents (first, payload);
        assert_contents (second, previous);
        g_autofree char *wrong = g_build_filename (f.dest, "custom-root", NULL);
        g_assert_false (exists (wrong));
    }

    g_autofree char *backup = scenario->replace && !scenario->directory && should_succeed ?
        find_contents (f.dest_dir, previous, TRUE) : NULL;
    g_autofree char *bulk_backup = scenario->fault == RECOVERY_UNKNOWN ?
        find_contents (f.dest_dir, previous, TRUE) : NULL;
    if (scenario->fault == RECOVERY_UNKNOWN) {
        g_assert_nonnull (bulk_backup);
        g_assert_nonnull (f.unknown_recovery_entry);
        assert_contents (bulk_backup, previous);
        assert_contents (f.unknown_recovery_entry, foreign);
    }
    if (scenario->replace && !scenario->directory && should_succeed) {
        g_assert_nonnull (backup);
        g_assert_cmpuint (f.exchanges, ==, 1);
        g_autofree char *text = nemo_progress_info_get_completion_text (f.progress);
        g_autofree char *recovery = g_path_get_dirname (backup);
        struct stat recovery_stat;
        g_assert_cmpint (lstat (recovery, &recovery_stat), ==, 0);
        g_assert_true (S_ISDIR (recovery_stat.st_mode));
        g_assert_cmpuint (recovery_stat.st_mode & 0700, ==, 0700);
        g_assert_cmpuint (recovery_stat.st_mode & 0022, ==, 0);
        g_assert_cmpuint (recovery_stat.st_uid, ==, getuid ());
        g_assert_nonnull (strstr (text, recovery));
        if (named ("recovery-entry-details"))
            g_assert_nonnull (strstr (text, backup));
        if (scenario->fault == RECOVERY_READABLE) {
            g_assert_cmpuint (recovery_stat.st_mode & 0777, ==, 0755);
            g_assert_nonnull (strstr (text, "Other accounts may be able to read"));
            g_assert_nonnull (strstr (text, backup));
        }
    }
    if (undo) {
        g_autofree char *retained_after_finalization = NULL;
        NemoFileUndoInfo *info = nemo_file_undo_manager_get_action ();
        g_assert_nonnull (info);
        if (named ("undo-no-record-refused")) {
            g_autoptr (GFile) source_dir = g_file_new_for_path (f.source_dir);
            g_autoptr (GFile) destination_dir = g_file_new_for_path (f.dest_dir);
            g_autoptr (GFile) source = g_file_new_for_path (f.source);
            g_autoptr (GFile) destination = g_file_new_for_path (f.dest);
            info = nemo_file_undo_info_ext_new (NEMO_FILE_UNDO_OP_COPY, 1, source_dir, destination_dir);
            nemo_file_undo_info_ext_add_origin_target_pair (NEMO_FILE_UNDO_INFO_EXT (info),
                                                          source, destination);
            nemo_file_undo_manager_set_action (info);
            g_object_unref (info);
            info = nemo_file_undo_manager_get_action ();
        }
        g_object_ref (info);
        if (named ("undo-merged-directory") || named ("undo-merge-new-subdirectory")) {
            g_autofree char *nested = g_build_filename (f.dest, "nested", NULL);
            g_autofree char *first = g_build_filename (f.dest, "first", NULL);
            g_autofree char *second = g_build_filename (nested, "second", NULL);
            gboolean existing_only = named ("undo-merged-directory");
            g_assert_cmpuint (f.safe_undo_records, ==, 2);
            g_assert_cmpuint (f.undo_entries, ==, existing_only ? 2 : 3);
            g_assert_cmpuint (g_hash_table_size (f.undo_targets), ==, f.undo_entries);
            g_assert_false (g_hash_table_contains (f.undo_targets, f.dest));
            g_assert_cmpint (g_hash_table_contains (f.undo_targets, nested), ==, !existing_only);
            g_assert_true (g_hash_table_contains (f.undo_targets, first));
            g_assert_true (g_hash_table_contains (f.undo_targets, second));
            g_test_message ("Undo records: %u safe records for %u destinations; nested directory %s",
                            f.safe_undo_records, f.undo_entries, existing_only ? "pre-existed" : "created");
        }
        guint mutations_before = f.mutations;
        if (named ("undo-newer-target"))
            write_contents (f.dest, foreign);
        if (named ("undo-reopened-root-swap")) {
            g_assert_cmpint (rename (f.dest_dir, f.saved), ==, 0);
            g_assert_cmpint (g_mkdir (f.dest_dir, 0700), ==, 0);
            g_autofree char *installed = g_build_filename (f.saved, "payload", NULL);
            g_assert_cmpint (rename (installed, f.dest), ==, 0);
        }
        if (named ("undo-newer-source"))
            write_contents (f.source, foreign);
        if (named ("undo-new-child")) {
            g_autofree char *child = g_build_filename (f.dest, "new-child", NULL);
            write_contents (child, foreign);
        }
        gboolean blocked = (scenario->directory && !named ("undo-merged-directory")) ||
                           mixed_undo () || named ("undo-crossfs-refused") ||
                           named ("undo-no-record-refused");
        gboolean use_manager = blocked || named ("undo-newer-target") ||
                               named ("undo-merged-directory") ||
                               named ("undo-reopened-root-swap") ||
                               scenario->fault == UNDO_POSTCAPTURE_FSYNC ||
                               scenario->fault == UNDO_TARGET_RACE ||
                               scenario->fault == UNDO_RESTORE_RACE ||
                               scenario->fault == UNDO_FSYNC ||
                               scenario->fault == UNDO_CANCEL_PREFLIGHT ||
                               scenario->fault == UNDO_CANCEL_CAPTURE;
        if (named ("undo-retry-then-postcapture")) {
            g_assert_cmpint (rename (f.dest, f.saved), ==, 0);
            write_contents (f.dest, foreign);
            run_manager_undo (info);
            assert_retryable_refusal (info, mutations_before);
            assert_contents (f.dest, foreign);
            g_assert_cmpint (g_unlink (f.dest), ==, 0);
            g_assert_cmpint (rename (f.saved, f.dest), ==, 0);
        }
        if (use_manager)
            run_manager_undo (info);
        else
            run_undo (info, TRUE);
        if (named ("undo-replacement") || named ("history-releases-anchors")) {
            g_assert_true (f.success);
            assert_contents (f.dest, previous);
            if (named ("history-releases-anchors"))
                g_assert_cmpuint (g_hash_table_size (f.anchors), ==, 0);
            run_undo (info, FALSE);
            g_assert_true (f.success);
            assert_contents (f.dest, payload);
            if (named ("history-releases-anchors"))
                g_assert_cmpuint (g_hash_table_size (f.anchors), ==, 0);
        } else if (named ("undo-merged-directory")) {
            g_autofree char *nested = g_build_filename (f.dest, "nested", NULL);
            g_autofree char *first = g_build_filename (f.dest, "first", NULL);
            g_autofree char *second = g_build_filename (nested, "second", NULL);
            g_autofree char *existing = g_build_filename (f.dest, "existing", NULL);
            g_autofree char *nested_existing = g_build_filename (nested, "existing", NULL);
            NemoProgressResult result;
            struct stat root_after, nested_after;
            g_assert_true (f.success);
            g_assert_cmpuint (f.undo_captures, ==, 2);
            g_assert_true (nemo_progress_info_get_result (f.undo_progress, &result));
            g_assert_cmpuint (result.completed_items, ==, 2);
            g_assert_cmpint (result.outcome, ==, NEMO_PROGRESS_OUTCOME_SUCCESS);
            g_assert_false (exists (first));
            g_assert_false (exists (second));
            assert_contents (existing, foreign);
            assert_contents (nested_existing, foreign);
            g_assert_cmpint (lstat (f.dest, &root_after), ==, 0);
            g_assert_cmpint (lstat (nested, &nested_after), ==, 0);
            g_assert_cmpuint (root_after.st_ino, ==, f.merged_root_identity.st_ino);
            g_assert_cmpuint (nested_after.st_ino, ==, f.merged_nested_identity.st_ino);
            run_manager_apply (info, FALSE);
            g_assert_true (f.success);
            assert_contents (first, payload);
            assert_contents (second, previous);
            assert_contents (existing, foreign);
            assert_contents (nested_existing, foreign);
            g_assert_cmpint (lstat (f.dest, &root_after), ==, 0);
            g_assert_cmpint (lstat (nested, &nested_after), ==, 0);
            g_assert_cmpuint (root_after.st_ino, ==, f.merged_root_identity.st_ino);
            g_assert_cmpuint (nested_after.st_ino, ==, f.merged_nested_identity.st_ino);
        } else if (blocked) {
            assert_retryable_refusal (info, mutations_before);
            if (scenario->directory) {
                g_autofree char *first = g_build_filename (f.dest, "first", NULL);
                g_autofree char *second = g_build_filename (f.dest, "nested", "second", NULL);
                assert_contents (first, payload);
                assert_contents (second, previous);
                if (named ("undo-merge-new-subdirectory")) {
                    g_autofree char *existing = g_build_filename (f.dest, "existing", NULL);
                    assert_contents (existing, foreign);
                }
                if (named ("undo-new-child")) {
                    g_autofree char *child = g_build_filename (f.dest, "new-child", NULL);
                    assert_contents (child, foreign);
                }
            } else {
                assert_contents (f.dest, payload);
                if (mixed_undo ()) {
                    g_autofree char *child = g_build_filename (f.extra_dest, "child", NULL);
                    assert_contents (child, previous);
                    g_assert_false (exists (f.extra_source));
                }
            }
            if (scenario->operation == MOVE)
                g_assert_false (exists (f.source));
            g_autofree char *text = nemo_progress_info_get_completion_text (f.undo_progress);
            g_assert_true (strstr (text, "manual") || strstr (text, "Copy"));
            if (named ("undo-merge-new-subdirectory"))
                g_assert_nonnull (strstr (text, "no complete safe undo record"));
        } else if (scenario->fault == UNDO_POSTCAPTURE_FSYNC) {
            g_assert_false (f.success);
            g_assert_cmpuint (f.injections, >, 0);
            g_assert_cmpuint (f.undo_captures, ==, 1);
            g_assert_cmpuint (f.mutations, >, mutations_before);
            g_assert_false (nemo_file_undo_info_can_retry (info));
            g_assert_true (nemo_file_undo_info_may_have_changed (info));
            g_assert_null (nemo_file_undo_manager_get_action ());
            g_assert_cmpint (nemo_file_undo_manager_get_state (), ==, NEMO_FILE_UNDO_MANAGER_STATE_NONE);
            retained_after_finalization = find_contents (f.dest_dir, payload, TRUE);
            g_assert_nonnull (retained_after_finalization);
            assert_contents (f.source, payload);
        } else if (scenario->fault == UNDO_CANCEL_PREFLIGHT || scenario->fault == UNDO_CANCEL_CAPTURE) {
            NemoProgressResult result;
            g_assert_false (f.success);
            g_assert_cmpuint (f.injections, ==, 1);
            g_assert_true (nemo_progress_info_get_result (f.undo_progress, &result));
            g_assert_cmpint (result.outcome, ==, NEMO_PROGRESS_OUTCOME_CANCELLED);
            if (scenario->fault == UNDO_CANCEL_PREFLIGHT) {
                g_assert_false (nemo_file_undo_info_may_have_changed (info));
                g_assert_true (nemo_file_undo_manager_get_action () == info);
                g_assert_cmpint (nemo_file_undo_manager_get_state (), ==, NEMO_FILE_UNDO_MANAGER_STATE_UNDO);
                g_assert_cmpuint (f.mutations, ==, mutations_before);
                g_assert_cmpuint (f.undo_captures, ==, 0);
                assert_contents (f.dest, payload);
            } else {
                g_assert_true (nemo_file_undo_info_may_have_changed (info));
                g_assert_false (nemo_file_undo_info_can_retry (info));
                g_assert_null (nemo_file_undo_manager_get_action ());
                g_assert_cmpint (nemo_file_undo_manager_get_state (), ==, NEMO_FILE_UNDO_MANAGER_STATE_NONE);
                g_assert_cmpuint (f.undo_captures, ==, 1);
                retained_after_finalization = find_contents (f.dest_dir, payload, FALSE);
                g_assert_nonnull (retained_after_finalization);
            }
            assert_contents (f.source, payload);
        } else if (named ("undo-reopened-root-swap")) {
            assert_retryable_refusal (info, mutations_before);
            assert_contents (f.dest, payload);
            assert_contents (f.source, payload);
            g_assert_cmpuint (g_hash_table_size (f.anchors), ==, 0);
        } else if (named ("redo-newer-target")) {
            g_assert_true (f.success);
            write_contents (f.dest, foreign);
            run_undo (info, FALSE);
            g_assert_false (f.success);
            assert_contents (f.dest, foreign);
            g_autofree char *retained = find_contents (f.dest_dir, payload, TRUE);
            g_assert_nonnull (retained);
        } else {
            g_assert_false (f.success);
            if (named ("undo-newer-target"))
                assert_retryable_refusal (info, mutations_before);
            if (scenario->fault == UNDO_TARGET_RACE || scenario->fault == UNDO_RESTORE_RACE ||
                scenario->fault == UNDO_FSYNC) {
                g_assert_false (nemo_file_undo_info_can_retry (info));
                g_assert_true (nemo_file_undo_info_may_have_changed (info));
                g_assert_null (nemo_file_undo_manager_get_action ());
                g_assert_cmpint (nemo_file_undo_manager_get_state (), ==,
                                 NEMO_FILE_UNDO_MANAGER_STATE_NONE);
            }
            if (named ("undo-newer-source")) {
                assert_contents (f.source, foreign);
                assert_contents (f.dest, payload);
            } else if (named ("undo-new-child")) {
                g_autofree char *child = g_build_filename (f.dest, "new-child", NULL);
                assert_contents (child, foreign);
            } else if (scenario->fault != UNDO_FSYNC)
                assert_contents (f.dest, foreign);
            if (scenario->fault == UNDO_RESTORE_RACE) {
                assert_contents (backup, previous);
                retained_after_finalization = find_contents (f.dest_dir, payload, TRUE);
                g_assert_nonnull (retained_after_finalization);
            }
            if (scenario->fault == UNDO_FSYNC) {
                g_autofree char *retained = find_contents (f.dest_dir, payload, FALSE);
                g_assert_nonnull (retained);
                assert_contents (f.source, payload);
            }
            if (scenario->fault != FAULT_NONE)
                g_assert_cmpuint (f.injections, >, 0);
        }
        g_object_unref (info);
        if (retained_after_finalization)
            assert_contents (retained_after_finalization, payload);
        if (scenario->fault == UNDO_RESTORE_RACE)
            assert_contents (backup, previous);
    }
    if (g_str_has_prefix (scenario->name, "namespace-"))
        check_namespace_reuse ();
    if (named ("recovery-undo-check-no-mutation") || named ("native-undo-roundtrip"))
        check_undo_preflight_namespace ();
    g_clear_pointer (&f.preflight_record, nemo_transfer_undo_unref);
    gboolean bounded_namespace = (large_case () && scenario->fault == FAULT_NONE) ||
                                 g_str_has_prefix (scenario->name, "namespace-") ||
                                 checked_no_backup_move ();
    guint destination_entries = 0, source_entries = 0;
    if (bounded_namespace) {
        destination_entries = assert_completed_recovery_namespace (f.dest_dir);
        source_entries = assert_completed_recovery_namespace (f.source_dir);
    }
    nemo_file_undo_manager_set_action (NULL);
    if (bounded_namespace) {
        g_assert_cmpuint (assert_completed_recovery_namespace (f.dest_dir), ==, destination_entries);
        g_assert_cmpuint (assert_completed_recovery_namespace (f.source_dir), ==, source_entries);
    }
    if (f.trash_root) {
        GString *after = g_string_new (NULL);
        snapshot_namespace (f.trash_root, after);
        g_assert_cmpstr (after->str, ==, f.trash_snapshot->str);
        g_string_free (after, TRUE);
        g_string_free (f.trash_snapshot, TRUE);
    }
    if (f.protected_path)
        assert_protected_entry ();
    if (scenario->fault == RECOVERY_UNKNOWN) {
        assert_contents (bulk_backup, previous);
        assert_contents (f.unknown_recovery_entry, foreign);
    }
    if (named ("recovery-finalization") || named ("recovery-entry-details"))
        assert_contents (backup, previous);
    g_clear_object (&f.progress);
    g_clear_object (&f.undo_progress);
    g_object_unref (f.manager);
    g_hash_table_unref (f.writers);
    g_hash_table_unref (f.anchors);
    g_hash_table_unref (f.undo_targets);
    g_clear_pointer (&f.large_destinations, g_hash_table_unref);
    g_clear_pointer (&f.large_sources, g_hash_table_unref);
    g_clear_pointer (&f.large_captures, g_hash_table_unref);
    g_clear_pointer (&f.large_files, g_ptr_array_unref);
    g_string_free (f.dialog_text, TRUE);
    if (f.protected_snapshot)
        g_string_free (f.protected_snapshot, TRUE);
    if (scenario->crossfs)
        remove_fixture (f.source_dir);
    if (named ("real-tmpfs-destination"))
        remove_fixture (f.dest_dir);
    if (scenario->fault == SOURCE_PARENT_SWAP)
        remove_fixture (f.saved);
    remove_fixture (f.root);
    g_free (f.root);
    g_free (f.source_dir);
    g_free (f.dest_dir);
    g_free (f.source);
    g_free (f.dest);
    g_free (f.saved);
    g_free (f.capture);
    g_free (f.extra_source);
    g_free (f.extra_dest);
    g_free (f.undo_capture);
    g_free (f.unknown_recovery_entry);
    g_free (f.protected_path);
    g_free (f.retained_record);
    g_free (f.trash_root);
    g_free (f.trash_original);
    g_mutex_clear (&f.lock);
    g_cond_clear (&f.ready);
}

static void
expected_warning (const char *domain, GLogLevelFlags level, const char *message, gpointer data)
{
    if (g_str_has_prefix (message, "Could not inhibit power management:") &&
        strstr (message, "org.freedesktop.DBus.Error.NameHasNoOwner"))
        return;
    g_error ("Unexpected production warning: %s", message);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    g_assert_cmpint (argc, >=, 2);
    for (guint i = 0; i < G_N_ELEMENTS (scenarios); i++)
        if (g_strcmp0 (argv[1], scenarios[i].name) == 0)
            scenario = &scenarios[i];
    g_assert_nonnull (scenario);
    argv[1] = argv[0];
    argv++;
    argc--;
    g_test_init (&argc, &argv, NULL);
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    g_log_set_handler ("Nemo", G_LOG_LEVEL_WARNING, expected_warning, NULL);
    gtk_init (&argc, &argv);
    nemo_global_preferences_init ();
    nemo_file_undo_manager_get ();
    g_autofree char *path = g_strconcat ("/transfer-safety/", scenario->name, NULL);
    g_test_add_func (path, test_transfer);
    return g_test_run ();
}
