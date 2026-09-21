#define _GNU_SOURCE
#include <config.h>

#ifdef NEMO_SMPL
#include "nemo-transfer-safety.h"
#include "nemo-mount-operation.h"

#include <gio/gunixmounts.h>
#include <glib/gi18n.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <linux/stat.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef STATX_MNT_ID_UNIQUE
#define STATX_MNT_ID_UNIQUE 0x00004000U
#endif

typedef struct {
    GFile *file;
    char *path;
    int fd;
    struct statx identity;
    gboolean destination;
    gboolean root;
} TransferDirectory;

typedef struct {
    struct stat stat;
    char *checksum;
    char *link;
} TransferSnapshot;

typedef struct {
    TransferDirectory *parent;
    char *name;
    int fd;
    struct stat identity;
    GFile *file;
} TransferRecovery;

struct _NemoTransferGuard {
    gint refs;
    GPtrArray *directories;
    GHashTable *expected;
    GHashTable *record_parents;
    GHashTable *published_destinations;
    GError *error;
    GString *details;
    gboolean move;
    gboolean released;
    gboolean privacy_reported;
};

struct _NemoTransferTransaction {
    gint refs;
    NemoTransferGuard *guard;
    GFile *source;
    GFile *destination;
    GFile *source_file;
    GFile *destination_file;
    GFile *stage;
    TransferDirectory *source_parent;
    TransferDirectory *destination_parent;
    char *source_name;
    char *destination_name;
    TransferSnapshot source_before;
    TransferSnapshot installed;
    struct stat stage_identity;
    TransferRecovery recovery;
    gboolean stage_owned;
    gboolean published;
    gboolean backup;
    gboolean native_source;
    gboolean publication_uncertain;
    TransferSnapshot original;
};

struct _NemoTransferUndo {
    gint refs;
    NemoTransferTransaction *transaction;
    gboolean move;
    gboolean undone;
    guint serial;
};

static gboolean
transfer_error (GError **error, const char *operation)
{
    int saved = errno;
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved),
                 _("%s: %s"), operation, g_strerror (saved));
    return FALSE;
}

static gboolean
unsupported (GError **error)
{
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                         _("This location cannot provide the required atomic publication "
                           "and storage synchronization guarantees. "
                           "Use Copy to a supported local filesystem and review any "
                           "reported recovery locations."));
    return FALSE;
}

static gboolean
changed (GError **error)
{
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         _("A file, folder, or mount changed during the operation. "
                           "The operation was stopped to preserve the remaining data."));
    return FALSE;
}

static gboolean
sync_fd (int fd, GCancellable *cancel, GError **error)
{
    int result;
    do {
        if (g_cancellable_set_error_if_cancelled (cancel, error))
            return FALSE;
        result = fsync (fd);
    } while (result < 0 && errno == EINTR);
    return result == 0 || transfer_error (error, _("Could not synchronize storage"));
}

static gboolean
close_checked (int fd, GError **error)
{
    return close (fd) == 0 || transfer_error (error, _("Could not close the synchronized file"));
}

static gboolean
sync_filesystem_fd (int fd, GCancellable *cancel, GError **error)
{
    int result;
    do {
        if (g_cancellable_set_error_if_cancelled (cancel, error))
            return FALSE;
        result = syncfs (fd);
    } while (result < 0 && errno == EINTR);
    return result == 0 || transfer_error (error, _("Could not synchronize symbolic-link storage"));
}

static gboolean
check_directory_close (int fd, GError **error)
{
    /* Keep the pin alive for queued work/undo while checking close on a
     * reference to the same open file description after synchronization. */
    int reference = fcntl (fd, F_DUPFD_CLOEXEC, 3);
    if (reference < 0)
        return transfer_error (error, _("Could not retain the synchronized folder handle"));
    return close_checked (reference, error);
}

static gboolean
same_inode (const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           (a->st_mode & S_IFMT) == (b->st_mode & S_IFMT);
}

static gboolean
same_contents_metadata (const struct stat *a, const struct stat *b)
{
    return same_inode (a, b) && a->st_size == b->st_size &&
           a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_mode == b->st_mode && a->st_uid == b->st_uid && a->st_gid == b->st_gid;
}

static void
snapshot_clear (TransferSnapshot *snapshot)
{
    g_clear_pointer (&snapshot->checksum, g_free);
    g_clear_pointer (&snapshot->link, g_free);
    memset (&snapshot->stat, 0, sizeof snapshot->stat);
}

static void
snapshot_free (gpointer data)
{
    TransferSnapshot *snapshot = data;
    snapshot_clear (snapshot);
    g_free (snapshot);
}

static gboolean
supported_type (const char *type, gboolean destination)
{
    const char *persistent[] = { "ext2", "ext3", "ext4", "xfs", "btrfs",
                                 "f2fs", "vfat", "msdos", "exfat", "ntfs3" };
    for (guint i = 0; i < G_N_ELEMENTS (persistent); i++)
        if (g_strcmp0 (type, persistent[i]) == 0)
            return TRUE;
    return !destination && g_strcmp0 (type, "tmpfs") == 0;
}

/* Consult the kernel mount table before opening paths on potentially blocked
 * network/FUSE mounts. Never infer a local backend from GFile's native flag. */
static gboolean
supported_mount_path (const char *path, gboolean destination)
{
    GList *mounts = g_unix_mounts_get (NULL);
    const char *best_type = NULL;
    gsize best_length = 0;
    for (GList *l = mounts; l; l = l->next) {
        GUnixMountEntry *mount = l->data;
        const char *root = g_unix_mount_get_mount_path (mount);
        gsize length = strlen (root);
        if (length >= best_length && g_str_has_prefix (path, root) &&
            (length == 1 || path[length] == '/' || path[length] == '\0')) {
            best_length = length;
            best_type = g_unix_mount_get_fs_type (mount);
        }
    }
    gboolean ok = supported_type (best_type, destination);
    g_list_free_full (mounts, (GDestroyNotify) g_unix_mount_free);
    return ok;
}

static gboolean
supported_fd (int fd, gboolean destination, GError **error)
{
    struct statfs fs;
    if (fstatfs (fd, &fs) < 0)
        return transfer_error (error, _("Could not identify the filesystem"));
    switch ((unsigned long) fs.f_type) {
    case EXT4_SUPER_MAGIC:
    case XFS_SUPER_MAGIC:
    case BTRFS_SUPER_MAGIC:
    case F2FS_SUPER_MAGIC:
    case MSDOS_SUPER_MAGIC:
    case EXFAT_SUPER_MAGIC:
    case 0x7366746e: /* Kernel NTFS3; the mount-table check excludes ntfs-3g. */
        return TRUE;
    case TMPFS_MAGIC:
        if (!destination)
            return TRUE;
        break;
    }
    return unsupported (error);
}

static gboolean
directory_identity (int fd, struct statx *identity, GError **error)
{
    if (statx (fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
               STATX_BASIC_STATS | STATX_MNT_ID | STATX_MNT_ID_UNIQUE, identity) < 0)
        return transfer_error (error, _("Could not establish the mount identity"));
    if (!(identity->stx_mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE)) ||
        (identity->stx_mask & (STATX_INO | STATX_TYPE)) != (STATX_INO | STATX_TYPE) ||
        !S_ISDIR (identity->stx_mode))
        return unsupported (error);
    return TRUE;
}

static gboolean
same_directory (const struct statx *a, const struct statx *b)
{
    return a->stx_mnt_id == b->stx_mnt_id && a->stx_ino == b->stx_ino &&
           a->stx_dev_major == b->stx_dev_major && a->stx_dev_minor == b->stx_dev_minor &&
           S_ISDIR (b->stx_mode);
}

static int
open_root (const char *path, GError **error)
{
    int fd = open ("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        transfer_error (error, _("Could not open the filesystem root"));
        return -1;
    }
    g_auto (GStrv) parts = g_strsplit (path, "/", -1);
    for (guint i = 0; parts[i]; i++) {
        if (!parts[i][0])
            continue;
        int next = openat (fd, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int saved = errno;
        close (fd);
        fd = next;
        if (fd < 0) {
            errno = saved;
            transfer_error (error, _("Could not pin the folder without following symbolic links"));
            break;
        }
    }
    return fd;
}

static void
directory_free (gpointer data)
{
    TransferDirectory *directory = data;
    if (directory->fd >= 0)
        close (directory->fd);
    g_object_unref (directory->file);
    g_free (directory->path);
    g_free (directory);
}

static gboolean
directory_check (TransferDirectory *directory, GError **error)
{
    struct statx current;
    if (!supported_mount_path (directory->path, directory->destination))
        return changed (error);
    int fd = open_root (directory->path, error);
    if (fd < 0)
        return FALSE;
    gboolean ok = directory_identity (fd, &current, error);
    if (ok && !same_directory (&directory->identity, &current))
        ok = changed (error);
    if (ok && directory->fd < 0) {
        directory->fd = fd;
        return TRUE;
    }
    if (!close_checked (fd, ok ? error : NULL))
        ok = FALSE;
    return ok;
}

static TransferDirectory *
guard_directory (NemoTransferGuard *guard, GFile *file, gboolean destination,
                 gboolean initial, GError **error)
{
    if (!initial && destination) {
        gboolean within_destination = FALSE;
        for (guint i = 0; i < guard->directories->len; i++) {
            TransferDirectory *root = g_ptr_array_index (guard->directories, i);
            if (root->root && root->destination &&
                (g_file_equal (file, root->file) || g_file_has_prefix (file, root->file))) {
                within_destination = TRUE;
                break;
            }
        }
        if (!within_destination) {
            changed (error);
            return NULL;
        }
    }
    for (guint i = 0; i < guard->directories->len; i++) {
        TransferDirectory *directory = g_ptr_array_index (guard->directories, i);
        if (g_file_equal (directory->file, file)) {
            if (!directory_check (directory, error) ||
                (destination && !supported_fd (directory->fd, TRUE, error)))
                return NULL;
            directory->destination |= destination;
            directory->root |= initial;
            return directory;
        }
    }
    g_autofree char *path = g_file_get_path (file);
    if (!path || !g_file_is_native (file) || !supported_mount_path (path, destination)) {
        unsupported (error);
        return NULL;
    }
    TransferDirectory *ancestor = NULL;
    for (guint i = 0; i < guard->directories->len; i++) {
        TransferDirectory *candidate = g_ptr_array_index (guard->directories, i);
        if (g_file_has_prefix (file, candidate->file) &&
            (!ancestor || strlen (candidate->path) > strlen (ancestor->path)))
            ancestor = candidate;
    }
    if (!initial && !ancestor) {
        changed (error);
        return NULL;
    }
    int fd = -1;
    if (ancestor) {
        if (!directory_check (ancestor, error))
            return NULL;
        fd = fcntl (ancestor->fd, F_DUPFD_CLOEXEC, 3);
        g_autofree char *relative = g_file_get_relative_path (ancestor->file, file);
        g_auto (GStrv) parts = g_strsplit (relative, "/", -1);
        for (guint i = 0; fd >= 0 && parts[i]; i++) {
            int next = openat (fd, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            int saved = errno;
            close (fd);
            fd = next;
            errno = saved;
        }
        if (fd < 0) {
            transfer_error (error, _("Could not open the pinned descendant folder"));
            return NULL;
        }
    } else {
        fd = open_root (path, error);
    }
    if (fd < 0)
        return NULL;
    TransferDirectory *directory = g_new0 (TransferDirectory, 1);
    directory->fd = fd;
    directory->file = g_object_ref (file);
    directory->path = g_strdup (path);
    directory->destination = destination;
    directory->root = initial;
    if (!supported_fd (fd, destination, error) ||
        !directory_identity (fd, &directory->identity, error)) {
        directory_free (directory);
        return NULL;
    }
    if (ancestor && directory->identity.stx_mnt_id != ancestor->identity.stx_mnt_id) {
        directory_free (directory);
        changed (error);
        return NULL;
    }
    TransferSnapshot *expected = destination ? g_hash_table_lookup (guard->expected, file) : NULL;
    if (expected && S_ISDIR (expected->stat.st_mode)) {
        struct stat current;
        if (fstat (fd, &current) < 0 || !same_inode (&expected->stat, &current)) {
            directory_free (directory);
            changed (error);
            return NULL;
        }
    }
    g_ptr_array_add (guard->directories, directory);
    return directory;
}

NemoTransferGuard *
nemo_transfer_guard_new (GList *sources, GFile *destination, gboolean move)
{
    NemoTransferGuard *guard = g_new0 (NemoTransferGuard, 1);
    guard->refs = 1;
    guard->move = move;
    guard->directories = g_ptr_array_new_with_free_func (directory_free);
    guard->expected = g_hash_table_new_full (g_file_hash, (GEqualFunc) g_file_equal,
                                            g_object_unref, snapshot_free);
    guard->record_parents = g_hash_table_new_full (g_file_hash, (GEqualFunc) g_file_equal,
                                                 g_object_unref, NULL);
    guard->published_destinations = g_hash_table_new_full (g_file_hash, (GEqualFunc) g_file_equal,
                                                         g_object_unref, NULL);
    guard->details = g_string_new (NULL);
    if (nemo_mount_operation_is_removing ()) {
        g_set_error_literal (&guard->error, G_IO_ERROR, G_IO_ERROR_BUSY,
                             _("A device removal is in progress. The transfer was not started; "
                               "all source files were retained."));
        return guard;
    }
    if (destination && !guard_directory (guard, destination, TRUE, TRUE, &guard->error))
        return guard;
    for (GList *l = sources; l && !guard->error; l = l->next) {
        GFile *source = l->data;
        g_autoptr (GFile) parent = g_file_get_parent (source);
        g_autofree char *path = g_file_get_path (source);
        gboolean supported = path && g_file_is_native (source) &&
                             supported_mount_path (path, FALSE);
        if (move && !supported) {
            unsupported (&guard->error);
            break;
        }
        if (move || !destination || supported) {
            if (!parent)
                unsupported (&guard->error);
            else
                guard_directory (guard, parent, !destination, TRUE, &guard->error);
        }
    }
    return guard;
}

NemoTransferGuard *
nemo_transfer_guard_ref (NemoTransferGuard *guard)
{
    g_atomic_int_inc (&guard->refs);
    return guard;
}

void
nemo_transfer_guard_unref (NemoTransferGuard *guard)
{
    if (!guard || !g_atomic_int_dec_and_test (&guard->refs))
        return;
    g_ptr_array_unref (guard->directories);
    g_hash_table_unref (guard->expected);
    g_hash_table_unref (guard->record_parents);
    g_hash_table_unref (guard->published_destinations);
    g_clear_error (&guard->error);
    g_string_free (guard->details, TRUE);
    g_free (guard);
}

gboolean
nemo_transfer_guard_check (NemoTransferGuard *guard, GError **error)
{
    if (guard->error) {
        g_propagate_error (error, g_error_copy (guard->error));
        return FALSE;
    }
    guard->released = FALSE;
    if (nemo_mount_operation_is_removing ()) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                             _("A device removal is in progress. Remaining source files were retained."));
        return FALSE;
    }
    for (guint i = 0; i < guard->directories->len; i++) {
        TransferDirectory *directory = g_ptr_array_index (guard->directories, i);
        if (directory->root && !directory_check (directory, error))
            return FALSE;
    }
    return TRUE;
}

gboolean
nemo_transfer_guard_release (NemoTransferGuard *guard, GError **error)
{
    if (guard->released)
        return TRUE;
    gboolean ok = TRUE;
    for (guint i = 0; i < guard->directories->len; i++) {
        TransferDirectory *directory = g_ptr_array_index (guard->directories, i);
        if (directory->fd >= 0) {
            int fd = directory->fd;
            directory->fd = -1;
            if (!close_checked (fd, ok ? error : NULL))
                ok = FALSE;
        }
    }
    if (!ok) {
        g_string_append (guard->details,
                         _("\nTransfer folder handles could not all be closed successfully. "
                           "Already moved files may only exist at the destination or a recovery location."));
        GList *destinations = g_hash_table_get_keys (guard->published_destinations);
        for (GList *l = destinations; l; l = l->next)
            nemo_transfer_guard_describe_destination (guard, l->data);
        g_list_free (destinations);
    }
    guard->released = TRUE;
    return ok;
}

static GFile *
file_at (int fd, const char *name)
{
    g_autofree char *path = g_strdup_printf ("/proc/self/fd/%d/%s", fd, name);
    return g_file_new_for_path (path);
}

GFile *
nemo_transfer_guard_file (NemoTransferGuard *guard, GFile *file, gboolean destination,
                         GError **error)
{
    if (!nemo_transfer_guard_check (guard, error))
        return NULL;
    g_autoptr (GFile) parent = g_file_get_parent (file);
    g_autofree char *path = g_file_get_path (file);
    if (!destination && !guard->move &&
        (!path || !g_file_is_native (file) || !supported_mount_path (path, FALSE)))
        return g_object_ref (file);
    TransferDirectory *directory = parent ? guard_directory (guard, parent, destination, FALSE, error) : NULL;
    if (!directory) {
        if (!*error)
            unsupported (error);
        return NULL;
    }
    g_autofree char *name = g_file_get_basename (file);
    return file_at (directory->fd, name);
}

GFile *
nemo_transfer_guard_directory (NemoTransferGuard *guard, GFile *file, gboolean destination,
                               GError **error)
{
    if (!nemo_transfer_guard_check (guard, error))
        return NULL;
    g_autofree char *path = g_file_get_path (file);
    if (!destination && !guard->move &&
        (!path || !g_file_is_native (file) || !supported_mount_path (path, FALSE)))
        return g_object_ref (file);
    TransferDirectory *directory = guard_directory (guard, file, destination, FALSE, error);
    return directory ? file_at (directory->fd, ".") : NULL;
}

static TransferDirectory *
recovery_parent (NemoTransferGuard *guard, TransferDirectory *parent, gboolean destination)
{
    TransferDirectory *root = parent;
    for (guint i = 0; i < guard->directories->len; i++) {
        TransferDirectory *candidate = g_ptr_array_index (guard->directories, i);
        if (candidate->root && (!destination || candidate->destination) &&
            candidate->identity.stx_mnt_id == parent->identity.stx_mnt_id &&
            g_file_has_prefix (parent->file, candidate->file) &&
            strlen (candidate->path) < strlen (root->path))
            root = candidate;
    }
    return root;
}

char *
nemo_transfer_guard_take_details (NemoTransferGuard *guard)
{
    if (g_hash_table_size (guard->record_parents)) {
        GHashTableIter iter;
        gpointer parent;
        g_string_append (guard->details,
                         _("\nTransaction records are retained in .nemo-recovery-* folders under:"));
        g_hash_table_iter_init (&iter, guard->record_parents);
        while (g_hash_table_iter_next (&iter, &parent, NULL)) {
            g_autofree char *path = g_file_get_parse_name (parent);
            g_string_append_printf (guard->details, "\n%s", path);
        }
        g_string_append (guard->details,
                         _("\nSuccessful moves remove the captured source data; metadata-only "
                           "transaction records may remain. Do not remove folders containing "
                           "recovered originals without reviewing their contents."));
        g_hash_table_remove_all (guard->record_parents);
    }
    char *details = g_strdup (guard->details->str);
    g_string_truncate (guard->details, 0);
    return details;
}

void
nemo_transfer_guard_describe_destination (NemoTransferGuard *guard, GFile *destination)
{
    if (GPOINTER_TO_INT (g_hash_table_lookup (guard->published_destinations, destination)))
        return;
    g_hash_table_replace (guard->published_destinations, g_object_ref (destination), GINT_TO_POINTER (TRUE));
    g_autofree char *path = g_file_get_parse_name (destination);
    g_string_append_printf (guard->details, _("\nInspect destination entry: %s"), path);
}

static gboolean
snapshot_at (int parent, const char *name, TransferSnapshot *snapshot,
             gboolean hash, gboolean synchronize, GCancellable *cancel,
             GError **error)
{
    snapshot_clear (snapshot);
    if (fstatat (parent, name, &snapshot->stat, AT_SYMLINK_NOFOLLOW) < 0)
        return transfer_error (error, _("Could not inspect the pinned file"));
    if (S_ISLNK (snapshot->stat.st_mode)) {
        gsize length = MAX ((gsize) snapshot->stat.st_size + 1, (gsize) 4096);
        char *target = g_malloc (length + 1);
        ssize_t got = readlinkat (parent, name, target, length);
        if (got < 0 || (gsize) got == length) {
            g_free (target);
            if (got >= 0)
                errno = ENAMETOOLONG;
            return transfer_error (error, _("Could not read the symbolic link"));
        }
        target[got] = '\0';
        snapshot->link = target;
        /* A symlink cannot be opened for fsync without following it. Its
         * target bytes may live outside the directory's own mapping. */
        if (synchronize &&
            (!sync_filesystem_fd (parent, cancel, error) || !sync_fd (parent, cancel, error)))
            return FALSE;
        struct stat after;
        if (fstatat (parent, name, &after, AT_SYMLINK_NOFOLLOW) < 0 ||
            !same_contents_metadata (&snapshot->stat, &after) ||
            snapshot->stat.st_ctim.tv_sec != after.st_ctim.tv_sec ||
            snapshot->stat.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
            return changed (error);
        return TRUE;
    }
    if (!S_ISREG (snapshot->stat.st_mode) && !S_ISDIR (snapshot->stat.st_mode))
        return unsupported (error);
    int fd = openat (parent, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return transfer_error (error, _("Could not open the pinned file"));
    struct stat before, after, named;
    gboolean ok = fstat (fd, &before) == 0 && same_inode (&before, &snapshot->stat);
    if (!ok)
        changed (error);
    if (ok && synchronize)
        ok = sync_fd (fd, cancel, error);
    if (ok && hash && S_ISREG (before.st_mode)) {
        GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA256);
        guchar buffer[128 * 1024];
        guint64 count = 0;
        while (ok) {
            if (g_cancellable_set_error_if_cancelled (cancel, error)) {
                ok = FALSE;
                break;
            }
            ssize_t got = read (fd, buffer, sizeof buffer);
            if (got < 0 && errno == EINTR)
                continue;
            if (got < 0) {
                ok = transfer_error (error, _("Could not verify the pinned file"));
                break;
            }
            if (!got)
                break;
            count += got;
            g_checksum_update (checksum, buffer, got);
        }
        if (ok && count != (guint64) before.st_size)
            ok = changed (error);
        if (ok)
            snapshot->checksum = g_strdup (g_checksum_get_string (checksum));
        g_checksum_free (checksum);
    }
    if (ok && (fstat (fd, &after) < 0 ||
               fstatat (parent, name, &named, AT_SYMLINK_NOFOLLOW) < 0 ||
               !same_contents_metadata (&before, &after) ||
               before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
               before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
               !same_inode (&after, &named)))
        ok = changed (error);
    if (!close_checked (fd, ok ? error : NULL))
        ok = FALSE;
    return ok;
}

static gboolean
snapshot_matches (const TransferSnapshot *expected, const TransferSnapshot *current)
{
    return same_contents_metadata (&expected->stat, &current->stat) &&
           g_strcmp0 (expected->checksum, current->checksum) == 0 &&
           g_strcmp0 (expected->link, current->link) == 0;
}

gboolean
nemo_transfer_guard_expect (NemoTransferGuard *guard, GFile *destination, GError **error)
{
    if (g_hash_table_contains (guard->expected, destination))
        return TRUE;
    g_autoptr (GFile) parent = g_file_get_parent (destination);
    TransferDirectory *directory = guard_directory (guard, parent, TRUE, FALSE, error);
    if (!directory)
        return FALSE;
    g_autofree char *name = g_file_get_basename (destination);
    TransferSnapshot *snapshot = g_new0 (TransferSnapshot, 1);
    if (!snapshot_at (directory->fd, name, snapshot, TRUE, FALSE, NULL, error)) {
        snapshot_free (snapshot);
        return FALSE;
    }
    g_hash_table_insert (guard->expected, g_object_ref (destination), snapshot);
    return TRUE;
}

static char *
recovery_path (TransferRecovery *recovery)
{
    char proc[64], path[4096];
    g_snprintf (proc, sizeof proc, "/proc/self/fd/%d", recovery->fd);
    ssize_t count = readlink (proc, path, sizeof path - 1);
    if (count >= 0) {
        path[count] = '\0';
        return g_strdup (path);
    }
    return g_build_filename (recovery->parent->path, recovery->name, NULL);
}

static void
report_recovery (NemoTransferGuard *guard, TransferRecovery *recovery, const char *reason)
{
    g_autofree char *path = recovery_path (recovery);
    g_autoptr (GFile) file = g_file_new_for_path (path);
    g_autofree char *display = g_file_get_parse_name (file);
    g_string_append_printf (guard->details, _("\n%s\nRecovery files retained in: %s\n"
                                             "Recovery files continue to use storage space."),
                            reason, display);
}

static void
report_recovery_entry (NemoTransferGuard *guard, TransferRecovery *recovery,
                       const char *slot, const char *reason)
{
    report_recovery (guard, recovery, reason);
    g_autofree char *parent = recovery_path (recovery);
    g_autofree char *path = g_build_filename (parent, slot, NULL);
    g_autoptr (GFile) file = g_file_new_for_path (path);
    g_autofree char *display = g_file_get_parse_name (file);
    g_string_append_printf (guard->details, _("\nRetained entry: %s"), display);
}

static gboolean
recovery_new (TransferDirectory *parent, TransferRecovery *recovery,
              GCancellable *cancel, GError **error)
{
    memset (recovery, 0, sizeof *recovery);
    recovery->fd = -1;
    recovery->parent = parent;
    if (!directory_check (parent, error))
        return FALSE;
    for (guint i = 0; i < 16; i++) {
        g_autofree char *uuid = g_uuid_string_random ();
        g_free (recovery->name);
        recovery->name = g_strconcat (".nemo-recovery-", uuid, NULL);
        if (mkdirat (parent->fd, recovery->name, 0700) == 0)
            break;
        if (errno != EEXIST || i == 15)
            return transfer_error (error, _("Could not create private recovery storage"));
    }
    recovery->fd = openat (parent->fd, recovery->name,
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (recovery->fd < 0)
        return transfer_error (error, _("Could not pin private recovery storage"));
    if (fstat (recovery->fd, &recovery->identity) < 0)
        return transfer_error (error, _("Could not inspect private recovery storage"));
    if (recovery->identity.st_uid != geteuid () ||
        (recovery->identity.st_mode & (S_IWGRP | S_IWOTH))) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             _("This filesystem cannot isolate recovery entries from other writers."));
        return FALSE;
    }
    recovery->file = g_file_get_child (parent->file, recovery->name);
    return sync_fd (recovery->fd, cancel, error) && sync_fd (parent->fd, cancel, error);
}

static gboolean
write_all (int fd, const char *data, gsize length, GError **error)
{
    while (length) {
        ssize_t count = write (fd, data, length);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            if (!count)
                errno = EIO;
            return transfer_error (error, _("Could not write the recovery record"));
        }
        data += count;
        length -= count;
    }
    return TRUE;
}

static gboolean
recovery_record (NemoTransferTransaction *transaction, TransferRecovery *recovery,
                 const char *phase, GCancellable *cancel, GError **error)
{
    if ((recovery->identity.st_mode & (S_IRWXG | S_IRWXO)) &&
        !transaction->guard->privacy_reported) {
        g_string_append (transaction->guard->details,
                         _("\nRecovery directories are owner-controlled but not fully private on "
                           "this filesystem. Other accounts may be able to read their contents."));
        transaction->guard->privacy_reported = TRUE;
    }
    g_autoptr (GKeyFile) record = g_key_file_new ();
    g_autofree char *source = g_file_get_uri (transaction->source);
    g_autofree char *destination = g_file_get_uri (transaction->destination);
    /* URIs escape arbitrary filename bytes; base64 also makes the format
     * unambiguous independently of locale and key-file string escaping. */
    g_autofree char *source64 = g_base64_encode ((const guchar *) source, strlen (source));
    g_autofree char *destination64 = g_base64_encode ((const guchar *) destination, strlen (destination));
    g_key_file_set_integer (record, "Transfer", "version", 1);
    g_key_file_set_string (record, "Transfer", "source-uri-base64", source64);
    g_key_file_set_string (record, "Transfer", "destination-uri-base64", destination64);
    g_key_file_set_string (record, "Transfer", "phase", phase);
    g_key_file_set_uint64 (record, "Transfer", "directory-device", recovery->identity.st_dev);
    g_key_file_set_uint64 (record, "Transfer", "directory-inode", recovery->identity.st_ino);
    g_key_file_set_uint64 (record, "Transfer", "source-device", transaction->source_before.stat.st_dev);
    g_key_file_set_uint64 (record, "Transfer", "source-inode", transaction->source_before.stat.st_ino);
    g_key_file_set_uint64 (record, "Transfer", "installed-device", transaction->installed.stat.st_dev);
    g_key_file_set_uint64 (record, "Transfer", "installed-inode", transaction->installed.stat.st_ino);
    if (transaction->installed.checksum)
        g_key_file_set_string (record, "Transfer", "installed-sha256", transaction->installed.checksum);
    TransferSnapshot *approved = g_hash_table_lookup (transaction->guard->expected, transaction->destination);
    if (approved) {
        g_key_file_set_uint64 (record, "Transfer", "approved-device", approved->stat.st_dev);
        g_key_file_set_uint64 (record, "Transfer", "approved-inode", approved->stat.st_ino);
        if (approved->checksum)
            g_key_file_set_string (record, "Transfer", "approved-sha256", approved->checksum);
    }
    gsize length;
    g_autofree char *data = g_key_file_to_data (record, &length, NULL);
    g_autofree char *name = g_strconcat ("record-", phase, NULL);
    int fd = openat (recovery->fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return transfer_error (error, _("Could not create the recovery record"));
    g_hash_table_add (transaction->guard->record_parents, g_object_ref (recovery->parent->file));
    gboolean ok = write_all (fd, data, length, error) && sync_fd (fd, cancel, error);
    if (!close_checked (fd, ok ? error : NULL))
        ok = FALSE;
    return ok && sync_fd (recovery->fd, cancel, error) &&
           sync_fd (recovery->parent->fd, cancel, error);
}

static void
recovery_close (TransferRecovery *recovery)
{
    if (recovery->fd >= 0)
        close (recovery->fd);
    g_clear_object (&recovery->file);
    g_free (recovery->name);
}

static gboolean
recovery_finish (TransferRecovery *recovery, GError **error)
{
    if (recovery->fd < 0)
        return TRUE;
    int fd = recovery->fd;
    recovery->fd = -1;
    return close_checked (fd, error);
}

static gboolean
recovery_identity_check (TransferRecovery *recovery, GError **error)
{
    struct stat current, named;
    if (fstat (recovery->fd, &current) < 0 ||
        fstatat (recovery->parent->fd, recovery->name, &named, AT_SYMLINK_NOFOLLOW) < 0)
        return transfer_error (error, _("Could not confirm the recovery folder location"));
    if (!same_inode (&recovery->identity, &current) || !same_inode (&current, &named) ||
        current.st_uid != geteuid () || (current.st_mode & (S_IWGRP | S_IWOTH)))
        return changed (error);
    return TRUE;
}

gboolean
nemo_transfer_transaction_finish (NemoTransferTransaction *transaction, GError **error)
{
    if (transaction->recovery.fd < 0)
        return TRUE;
    if (!recovery_identity_check (&transaction->recovery, error)) {
        report_recovery (transaction->guard, &transaction->recovery,
                         _("The recovery folder changed location or permissions."));
        return FALSE;
    }
    if (recovery_finish (&transaction->recovery, error))
        return TRUE;
    report_recovery (transaction->guard, &transaction->recovery,
                     _("The published transaction's folder handle could not be closed successfully."));
    return FALSE;
}

static gboolean
recovery_reopen (TransferRecovery *recovery, GError **error)
{
    if (recovery->fd >= 0)
        return TRUE;
    if (!directory_check (recovery->parent, error))
        return FALSE;
    int fd = openat (recovery->parent->fd, recovery->name,
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return transfer_error (error, _("Could not reopen the retained recovery folder"));
    struct stat current;
    if (fstat (fd, &current) < 0 || !same_inode (&recovery->identity, &current) ||
        current.st_uid != geteuid () || (current.st_mode & (S_IWGRP | S_IWOTH))) {
        close (fd);
        return changed (error);
    }
    recovery->fd = fd;
    return TRUE;
}

static gboolean
rename_entry (int source, const char *source_name, int destination, const char *destination_name,
              unsigned int flags, GError **error)
{
    struct stat source_before, destination_before, source_after, destination_after;
    gboolean have_source = fstatat (source, source_name, &source_before, AT_SYMLINK_NOFOLLOW) == 0;
    gboolean have_destination = fstatat (destination, destination_name, &destination_before,
                                         AT_SYMLINK_NOFOLLOW) == 0;
    gboolean missing_destination = !have_destination && errno == ENOENT;
    for (guint attempt = 0; attempt < 16; attempt++) {
        if (renameat2 (source, source_name, destination, destination_name, flags) == 0)
            return TRUE;
        if (errno != EINTR)
            break;
        /* EXCHANGE must never be repeated blindly: an interrupted backend
         * could already have changed names. Retry only an unchanged namespace. */
        gboolean source_same = have_source &&
            fstatat (source, source_name, &source_after, AT_SYMLINK_NOFOLLOW) == 0 &&
            same_contents_metadata (&source_before, &source_after) &&
            source_before.st_ctim.tv_sec == source_after.st_ctim.tv_sec &&
            source_before.st_ctim.tv_nsec == source_after.st_ctim.tv_nsec;
        int destination_result = fstatat (destination, destination_name, &destination_after,
                                          AT_SYMLINK_NOFOLLOW);
        gboolean destination_same =
            (have_destination && destination_result == 0 &&
             same_contents_metadata (&destination_before, &destination_after) &&
             destination_before.st_ctim.tv_sec == destination_after.st_ctim.tv_sec &&
             destination_before.st_ctim.tv_nsec == destination_after.st_ctim.tv_nsec) ||
            (missing_destination && destination_result < 0 && errno == ENOENT);
        errno = EINTR;
        if (!source_same || !destination_same)
            break;
    }
    return transfer_error (error, _("Could not atomically publish without losing an existing entry"));
}

static gboolean
rename_result_uncertain (GError *error)
{
    return !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS) &&
           !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) &&
           !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT) &&
           !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG) &&
           !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
           !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
}

static gboolean
recovery_has_slot (TransferRecovery *recovery, const char *slot)
{
    struct stat current;
    return recovery->fd >= 0 &&
           fstatat (recovery->fd, slot, &current, AT_SYMLINK_NOFOLLOW) == 0;
}

NemoTransferTransaction *
nemo_transfer_transaction_new (NemoTransferGuard *guard, GFile *source, GFile *destination,
                               GCancellable *cancel, GError **error)
{
    g_autoptr (GFile) source_parent = NULL;
    g_autoptr (GFile) destination_parent = NULL;
    g_autofree char *source_path = NULL;
    if (!nemo_transfer_guard_check (guard, error))
        return NULL;
    NemoTransferTransaction *transaction = g_new0 (NemoTransferTransaction, 1);
    transaction->refs = 1;
    transaction->recovery.fd = -1;
    transaction->guard = nemo_transfer_guard_ref (guard);
    transaction->source = g_object_ref (source);
    transaction->destination = g_object_ref (destination);
    transaction->source_name = g_file_get_basename (source);
    transaction->destination_name = g_file_get_basename (destination);
    transaction->source_file = nemo_transfer_guard_file (guard, source, FALSE, error);
    if (!transaction->source_file)
        goto failed;
    transaction->destination_file = nemo_transfer_guard_file (guard, destination, TRUE, error);
    if (!transaction->destination_file)
        goto failed;
    source_parent = g_file_get_parent (source);
    destination_parent = g_file_get_parent (destination);
    transaction->destination_parent = guard_directory (guard, destination_parent, TRUE, FALSE, error);
    if (!transaction->destination_parent)
        goto failed;
    source_path = g_file_get_path (source);
    if (source_path && g_file_is_native (source) && supported_mount_path (source_path, FALSE)) {
        transaction->source_parent = guard_directory (guard, source_parent, FALSE, FALSE, error);
        if (!transaction->source_parent ||
            !snapshot_at (transaction->source_parent->fd, transaction->source_name,
                          &transaction->source_before, FALSE, FALSE, cancel, error))
            goto failed;
    }
    if (!recovery_new (recovery_parent (guard, transaction->destination_parent, TRUE),
                       &transaction->recovery, cancel, error))
        goto failed;
    transaction->stage = file_at (transaction->recovery.fd, "payload");
    if (!recovery_record (transaction, &transaction->recovery, "prepared", cancel, error))
        goto failed;
    return transaction;
failed:
    nemo_transfer_transaction_free (transaction);
    return NULL;
}

GFile *
nemo_transfer_transaction_source (NemoTransferTransaction *transaction)
{
    return transaction->source_file;
}

GFile *
nemo_transfer_transaction_destination (NemoTransferTransaction *transaction)
{
    return transaction->destination_file;
}

GFile *
nemo_transfer_transaction_stage (NemoTransferTransaction *transaction)
{
    return transaction->stage;
}

gboolean
nemo_transfer_transaction_stage_created (NemoTransferTransaction *transaction, GError **error)
{
    if (fstatat (transaction->recovery.fd, "payload", &transaction->stage_identity, AT_SYMLINK_NOFOLLOW) < 0)
        return transfer_error (error, _("Could not identify the private staging entry"));
    transaction->stage_owned = TRUE;
    return TRUE;
}

gboolean
nemo_transfer_transaction_publish (NemoTransferTransaction *transaction, gboolean overwrite,
                                   gboolean *published, GCancellable *cancel, GError **error)
{
    NemoTransferGuard *guard = transaction->guard;
    TransferDirectory *parent = transaction->destination_parent;
    TransferSnapshot *expected = g_hash_table_lookup (guard->expected, transaction->destination);
    TransferSnapshot current = { 0 };
    gboolean ok = FALSE;
    if (!nemo_transfer_guard_check (guard, error) || !directory_check (parent, error) ||
        !recovery_identity_check (&transaction->recovery, error) ||
        g_cancellable_set_error_if_cancelled (cancel, error))
        goto out;
    if (!snapshot_at (transaction->recovery.fd, "payload", &transaction->installed,
                      TRUE, S_ISLNK (transaction->stage_identity.st_mode), cancel, error))
        goto out;
    if (!same_inode (&transaction->stage_identity, &transaction->installed.stat)) {
        changed (error);
        goto out;
    }
    if (overwrite && expected) {
        if (!snapshot_at (parent->fd, transaction->destination_name, &current,
                          TRUE, TRUE, cancel, error))
            goto out;
        if (!snapshot_matches (expected, &current)) {
            changed (error);
            goto out;
        }
        if (!sync_fd (parent->fd, cancel, error))
            goto out;
        if (!rename_entry (transaction->recovery.fd, "payload", parent->fd,
                           transaction->destination_name, RENAME_EXCHANGE, error))
            goto rename_failed;
        transaction->backup = TRUE;
    } else {
        if (!rename_entry (transaction->recovery.fd, "payload", parent->fd,
                           transaction->destination_name, RENAME_NOREPLACE, error))
            goto rename_failed;
    }
    transaction->stage_owned = FALSE;
    transaction->published = *published = TRUE;
    if (!g_hash_table_contains (guard->published_destinations, transaction->destination))
        g_hash_table_insert (guard->published_destinations, g_object_ref (transaction->destination), NULL);
    if (transaction->backup) {
        snapshot_clear (&current);
        report_recovery_entry (guard, &transaction->recovery, "payload",
                               _("The previous destination was preserved for recovery and undo."));
        if (!snapshot_at (transaction->recovery.fd, "payload", &current, TRUE, TRUE, cancel, error) ||
            !snapshot_matches (expected, &current)) {
            if (!*error)
                changed (error);
            goto out;
        }
        snapshot_clear (&transaction->original);
        transaction->original = current;
        memset (&current, 0, sizeof current);
    }
    if (!sync_fd (parent->fd, cancel, error) ||
        !sync_fd (transaction->recovery.fd, cancel, error) ||
        !directory_check (parent, error) ||
        !recovery_identity_check (&transaction->recovery, error))
        goto out;
    snapshot_clear (&current);
    if (!snapshot_at (parent->fd, transaction->destination_name, &current, TRUE, FALSE, cancel, error) ||
        !snapshot_matches (&transaction->installed, &current)) {
        if (!*error)
            changed (error);
        goto out;
    }
    if (!recovery_record (transaction, &transaction->recovery, "published", cancel, error))
        goto out;
    if (!check_directory_close (parent->fd, error) ||
        !check_directory_close (transaction->recovery.fd, error))
        goto out;
    ok = TRUE;
    goto out;
rename_failed:
    if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
        GError *snapshot_error = NULL;
        if (!nemo_transfer_guard_expect (guard, transaction->destination, &snapshot_error)) {
            g_clear_error (error);
            g_propagate_error (error, snapshot_error);
        }
    } else if (rename_result_uncertain (*error)) {
        transaction->stage_owned = FALSE;
        transaction->published = *published = TRUE;
        transaction->publication_uncertain = TRUE;
        transaction->backup = overwrite && expected != NULL;
        report_recovery (guard, &transaction->recovery,
                         _("Publication returned an uncertain error. The destination and "
                           "private entries were retained; no rollback or retry was attempted."));
    }
out:
    if (!ok && transaction->published) {
        nemo_transfer_guard_describe_destination (guard, transaction->destination);
        if (transaction->backup)
            report_recovery_entry (guard, &transaction->recovery, "payload",
                                   transaction->publication_uncertain ?
                                   _("The private entry was retained because an exchange may have completed. "
                                     "Inspect it before attempting recovery.") :
                                   _("The displaced destination entry was retained; publication was not confirmed."));
        else
            report_recovery (guard, &transaction->recovery,
                             _("Publication was not confirmed. Inspect the source and destination "
                               "before removing anything; the recovery record was retained."));
    }
    snapshot_clear (&current);
    return ok;
}

static gboolean
restore_capture (NemoTransferTransaction *transaction, TransferRecovery *capture,
                 const char *slot, GError **error)
{
    if (!directory_check (transaction->source_parent, error) ||
        !rename_entry (capture->fd, slot, transaction->source_parent->fd,
                       transaction->source_name, RENAME_NOREPLACE, error))
        return FALSE;
    return sync_fd (capture->fd, NULL, error) &&
           sync_fd (transaction->source_parent->fd, NULL, error);
}

gboolean
nemo_transfer_transaction_retire_source (NemoTransferTransaction *transaction,
                                         GCancellable *cancel, GError **error)
{
    TransferRecovery capture = { .fd = -1 };
    TransferSnapshot source = { 0 }, destination = { 0 };
    gboolean captured = FALSE, ok = FALSE;
    if (!transaction->source_parent)
        return unsupported (error);
    if (!nemo_transfer_guard_check (transaction->guard, error) ||
        !directory_check (transaction->source_parent, error) ||
        !directory_check (transaction->destination_parent, error) ||
        !snapshot_at (transaction->destination_parent->fd, transaction->destination_name,
                      &destination, TRUE, FALSE, cancel, error))
        goto out;
    if (!snapshot_matches (&transaction->installed, &destination)) {
        changed (error);
        goto out;
    }
    if (!recovery_new (recovery_parent (transaction->guard, transaction->source_parent, FALSE),
                       &capture, cancel, error) ||
        !recovery_record (transaction, &capture, "source-prepared", cancel, error) ||
        g_cancellable_set_error_if_cancelled (cancel, error))
        goto out;
    if (!rename_entry (transaction->source_parent->fd, transaction->source_name,
                       capture.fd, "captured-source", RENAME_NOREPLACE, error)) {
        captured = recovery_has_slot (&capture, "captured-source");
        goto out;
    }
    captured = TRUE;
    if (!sync_fd (transaction->source_parent->fd, cancel, error) ||
        !sync_fd (capture.fd, cancel, error) ||
        !snapshot_at (capture.fd, "captured-source", &source, TRUE, FALSE, cancel, error))
        goto out;
    if (!same_contents_metadata (&transaction->source_before.stat, &source.stat) ||
        g_strcmp0 (source.checksum, transaction->installed.checksum) != 0 ||
        g_strcmp0 (source.link, transaction->installed.link) != 0) {
        changed (error);
        goto out;
    }
    snapshot_clear (&destination);
    if (!directory_check (transaction->destination_parent, error) ||
        !snapshot_at (transaction->destination_parent->fd, transaction->destination_name,
                      &destination, TRUE, FALSE, cancel, error) ||
        g_cancellable_set_error_if_cancelled (cancel, error))
        goto out;
    if (!snapshot_matches (&transaction->installed, &destination)) {
        changed (error);
        goto out;
    }
    if (!recovery_identity_check (&capture, error))
        goto out;
    if (unlinkat (capture.fd, "captured-source", 0) < 0) {
        transfer_error (error, _("Could not remove the verified captured source"));
        goto out;
    }
    captured = FALSE;
    if (!sync_fd (capture.fd, cancel, error) ||
        !sync_fd (transaction->source_parent->fd, cancel, error) ||
        !check_directory_close (capture.fd, error) ||
        !check_directory_close (transaction->source_parent->fd, error)) {
        g_string_append (transaction->guard->details,
                         _("\nThe destination was confirmed. The source was removed, "
                           "but persistence of source removal could not be confirmed."));
        goto out;
    }
    ok = TRUE;
out:
    if (captured) {
        GError *restore_error = NULL;
        if (!restore_capture (transaction, &capture, "captured-source", &restore_error)) {
            report_recovery_entry (transaction->guard, &capture, "captured-source",
                                   _("The captured source was retained; its original name could not be restored safely."));
            g_clear_error (&restore_error);
        } else {
            g_string_append (transaction->guard->details,
                             _("\nThe source was restored without replacing another entry."));
        }
    }
    snapshot_clear (&source);
    snapshot_clear (&destination);
    if (ok && !recovery_finish (&capture, error)) {
        ok = FALSE;
        g_string_append (transaction->guard->details,
                         _("\nThe source data was removed after confirmed publication, "
                           "but its recovery folder handle could not be closed."));
    }
    recovery_close (&capture);
    return ok;
}

gboolean
nemo_transfer_retire_directory (NemoTransferGuard *guard, GFile *source,
                                GCancellable *cancel, GError **error)
{
    g_autoptr (GFile) parent_file = g_file_get_parent (source);
    TransferDirectory *parent = guard_directory (guard, parent_file, FALSE, FALSE, error);
    if (!parent)
        return FALSE;
    TransferDirectory *directory = guard_directory (guard, source, FALSE, FALSE, error);
    if (!directory)
        return FALSE;
    g_autofree char *name = g_file_get_basename (source);
    TransferRecovery capture = { .fd = -1 };
    NemoTransferTransaction record = { .guard = guard, .source = source, .destination = source };
    gboolean captured = FALSE, ok = FALSE;
    if (!recovery_new (recovery_parent (guard, parent, FALSE), &capture, cancel, error) ||
        !snapshot_at (parent->fd, name, &record.source_before, FALSE, FALSE, cancel, error) ||
        !recovery_record (&record, &capture, "source-directory-prepared", cancel, error) ||
        g_cancellable_set_error_if_cancelled (cancel, error) ||
        !directory_check (directory, error))
        goto out;
    if (!rename_entry (parent->fd, name, capture.fd, "captured-source", RENAME_NOREPLACE, error)) {
        captured = recovery_has_slot (&capture, "captured-source");
        goto out;
    }
    captured = TRUE;
    struct statx current;
    if (statx (capture.fd, "captured-source", AT_SYMLINK_NOFOLLOW,
               STATX_BASIC_STATS | STATX_MNT_ID | STATX_MNT_ID_UNIQUE, &current) < 0) {
        transfer_error (error, _("Could not inspect the captured source folder"));
        goto out;
    }
    if (!same_directory (&directory->identity, &current)) {
        changed (error);
        goto out;
    }
    if (g_cancellable_set_error_if_cancelled (cancel, error))
        goto out;
    if (!recovery_identity_check (&capture, error))
        goto out;
    /* AT_REMOVEDIR cannot delete a newly appeared child. No recursive delete. */
    if (unlinkat (capture.fd, "captured-source", AT_REMOVEDIR) < 0) {
        transfer_error (error, _("Could not remove the empty captured source folder"));
        goto out;
    }
    captured = FALSE;
    ok = sync_fd (capture.fd, cancel, error) && sync_fd (parent->fd, cancel, error) &&
         check_directory_close (capture.fd, error) && check_directory_close (parent->fd, error);
out:
    if (captured) {
        GError *restore_error = NULL;
        if (!rename_entry (capture.fd, "captured-source", parent->fd, name,
                           RENAME_NOREPLACE, &restore_error))
            report_recovery_entry (guard, &capture, "captured-source",
                                   _("The source folder was retained in recovery storage."));
        else if (!sync_fd (capture.fd, NULL, &restore_error) || !sync_fd (parent->fd, NULL, &restore_error))
            g_string_append (guard->details, _("\nSource folder restoration could not be synchronized."));
        g_clear_error (&restore_error);
    }
    if (ok && !recovery_finish (&capture, error)) {
        ok = FALSE;
        g_string_append (guard->details,
                         _("\nThe empty source folder was removed, but its recovery handle "
                           "could not be closed."));
    }
    recovery_close (&capture);
    snapshot_clear (&record.source_before);
    return ok;
}

void
nemo_transfer_transaction_free (NemoTransferTransaction *transaction)
{
    if (!transaction)
        return;
    if (!g_atomic_int_dec_and_test (&transaction->refs)) {
        /* Undo retains identity and recovery data, not one open descriptor per
         * copied photo. Reopening later must match the recorded directory. */
        if (transaction->recovery.fd >= 0) {
            close (transaction->recovery.fd);
            transaction->recovery.fd = -1;
        }
        return;
    }
    if (transaction->stage_owned && !transaction->published && !transaction->native_source) {
        struct stat current;
        if (fstatat (transaction->recovery.fd, "payload", &current, AT_SYMLINK_NOFOLLOW) == 0 &&
            same_inode (&transaction->stage_identity, &current)) {
            if (unlinkat (transaction->recovery.fd, "payload", 0) < 0)
                report_recovery (transaction->guard, &transaction->recovery,
                                 _("An incomplete staging entry could not be removed."));
        } else {
            report_recovery (transaction->guard, &transaction->recovery,
                             _("A changed staging entry was retained."));
        }
    }
    recovery_close (&transaction->recovery);
    snapshot_clear (&transaction->source_before);
    snapshot_clear (&transaction->installed);
    snapshot_clear (&transaction->original);
    g_clear_object (&transaction->source);
    g_clear_object (&transaction->destination);
    g_clear_object (&transaction->source_file);
    g_clear_object (&transaction->destination_file);
    g_clear_object (&transaction->stage);
    g_free (transaction->source_name);
    g_free (transaction->destination_name);
    nemo_transfer_guard_unref (transaction->guard);
    g_free (transaction);
}

static gboolean
sync_tree (int parent, const char *name, guint64 mount_id, GCancellable *cancel,
           GError **error)
{
    TransferSnapshot before = { 0 }, after = { 0 };
    gboolean ok = snapshot_at (parent, name, &before, FALSE, TRUE, cancel, error);
    if (!ok || !S_ISDIR (before.stat.st_mode))
        goto out;
    int fd = openat (parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        ok = transfer_error (error, _("Could not open the source tree"));
        goto out;
    }
    struct statx identity;
    ok = directory_identity (fd, &identity, error);
    if (ok && identity.stx_mnt_id != mount_id)
        ok = unsupported (error);
    DIR *entries = NULL;
    if (ok) {
        int scan = fcntl (fd, F_DUPFD_CLOEXEC, 3);
        if (scan < 0 || !(entries = fdopendir (scan))) {
            if (scan >= 0)
                close (scan);
            ok = transfer_error (error, _("Could not enumerate the source tree"));
        }
    }
    while (ok && entries) {
        errno = 0;
        struct dirent *entry = readdir (entries);
        if (!entry) {
            if (errno)
                ok = transfer_error (error, _("Could not read the source tree"));
            break;
        }
        if (!strcmp (entry->d_name, ".") || !strcmp (entry->d_name, ".."))
            continue;
        ok = sync_tree (fd, entry->d_name, mount_id, cancel, error);
    }
    if (entries && closedir (entries) < 0 && ok)
        ok = transfer_error (error, _("Could not close the source tree enumeration"));
    if (ok)
        ok = sync_fd (fd, cancel, error);
    if (!close_checked (fd, ok ? error : NULL))
        ok = FALSE;
    if (ok) {
        ok = snapshot_at (parent, name, &after, FALSE, FALSE, cancel, error);
        if (ok && !snapshot_matches (&before, &after))
            ok = changed (error);
    }
out:
    snapshot_clear (&before);
    snapshot_clear (&after);
    return ok;
}

gboolean
nemo_transfer_sync_existing (NemoTransferGuard *guard, GFile *destination,
                            GCancellable *cancel, GError **error)
{
    if (!nemo_transfer_guard_check (guard, error))
        return FALSE;
    g_autoptr (GFile) parent_file = g_file_get_parent (destination);
    TransferDirectory *parent = guard_directory (guard, parent_file, TRUE, FALSE, error);
    g_autofree char *name = g_file_get_basename (destination);
    TransferSnapshot snapshot = { 0 };
    if (!parent)
        return FALSE;
    gboolean ok = snapshot_at (parent->fd, name, &snapshot, FALSE, TRUE, cancel, error) &&
                  sync_fd (parent->fd, cancel, error) && directory_check (parent, error) &&
                  check_directory_close (parent->fd, error);
    snapshot_clear (&snapshot);
    return ok;
}

gboolean
nemo_transfer_native_move (NemoTransferGuard *guard, GFile *source, GFile *destination,
                           gboolean overwrite, gboolean *published, NemoTransferUndo **undo,
                           GCancellable *cancel, GError **error)
{
    if (!nemo_transfer_guard_check (guard, error))
        return FALSE;
    g_autoptr (GFile) source_parent_file = g_file_get_parent (source);
    g_autoptr (GFile) destination_parent_file = g_file_get_parent (destination);
    TransferDirectory *source_parent = guard_directory (guard, source_parent_file, FALSE, FALSE, error);
    if (!source_parent)
        return FALSE;
    TransferDirectory *destination_parent = guard_directory (guard, destination_parent_file, TRUE, FALSE, error);
    if (!destination_parent)
        return FALSE;
    g_autofree char *source_name = g_file_get_basename (source);
    g_autofree char *destination_name = g_file_get_basename (destination);
    struct stat source_stat, destination_stat;
    if (fstatat (source_parent->fd, source_name, &source_stat, AT_SYMLINK_NOFOLLOW) < 0)
        return transfer_error (error, _("Could not inspect the move source"));
    if (fstatat (destination_parent->fd, destination_name, &destination_stat, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!nemo_transfer_guard_expect (guard, destination, error))
            return FALSE;
        if (!overwrite) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS, _("File exists"));
            return FALSE;
        }
        if (S_ISDIR (source_stat.st_mode) && S_ISDIR (destination_stat.st_mode)) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE,
                                 _("The folder contents must be merged individually."));
            return FALSE;
        }
        if (S_ISDIR (source_stat.st_mode) != S_ISDIR (destination_stat.st_mode))
            return unsupported (error);
    } else if (errno != ENOENT) {
        return transfer_error (error, _("Could not inspect the move destination"));
    }
    if (source_parent->identity.stx_mnt_id != destination_parent->identity.stx_mnt_id) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             _("A verified copy is required between these filesystems."));
        return FALSE;
    }
    if (!sync_tree (source_parent->fd, source_name, source_parent->identity.stx_mnt_id, cancel, error) ||
        !sync_fd (source_parent->fd, cancel, error))
        return FALSE;
    NemoTransferTransaction *transaction = nemo_transfer_transaction_new (guard, source, destination, cancel, error);
    if (!transaction)
        return FALSE;
    gboolean captured = FALSE, ok = FALSE;
    TransferSnapshot captured_snapshot = { 0 };
    transaction->native_source = TRUE;
    if (!same_contents_metadata (&source_stat, &transaction->source_before.stat)) {
        changed (error);
        goto out;
    }
    if (!directory_check (source_parent, error) ||
        g_cancellable_set_error_if_cancelled (cancel, error))
        goto out;
    if (!rename_entry (source_parent->fd, source_name, transaction->recovery.fd,
                       "payload", RENAME_NOREPLACE, error)) {
        captured = recovery_has_slot (&transaction->recovery, "payload");
        goto out;
    }
    captured = TRUE;
    if (!nemo_transfer_transaction_stage_created (transaction, error) ||
        !snapshot_at (transaction->recovery.fd, "payload", &captured_snapshot, FALSE, FALSE, cancel, error))
        goto out;
    if (!same_contents_metadata (&transaction->source_before.stat, &captured_snapshot.stat)) {
        changed (error);
        goto out;
    }
    if (!sync_fd (source_parent->fd, cancel, error) ||
        !sync_fd (transaction->recovery.fd, cancel, error) ||
        !nemo_transfer_transaction_publish (transaction, overwrite, published, cancel, error))
        goto out;
    captured = FALSE;
    if (!sync_fd (source_parent->fd, cancel, error) ||
        !directory_check (source_parent, error) ||
        !check_directory_close (source_parent->fd, error) ||
        !nemo_transfer_transaction_finish (transaction, error))
        goto out;
    if (undo)
        *undo = nemo_transfer_transaction_undo (transaction, TRUE);
    ok = TRUE;
out:
    if (transaction->published) {
        if (!ok) {
            g_autofree char *destination_path = g_file_get_parse_name (destination);
            g_string_append_printf (guard->details,
                                    _("\nThe native move changed or may have changed a namespace, but completion "
                                      "could not be confirmed. Inspect destination: %s\n"
                                      "Do not assume the former source name was retained."),
                                    destination_path);
        }
    } else if (captured) {
        GError *restore_error = NULL;
        if (!restore_capture (transaction, &transaction->recovery, "payload", &restore_error)) {
            *published = TRUE; /* State changed: callers must not retry or fall back. */
            report_recovery_entry (guard, &transaction->recovery, "payload",
                                   _("The captured move source entry was retained in recovery storage."));
            g_clear_error (&restore_error);
        }
    }
    snapshot_clear (&captured_snapshot);
    nemo_transfer_transaction_free (transaction);
    return ok;
}

NemoTransferUndo *
nemo_transfer_transaction_undo (NemoTransferTransaction *transaction, gboolean move)
{
    NemoTransferUndo *undo = g_new0 (NemoTransferUndo, 1);
    undo->refs = 1;
    undo->transaction = transaction;
    undo->move = move;
    g_atomic_int_inc (&transaction->refs);
    return undo;
}

NemoTransferUndo *
nemo_transfer_undo_ref (NemoTransferUndo *undo)
{
    g_atomic_int_inc (&undo->refs);
    return undo;
}

void
nemo_transfer_undo_unref (NemoTransferUndo *undo)
{
    if (!undo || !g_atomic_int_dec_and_test (&undo->refs))
        return;
    nemo_transfer_transaction_free (undo->transaction);
    g_free (undo);
}

char *
nemo_transfer_undo_take_details (NemoTransferUndo *undo)
{
    return nemo_transfer_guard_take_details (undo->transaction->guard);
}

gboolean
nemo_transfer_undo_release (NemoTransferUndo *undo, GError **error)
{
    TransferRecovery *recovery = &undo->transaction->recovery;
    gboolean ok = TRUE;
    if (recovery->fd >= 0) {
        int fd = recovery->fd;
        recovery->fd = -1;
        ok = close_checked (fd, error);
    }
    if (!nemo_transfer_guard_release (undo->transaction->guard, ok ? error : NULL))
        ok = FALSE;
    return ok;
}

static gboolean
undo_capture (NemoTransferTransaction *transaction, int parent, const char *name,
              const char *slot, const TransferSnapshot *expected, GCancellable *cancel,
              GError **error)
{
    TransferSnapshot current = { 0 };
    gboolean ok = snapshot_at (parent, name, &current, TRUE, TRUE, cancel, error);
    if (ok && !snapshot_matches (expected, &current))
        ok = changed (error);
    snapshot_clear (&current);
    if (!ok)
        return FALSE;
    if (!rename_entry (parent, name, transaction->recovery.fd, slot, RENAME_NOREPLACE, error)) {
        if (recovery_has_slot (&transaction->recovery, slot))
            report_recovery (transaction->guard, &transaction->recovery,
                             _("An undo capture returned an uncertain error. Its entry was retained."));
        return FALSE;
    }
    ok = snapshot_at (transaction->recovery.fd, slot, &current, TRUE, FALSE, cancel, error);
    if (ok && !snapshot_matches (expected, &current))
        ok = changed (error);
    snapshot_clear (&current);
    if (!ok) {
        GError *restore_error = NULL;
        if (!rename_entry (transaction->recovery.fd, slot, parent, name,
                           RENAME_NOREPLACE, &restore_error))
            report_recovery (transaction->guard, &transaction->recovery,
                             _("A changed undo target was retained without overwriting its newer name."));
        else if (!sync_fd (parent, NULL, &restore_error) ||
                 !sync_fd (transaction->recovery.fd, NULL, &restore_error))
            g_string_append (transaction->guard->details,
                             _("\nThe undo target was restored, but its namespace could not be synchronized."));
        g_clear_error (&restore_error);
        return FALSE;
    }
    if (!sync_fd (parent, cancel, error) || !sync_fd (transaction->recovery.fd, cancel, error)) {
        report_recovery (transaction->guard, &transaction->recovery,
                         _("Undo stopped after capture. The captured entry was retained."));
        return FALSE;
    }
    return TRUE;
}

static gboolean
check_snapshot (int fd, const char *name, const TransferSnapshot *expected,
                GCancellable *cancel, GError **error)
{
    TransferSnapshot current = { 0 };
    gboolean ok = snapshot_at (fd, name, &current, TRUE, FALSE, cancel, error);
    if (ok && !snapshot_matches (expected, &current))
        ok = changed (error);
    snapshot_clear (&current);
    return ok;
}

static gboolean
check_absent (int fd, const char *name, GError **error)
{
    struct stat current;
    if (fstatat (fd, name, &current, AT_SYMLINK_NOFOLLOW) == 0) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                             _("A restoration name is occupied. Undo or redo did not replace it."));
        return FALSE;
    }
    return errno == ENOENT || transfer_error (error, _("Could not inspect the restoration name"));
}

gboolean
nemo_transfer_undo_check (NemoTransferUndo *undo, gboolean redo,
                         GCancellable *cancel, GError **error)
{
    NemoTransferTransaction *transaction = undo->transaction;
    if (redo != undo->undone || S_ISDIR (transaction->installed.stat.st_mode)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             _("Folder undo cannot establish a complete safe recovery record. "
                               "No files were changed. Retained recovery files can be copied manually."));
        return FALSE;
    }
    if (!nemo_transfer_guard_check (transaction->guard, error) ||
        !directory_check (transaction->destination_parent, error))
        return FALSE;
    if (undo->move) {
        if (!transaction->source_parent)
            return unsupported (error);
        if (!directory_check (transaction->source_parent, error))
            return FALSE;
        if (transaction->source_parent->identity.stx_mnt_id !=
            transaction->destination_parent->identity.stx_mnt_id) {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                 _("Cross-filesystem move undo is unavailable because a safe reverse "
                                   "transfer cannot be established. No files were changed. "
                                   "Use Copy to restore files manually."));
            return FALSE;
        }
    }
    if (!recovery_reopen (&transaction->recovery, error))
        return FALSE;
    if (!recovery_identity_check (&transaction->recovery, error)) {
        report_recovery (transaction->guard, &transaction->recovery,
                         _("Undo stopped because the recovery folder changed."));
        return FALSE;
    }
    gboolean ok;
    if (!redo) {
        ok = check_snapshot (transaction->destination_parent->fd, transaction->destination_name,
                              &transaction->installed, cancel, error) &&
             (!undo->move || check_absent (transaction->source_parent->fd, transaction->source_name, error)) &&
             (!transaction->backup || check_snapshot (transaction->recovery.fd, "payload",
                                                       &transaction->original, cancel, error));
    } else {
        ok = transaction->backup ?
             check_snapshot (transaction->destination_parent->fd, transaction->destination_name,
                              &transaction->original, cancel, error) :
             check_absent (transaction->destination_parent->fd, transaction->destination_name, error);
        if (ok)
            ok = undo->move ?
                 check_snapshot (transaction->source_parent->fd, transaction->source_name,
                                  &transaction->installed, cancel, error) :
                 check_snapshot (transaction->recovery.fd, "undone",
                                  &transaction->installed, cancel, error);
    }
    if (!recovery_finish (&transaction->recovery, ok ? error : NULL))
        ok = FALSE;
    return ok;
}

gboolean
nemo_transfer_undo_apply (NemoTransferUndo *undo, gboolean redo,
                         GCancellable *cancel, GError **error)
{
    NemoTransferTransaction *transaction = undo->transaction;
    TransferDirectory *destination = transaction->destination_parent;
    gboolean ok = FALSE;
    if (!nemo_transfer_undo_check (undo, redo, cancel, error))
        return FALSE;
    if (redo != undo->undone || S_ISDIR (transaction->installed.stat.st_mode)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             _("This undo record cannot safely restore the folder or its current contents. "
                               "No files were deleted."));
        return FALSE;
    }
    if (undo->move && !transaction->source_parent)
        return unsupported (error);
    if (undo->move && !directory_check (transaction->source_parent, error))
        return FALSE;
    if (!recovery_reopen (&transaction->recovery, error))
        return FALSE;
    g_autofree char *phase = g_strdup_printf ("%s-%u-prepared", redo ? "redo" : "undo", ++undo->serial);
    if (!recovery_record (transaction, &transaction->recovery, phase, cancel, error))
        return FALSE;
    if (!redo) {
        if (undo->move) {
            struct stat current;
            if (fstatat (transaction->source_parent->fd, transaction->source_name,
                         &current, AT_SYMLINK_NOFOLLOW) == 0) {
                g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                                     _("The original source name is occupied. Undo did not replace it."));
                return FALSE;
            }
            if (errno != ENOENT)
                return transfer_error (error, _("Could not inspect the original source name"));
            if (transaction->source_parent->identity.stx_mnt_id != destination->identity.stx_mnt_id)
                return unsupported (error);
        }
        if (!undo_capture (transaction, destination->fd, transaction->destination_name,
                           "undone", &transaction->installed, cancel, error))
            return FALSE;
        if (undo->move && !rename_entry (transaction->recovery.fd, "undone",
                                         transaction->source_parent->fd, transaction->source_name,
                                         RENAME_NOREPLACE, error))
            goto retained;
        if (transaction->backup &&
            !rename_entry (transaction->recovery.fd, "payload", destination->fd,
                           transaction->destination_name, RENAME_NOREPLACE, error))
            goto retained;
    } else {
        if (transaction->backup &&
            !undo_capture (transaction, destination->fd, transaction->destination_name,
                           "payload", &transaction->original, cancel, error))
            return FALSE;
        if (undo->move &&
            !undo_capture (transaction, transaction->source_parent->fd, transaction->source_name,
                           "undone", &transaction->installed, cancel, error))
            goto retained;
        if (!rename_entry (transaction->recovery.fd, "undone", destination->fd,
                           transaction->destination_name, RENAME_NOREPLACE, error))
            goto retained;
    }
    ok = sync_fd (destination->fd, cancel, error) &&
         sync_fd (transaction->recovery.fd, cancel, error) &&
         (!undo->move || sync_fd (transaction->source_parent->fd, cancel, error)) &&
         check_directory_close (destination->fd, error) &&
         check_directory_close (transaction->recovery.fd, error) &&
         (!undo->move || check_directory_close (transaction->source_parent->fd, error));
    if (!recovery_finish (&transaction->recovery, ok ? error : NULL))
        ok = FALSE;
    if (ok)
        undo->undone = !redo;
    if (ok && !redo && !undo->move)
        report_recovery (transaction->guard, &transaction->recovery,
                         _("The undone copy was retained for guarded redo."));
retained:
    if (!ok)
        report_recovery (transaction->guard, &transaction->recovery,
                         _("Undo or redo was incomplete. Captured and prior destination entries were retained."));
    recovery_finish (&transaction->recovery, NULL);
    return ok;
}

#endif
