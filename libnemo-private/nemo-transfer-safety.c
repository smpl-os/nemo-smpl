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
#include <sys/resource.h>
#include <sys/vfs.h>
#include <unistd.h>

#ifndef STATX_MNT_ID_UNIQUE
#define STATX_MNT_ID_UNIQUE 0x00004000U
#endif

typedef enum {
    TRANSFER_EXCHANGE_UNKNOWN,
    TRANSFER_EXCHANGE_SUPPORTED,
    TRANSFER_EXCHANGE_UNSUPPORTED
} TransferExchangeCapability;

typedef struct {
    NemoTransferGuard *guard;
    GFile *file;
    char *path;
    int fd;
    struct statx identity;
    gboolean destination;
    gboolean root;
    guint pins;
    GList cache_link;
    TransferExchangeCapability exchange_capability;
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
    char *container_name;
    int container_fd;
    struct stat container_identity;
    TransferSnapshot marker;
    char *bucket_name;
    int bucket_fd;
    struct stat bucket_identity;
    TransferSnapshot bucket_marker;
    GHashTable *records;
    gboolean cleaned;
    gboolean parent_held;
} TransferRecovery;

struct _NemoTransferGuard {
    gint refs;
    GPtrArray *directories;
    GPtrArray *roots;
    GHashTable *directory_index;
    GQueue directory_cache;
    guint cache_limit;
    GHashTable *expected;
    GHashTable *record_parents;
    GHashTable *published_destinations;
    GError *error;
    GString *details;
    gboolean move;
    gboolean released;
    gboolean privacy_reported;
    gboolean replacement_unsupported;
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
               STATX_BASIC_STATS | STATX_BTIME | STATX_MNT_ID | STATX_MNT_ID_UNIQUE, identity) < 0)
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
           (!(a->stx_mask & STATX_BTIME) ||
            ((b->stx_mask & STATX_BTIME) &&
             a->stx_btime.tv_sec == b->stx_btime.tv_sec &&
             a->stx_btime.tv_nsec == b->stx_btime.tv_nsec)) &&
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

static TransferDirectory *
directory_hold (TransferDirectory *directory)
{
    if (directory)
        directory->pins++;
    return directory;
}

static void
directory_unpin (TransferDirectory *directory)
{
    if (directory) {
        g_assert (directory->pins > 0);
        directory->pins--;
    }
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (TransferDirectory, directory_unpin)

static void
directory_cache_remove (TransferDirectory *directory)
{
    if (directory->cache_link.data) {
        g_queue_unlink (&directory->guard->directory_cache, &directory->cache_link);
        directory->cache_link.data = NULL;
    }
}

static void
directory_cache_touch (TransferDirectory *directory)
{
    directory_cache_remove (directory);
    if (!directory->root && directory->fd >= 0) {
        directory->cache_link.data = directory;
        g_queue_push_tail_link (&directory->guard->directory_cache, &directory->cache_link);
    }
}

static gboolean
directory_cache_room (NemoTransferGuard *guard, TransferDirectory *keep, GError **error)
{
    while (guard->directory_cache.length >= guard->cache_limit) {
        TransferDirectory *candidate = NULL;
        for (GList *l = guard->directory_cache.head; l; l = l->next) {
            TransferDirectory *directory = l->data;
            if (directory != keep && directory->pins == 0) {
                candidate = directory;
                break;
            }
        }
        /* Active aliases must keep their descriptor numbers. The cache bound
         * excludes the live recursive call stack and permanent queue roots. */
        if (!candidate)
            break;
        directory_cache_remove (candidate);
        int fd = candidate->fd;
        candidate->fd = -1;
        GError *close_error = NULL;
        if (!close_checked (fd, &close_error)) {
            if (!guard->error)
                guard->error = g_error_copy (close_error);
            g_propagate_error (error, close_error);
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean
directory_check (TransferDirectory *directory, GError **error)
{
    struct statx current;
    if (!supported_mount_path (directory->path, directory->destination))
        return changed (error);
    if (!directory_cache_room (directory->guard, directory, error))
        return FALSE;
    int fd = open_root (directory->path, error);
    if (fd < 0)
        return FALSE;
    gboolean ok = directory_identity (fd, &current, error);
    if (ok && !same_directory (&directory->identity, &current))
        ok = changed (error);
    if (ok && directory->fd < 0) {
        directory->fd = fd;
        directory_cache_touch (directory);
        return TRUE;
    }
    if (!close_checked (fd, ok ? error : NULL))
        ok = FALSE;
    if (ok)
        directory_cache_touch (directory);
    return ok;
}

static TransferDirectory *
guard_directory (NemoTransferGuard *guard, GFile *file, gboolean destination,
                 gboolean initial, GError **error)
{
    if (!initial && destination) {
        gboolean within_destination = FALSE;
        for (guint i = 0; i < guard->roots->len; i++) {
            TransferDirectory *root = g_ptr_array_index (guard->roots, i);
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
    TransferDirectory *existing = g_hash_table_lookup (guard->directory_index, file);
    if (existing) {
        /* Repeated selections share the first queue-time pin. The worker
         * revalidates it before any transfer, not once per selected photo. */
        if (initial && existing->root && (!destination || existing->destination))
            return existing;
        if (!directory_check (existing, error) ||
            (destination && !supported_fd (existing->fd, TRUE, error)))
            return NULL;
        existing->destination |= destination;
        if (initial && !existing->root) {
            directory_cache_remove (existing);
            existing->root = TRUE;
            g_ptr_array_add (guard->roots, existing);
        }
        return existing;
    }
    g_autofree char *path = g_file_get_path (file);
    if (!path || !g_file_is_native (file) || !supported_mount_path (path, destination)) {
        unsupported (error);
        return NULL;
    }
    TransferDirectory *ancestor = NULL;
    g_autoptr (GFile) ancestor_file = g_file_get_parent (file);
    while (ancestor_file && !ancestor) {
        ancestor = g_hash_table_lookup (guard->directory_index, ancestor_file);
        GFile *next = g_file_get_parent (ancestor_file);
        g_object_unref (ancestor_file);
        ancestor_file = next;
    }
    if (!initial && !ancestor) {
        changed (error);
        return NULL;
    }
    if (!directory_cache_room (guard, ancestor, error))
        return NULL;
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
    directory->guard = guard;
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
    g_hash_table_insert (guard->directory_index, directory->file, directory);
    if (initial)
        g_ptr_array_add (guard->roots, directory);
    directory_cache_touch (directory);
    return directory;
}

NemoTransferGuard *
nemo_transfer_guard_new (GList *sources, GFile *destination, gboolean move)
{
    NemoTransferGuard *guard = g_new0 (NemoTransferGuard, 1);
    guard->refs = 1;
    guard->move = move;
    guard->directories = g_ptr_array_new_with_free_func (directory_free);
    guard->roots = g_ptr_array_new ();
    guard->directory_index = g_hash_table_new (g_file_hash, (GEqualFunc) g_file_equal);
    struct rlimit limit;
    guard->cache_limit = getrlimit (RLIMIT_NOFILE, &limit) == 0 ?
                         CLAMP (limit.rlim_cur / 8, 4, 32) : 16;
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
            if (g_file_has_uri_scheme (source, "trash"))
                g_set_error_literal (&guard->error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                     _("Safe Trash restore is unavailable. The original Trash item "
                                       "was retained. Use Copy to a supported local destination instead, "
                                       "and review the copied files before emptying Trash."));
            else
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
    g_hash_table_unref (guard->directory_index);
    g_ptr_array_unref (guard->roots);
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
    for (guint i = 0; i < guard->roots->len; i++) {
        TransferDirectory *directory = g_ptr_array_index (guard->roots, i);
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
            directory_cache_remove (directory);
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

typedef struct {
    NemoTransferGuard *guard;
    TransferDirectory *directory;
} DirectoryLease;

static void
directory_lease_free (gpointer data)
{
    DirectoryLease *lease = data;
    directory_unpin (lease->directory);
    nemo_transfer_guard_unref (lease->guard);
    g_free (lease);
}

static GFile *
directory_file (TransferDirectory *directory, const char *name)
{
    GFile *file = file_at (directory->fd, name);
    DirectoryLease *lease = g_new (DirectoryLease, 1);
    lease->guard = nemo_transfer_guard_ref (directory->guard);
    lease->directory = directory_hold (directory);
    g_object_set_data_full (G_OBJECT (file), "nemo-transfer-directory-lease",
                            lease, directory_lease_free);
    return file;
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
    return directory_file (directory, name);
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
    return directory ? directory_file (directory, ".") : NULL;
}

static TransferDirectory *
recovery_parent (NemoTransferGuard *guard, TransferDirectory *parent, gboolean destination)
{
    TransferDirectory *root = parent;
    for (guint i = 0; i < guard->roots->len; i++) {
        TransferDirectory *candidate = g_ptr_array_index (guard->roots, i);
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
                         _("\nTransaction records are retained in these recovery folders:"));
        g_hash_table_iter_init (&iter, guard->record_parents);
        while (g_hash_table_iter_next (&iter, &parent, NULL)) {
            g_autofree char *path = g_file_get_parse_name (parent);
            g_string_append_printf (guard->details, "\n%s", path);
        }
        g_string_append (guard->details,
                         _("\nSuccessful moves remove the captured source data. Do not remove folders containing "
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
    return g_build_filename (recovery->parent->path, recovery->container_name,
                             recovery->bucket_name, recovery->name, NULL);
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
write_all (int fd, const char *data, gsize length, GError **error);

static gboolean
recovery_owned_snapshot (const TransferSnapshot *expected, const TransferSnapshot *current)
{
    return snapshot_matches (expected, current) &&
           expected->stat.st_ctim.tv_sec == current->stat.st_ctim.tv_sec &&
           expected->stat.st_ctim.tv_nsec == current->stat.st_ctim.tv_nsec &&
           expected->stat.st_nlink == current->stat.st_nlink;
}

static gboolean
recovery_mount_path (TransferRecovery *recovery, const char *bucket, const char *transaction,
                     GError **error)
{
    g_autofree char *path = g_build_filename (recovery->parent->path,
                                             recovery->container_name, bucket, transaction, NULL);
    return supported_mount_path (path, recovery->parent->destination) || unsupported (error);
}

static gboolean
recovery_same_mount (TransferRecovery *recovery, int fd, GError **error)
{
    struct statx identity;
    if (!directory_identity (fd, &identity, error))
        return FALSE;
    if (identity.stx_mnt_id != recovery->parent->identity.stx_mnt_id) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                             _("Recovery storage crosses a mount boundary. "
                               "The operation was stopped without using that storage."));
        return FALSE;
    }
    return supported_fd (fd, recovery->parent->destination, error);
}

static gboolean
recovery_marker_unrecognized (TransferRecovery *recovery, GError **error)
{
    g_autoptr (GFile) file = g_file_get_child (recovery->parent->file, recovery->container_name);
    g_autofree char *path = g_file_get_parse_name (file);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                 _("Recovery storage at \"%s\" has no recognized owner record or is not owner-controlled. "
                   "It was left unchanged. Wait for any other transfer to finish, "
                   "then inspect this folder or choose another location."), path);
    return FALSE;
}

static gboolean
recovery_namespace_marker_read (TransferRecovery *recovery, int folder,
                                const char *prefix, gboolean has_token,
                                TransferSnapshot *marker, GCancellable *cancel, GError **error)
{
    gsize length = strlen (prefix) + (has_token ? 36 + 1 : 0);
    g_autofree char *contents = g_malloc0 (length + 1);
    int fd = openat (folder, "owner", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT || errno == ELOOP ? recovery_marker_unrecognized (recovery, error) :
               transfer_error (error, _("Could not inspect the recovery storage owner record"));
    gboolean ok = fstat (fd, &marker->stat) == 0;
    if (!ok)
        transfer_error (error, _("Could not inspect the recovery storage owner record"));
    if (ok && (!S_ISREG (marker->stat.st_mode) || marker->stat.st_uid != geteuid () ||
               marker->stat.st_nlink != 1 || (marker->stat.st_mode & (S_IWGRP | S_IWOTH)) ||
               marker->stat.st_size != (off_t) length))
        ok = recovery_marker_unrecognized (recovery, error);
    gsize count = 0;
    while (ok && count < length) {
        if (g_cancellable_set_error_if_cancelled (cancel, error)) {
            ok = FALSE;
            break;
        }
        ssize_t got = read (fd, contents + count, length - count);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0)
            ok = transfer_error (error, _("Could not read the recovery storage owner record"));
        else if (!got)
            ok = recovery_marker_unrecognized (recovery, error);
        else
            count += got;
    }
    if (ok && memcmp (contents, prefix, strlen (prefix)))
        ok = recovery_marker_unrecognized (recovery, error);
    if (ok && has_token) {
        g_autofree char *token = g_strndup (contents + strlen (prefix), 36);
        if (contents[length - 1] != '\n' || !g_uuid_string_is_valid (token))
            ok = recovery_marker_unrecognized (recovery, error);
    }
    struct stat after, named;
    if (ok && (fstat (fd, &after) < 0 ||
               fstatat (folder, "owner", &named, AT_SYMLINK_NOFOLLOW) < 0 ||
               !same_contents_metadata (&marker->stat, &after) ||
               marker->stat.st_ctim.tv_sec != after.st_ctim.tv_sec ||
               marker->stat.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
               marker->stat.st_nlink != after.st_nlink || !same_inode (&after, &named)))
        ok = changed (error);
    if (!close_checked (fd, ok ? error : NULL))
        ok = FALSE;
    if (ok)
        marker->checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *) contents, length);
    return ok;
}

static gboolean
recovery_marker_read (TransferRecovery *recovery, TransferSnapshot *marker,
                      GCancellable *cancel, GError **error)
{
    g_autofree char *prefix = g_strdup_printf ("Nemo recovery storage\nversion=1\nowner=%"
                                             G_GUINT64_FORMAT "\ntoken=", (guint64) geteuid ());
    return recovery_namespace_marker_read (recovery, recovery->container_fd, prefix, TRUE,
                                           marker, cancel, error);
}

static gboolean
recovery_container_check (TransferRecovery *recovery, GError **error)
{
    struct stat current, named;
    if (!directory_check (recovery->parent, error) ||
        !recovery_mount_path (recovery, NULL, NULL, error) ||
        !recovery_same_mount (recovery, recovery->container_fd, error))
        return FALSE;
    if (fstat (recovery->container_fd, &current) < 0 ||
        fstatat (recovery->parent->fd, recovery->container_name, &named, AT_SYMLINK_NOFOLLOW) < 0)
        return transfer_error (error, _("Could not confirm the recovery storage location"));
    if (!same_inode (&recovery->container_identity, &current) || !same_inode (&current, &named) ||
        current.st_uid != geteuid () || (current.st_mode & (S_IWGRP | S_IWOTH)))
        return changed (error);
    TransferSnapshot marker = { 0 };
    gboolean ok = recovery_marker_read (recovery, &marker, NULL, error);
    if (ok && !recovery_owned_snapshot (&recovery->marker, &marker))
        ok = changed (error);
    snapshot_clear (&marker);
    return ok;
}

static gboolean
recovery_container_open (TransferRecovery *recovery, gboolean create,
                         GCancellable *cancel, GError **error)
{
    if (recovery->container_fd >= 0) {
        int fd = recovery->container_fd;
        recovery->container_fd = -1;
        if (!close_checked (fd, error))
            return FALSE;
    }
    if (!recovery->parent_held) {
        directory_hold (recovery->parent);
        recovery->parent_held = TRUE;
    }
    if (!directory_check (recovery->parent, error) ||
        !recovery_mount_path (recovery, NULL, NULL, error))
        return FALSE;
    gboolean created = FALSE;
    if (create) {
        if (mkdirat (recovery->parent->fd, recovery->container_name, 0700) == 0)
            created = TRUE;
        else if (errno != EEXIST)
            return transfer_error (error, _("Could not create private recovery storage"));
    }
    recovery->container_fd = openat (recovery->parent->fd, recovery->container_name,
                                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (recovery->container_fd < 0)
        return errno == ENOTDIR || errno == ELOOP ? recovery_marker_unrecognized (recovery, error) :
               transfer_error (error, _("Could not pin private recovery storage"));
    if (!recovery_same_mount (recovery, recovery->container_fd, error))
        return FALSE;
    struct stat current;
    if (fstat (recovery->container_fd, &current) < 0)
        return transfer_error (error, _("Could not inspect private recovery storage"));
    if (current.st_uid != geteuid () || (current.st_mode & (S_IWGRP | S_IWOTH)))
        return recovery_marker_unrecognized (recovery, error);
    if (!create && !same_inode (&recovery->container_identity, &current))
        return changed (error);
    g_autofree char *checksum = NULL;
    struct stat created_marker = { 0 };
    if (created) {
        g_autofree char *token = g_uuid_string_random ();
        g_autofree char *contents = g_strdup_printf ("Nemo recovery storage\nversion=1\nowner=%"
                                                   G_GUINT64_FORMAT "\ntoken=%s\n",
                                                   (guint64) geteuid (), token);
        checksum = g_compute_checksum_for_string (G_CHECKSUM_SHA256, contents, -1);
        int fd = openat (recovery->container_fd, "owner",
                         O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            return transfer_error (error, _("Could not create the recovery storage owner record"));
        gboolean ok = write_all (fd, contents, strlen (contents), error) &&
                      sync_fd (fd, cancel, error);
        if (ok && fstat (fd, &created_marker) < 0)
            ok = transfer_error (error, _("Could not identify the recovery storage owner record"));
        if (!close_checked (fd, ok ? error : NULL))
            ok = FALSE;
        if (!ok)
            return FALSE;
    }
    TransferSnapshot marker = { 0 };
    gboolean ok = recovery_marker_read (recovery, &marker, cancel, error);
    if (ok && ((created && (g_strcmp0 (marker.checksum, checksum) != 0 ||
                            !same_contents_metadata (&created_marker, &marker.stat) ||
                            created_marker.st_ctim.tv_sec != marker.stat.st_ctim.tv_sec ||
                            created_marker.st_ctim.tv_nsec != marker.stat.st_ctim.tv_nsec)) ||
               (!create && !recovery_owned_snapshot (&recovery->marker, &marker))))
        ok = changed (error);
    if (ok && create) {
        recovery->container_identity = current;
        recovery->marker = marker;
        memset (&marker, 0, sizeof marker);
    }
    snapshot_clear (&marker);
    if (!ok || !recovery_container_check (recovery, error))
        return FALSE;
    return !created || (sync_fd (recovery->container_fd, cancel, error) &&
                        sync_fd (recovery->parent->fd, cancel, error));
}

static gboolean
recovery_bucket_marker_read (TransferRecovery *recovery, TransferSnapshot *marker,
                             GCancellable *cancel, GError **error)
{
    g_autofree char *contents = g_strdup_printf ("Nemo recovery bucket\nversion=1\nowner=%"
                                               G_GUINT64_FORMAT "\ncontainer-sha256=%s\nbucket=%s\n",
                                               (guint64) geteuid (), recovery->marker.checksum,
                                               recovery->bucket_name);
    gboolean ok = recovery_namespace_marker_read (recovery, recovery->bucket_fd, contents, FALSE,
                                                  marker, cancel, error);
    if (!ok)
        g_prefix_error (error, _("Could not validate recovery bucket %s: "), recovery->bucket_name);
    return ok;
}

static gboolean
recovery_bucket_check (TransferRecovery *recovery, GError **error)
{
    if (!recovery_container_check (recovery, error) ||
        !recovery_mount_path (recovery, recovery->bucket_name, NULL, error) ||
        !recovery_same_mount (recovery, recovery->bucket_fd, error))
        return FALSE;
    struct stat current, named;
    if (fstat (recovery->bucket_fd, &current) < 0 ||
        fstatat (recovery->container_fd, recovery->bucket_name, &named, AT_SYMLINK_NOFOLLOW) < 0)
        return transfer_error (error, _("Could not confirm the recovery bucket location"));
    if (!same_inode (&recovery->bucket_identity, &current) || !same_inode (&current, &named) ||
        current.st_uid != geteuid () || (current.st_mode & (S_IWGRP | S_IWOTH)))
        return changed (error);
    TransferSnapshot marker = { 0 };
    gboolean ok = recovery_bucket_marker_read (recovery, &marker, NULL, error);
    if (ok && !recovery_owned_snapshot (&recovery->bucket_marker, &marker))
        ok = changed (error);
    snapshot_clear (&marker);
    return ok;
}

static gboolean
recovery_bucket_open (TransferRecovery *recovery, gboolean create,
                      GCancellable *cancel, GError **error)
{
    if (recovery->bucket_fd >= 0) {
        int fd = recovery->bucket_fd;
        recovery->bucket_fd = -1;
        if (!close_checked (fd, error))
            return FALSE;
    }
    if (!recovery_container_check (recovery, error) ||
        !recovery_mount_path (recovery, recovery->bucket_name, NULL, error))
        return FALSE;
    gboolean created = FALSE;
    if (create) {
        if (mkdirat (recovery->container_fd, recovery->bucket_name, 0700) == 0)
            created = TRUE;
        else if (errno != EEXIST)
            return transfer_error (error, _("Could not create a private recovery bucket"));
    }
    recovery->bucket_fd = openat (recovery->container_fd, recovery->bucket_name,
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (recovery->bucket_fd < 0)
        return errno == ENOTDIR || errno == ELOOP ? recovery_marker_unrecognized (recovery, error) :
               transfer_error (error, _("Could not pin the private recovery bucket"));
    if (!recovery_same_mount (recovery, recovery->bucket_fd, error))
        return FALSE;
    struct stat current;
    if (fstat (recovery->bucket_fd, &current) < 0)
        return transfer_error (error, _("Could not inspect the private recovery bucket"));
    if (current.st_uid != geteuid () || (current.st_mode & (S_IWGRP | S_IWOTH)))
        return recovery_marker_unrecognized (recovery, error);
    if (!create && !same_inode (&recovery->bucket_identity, &current))
        return changed (error);
    struct stat created_marker = { 0 };
    if (created) {
        g_autofree char *contents = g_strdup_printf ("Nemo recovery bucket\nversion=1\nowner=%"
                                                   G_GUINT64_FORMAT "\ncontainer-sha256=%s\nbucket=%s\n",
                                                   (guint64) geteuid (), recovery->marker.checksum,
                                                   recovery->bucket_name);
        int fd = openat (recovery->bucket_fd, "owner",
                         O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            return transfer_error (error, _("Could not create the recovery bucket owner record"));
        gboolean ok = write_all (fd, contents, strlen (contents), error) && sync_fd (fd, cancel, error);
        if (ok && fstat (fd, &created_marker) < 0)
            ok = transfer_error (error, _("Could not identify the recovery bucket owner record"));
        if (!close_checked (fd, ok ? error : NULL))
            ok = FALSE;
        if (!ok)
            return FALSE;
    }
    TransferSnapshot marker = { 0 };
    gboolean ok = recovery_bucket_marker_read (recovery, &marker, cancel, error);
    if (ok && ((created && (!same_contents_metadata (&created_marker, &marker.stat) ||
                            created_marker.st_ctim.tv_sec != marker.stat.st_ctim.tv_sec ||
                            created_marker.st_ctim.tv_nsec != marker.stat.st_ctim.tv_nsec)) ||
               (!create && !recovery_owned_snapshot (&recovery->bucket_marker, &marker))))
        ok = changed (error);
    if (ok && create) {
        recovery->bucket_identity = current;
        recovery->bucket_marker = marker;
        memset (&marker, 0, sizeof marker);
    }
    snapshot_clear (&marker);
    if (!ok || !recovery_bucket_check (recovery, error))
        return FALSE;
    return !created || (sync_fd (recovery->bucket_fd, cancel, error) &&
                        sync_fd (recovery->container_fd, cancel, error));
}

static gboolean
recovery_new_in_bucket (TransferDirectory *parent, TransferRecovery *recovery,
                        const char *bucket_name, GCancellable *cancel, GError **error)
{
    memset (recovery, 0, sizeof *recovery);
    recovery->fd = -1;
    recovery->container_fd = -1;
    recovery->bucket_fd = -1;
    recovery->parent = parent;
    recovery->container_name = g_strdup_printf (".nemo-recovery-%" G_GUINT64_FORMAT, (guint64) geteuid ());
    recovery->bucket_name = g_strdup (bucket_name);
    recovery->records = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, snapshot_free);
    if (!recovery_container_open (recovery, TRUE, cancel, error))
        return FALSE;
    for (guint i = 0; i < 16; i++) {
        g_autofree char *uuid = g_uuid_string_random ();
        if (!recovery->bucket_name)
            recovery->bucket_name = g_strndup (uuid, 2);
        if (recovery->bucket_fd < 0 && !recovery_bucket_open (recovery, TRUE, cancel, error))
            return FALSE;
        uuid[0] = recovery->bucket_name[0];
        uuid[1] = recovery->bucket_name[1];
        g_free (recovery->name);
        recovery->name = g_strconcat ("transaction-", uuid, NULL);
        if (mkdirat (recovery->bucket_fd, recovery->name, 0700) == 0)
            break;
        if (errno != EEXIST || i == 15)
            return transfer_error (error, _("Could not create private recovery storage"));
    }
    if (!recovery_mount_path (recovery, recovery->bucket_name, recovery->name, error))
        return FALSE;
    recovery->fd = openat (recovery->bucket_fd, recovery->name,
                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (recovery->fd < 0)
        return transfer_error (error, _("Could not pin private recovery storage"));
    if (!recovery_same_mount (recovery, recovery->fd, error))
        return FALSE;
    if (fstat (recovery->fd, &recovery->identity) < 0)
        return transfer_error (error, _("Could not inspect private recovery storage"));
    if (recovery->identity.st_uid != geteuid () ||
        (recovery->identity.st_mode & (S_IWGRP | S_IWOTH))) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             _("This filesystem cannot isolate recovery entries from other writers."));
        return FALSE;
    }
    g_autoptr (GFile) container = g_file_get_child (parent->file, recovery->container_name);
    g_autoptr (GFile) bucket = g_file_get_child (container, recovery->bucket_name);
    recovery->file = g_file_get_child (bucket, recovery->name);
    return sync_fd (recovery->fd, cancel, error) &&
           sync_fd (recovery->bucket_fd, cancel, error) &&
           sync_fd (recovery->container_fd, cancel, error) && sync_fd (parent->fd, cancel, error);
}

static gboolean
recovery_new (TransferDirectory *parent, TransferRecovery *recovery,
              GCancellable *cancel, GError **error)
{
    return recovery_new_in_bucket (parent, recovery, NULL, cancel, error);
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
    if (((recovery->identity.st_mode | recovery->container_identity.st_mode |
          recovery->bucket_identity.st_mode) & (S_IRWXG | S_IRWXO)) &&
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
    g_hash_table_add (transaction->guard->record_parents, g_object_ref (recovery->file));
    gboolean ok = write_all (fd, data, length, error) && sync_fd (fd, cancel, error);
    TransferSnapshot *owned = g_new0 (TransferSnapshot, 1);
    if (ok && fstat (fd, &owned->stat) < 0)
        ok = transfer_error (error, _("Could not identify the recovery record"));
    if (ok)
        owned->checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *) data, length);
    if (!close_checked (fd, ok ? error : NULL))
        ok = FALSE;
    if (ok)
        g_hash_table_insert (recovery->records, g_strdup (name), owned);
    else
        snapshot_free (owned);
    return ok && sync_fd (recovery->fd, cancel, error) &&
           sync_fd (recovery->bucket_fd, cancel, error) &&
           sync_fd (recovery->container_fd, cancel, error) &&
           sync_fd (recovery->parent->fd, cancel, error);
}

static void
recovery_close (TransferRecovery *recovery)
{
    if (recovery->fd >= 0)
        close (recovery->fd);
    if (recovery->bucket_name && recovery->bucket_fd >= 0)
        close (recovery->bucket_fd);
    if (recovery->container_name && recovery->container_fd >= 0)
        close (recovery->container_fd);
    recovery->fd = recovery->bucket_fd = recovery->container_fd = -1;
    g_clear_object (&recovery->file);
    g_clear_pointer (&recovery->name, g_free);
    g_clear_pointer (&recovery->container_name, g_free);
    g_clear_pointer (&recovery->bucket_name, g_free);
    g_clear_pointer (&recovery->records, g_hash_table_unref);
    snapshot_clear (&recovery->marker);
    snapshot_clear (&recovery->bucket_marker);
    if (recovery->parent_held) {
        directory_unpin (recovery->parent);
        recovery->parent_held = FALSE;
    }
}

static gboolean
recovery_finish (TransferRecovery *recovery, GError **error)
{
    gboolean ok = TRUE;
    if (recovery->fd >= 0) {
        int fd = recovery->fd;
        recovery->fd = -1;
        ok = close_checked (fd, error);
    }
    if (recovery->bucket_name && recovery->bucket_fd >= 0) {
        int fd = recovery->bucket_fd;
        recovery->bucket_fd = -1;
        if (!close_checked (fd, ok ? error : NULL))
            ok = FALSE;
    }
    if (recovery->container_name && recovery->container_fd >= 0) {
        int fd = recovery->container_fd;
        recovery->container_fd = -1;
        if (!close_checked (fd, ok ? error : NULL))
            ok = FALSE;
    }
    if (recovery->parent_held) {
        directory_unpin (recovery->parent);
        recovery->parent_held = FALSE;
    }
    return ok;
}

static gboolean
recovery_identity_check (TransferRecovery *recovery, GError **error)
{
    struct stat current, named;
    if (!recovery_bucket_check (recovery, error) ||
        !recovery_mount_path (recovery, recovery->bucket_name, recovery->name, error) ||
        !recovery_same_mount (recovery, recovery->fd, error))
        return FALSE;
    if (fstat (recovery->fd, &current) < 0 ||
        fstatat (recovery->bucket_fd, recovery->name, &named, AT_SYMLINK_NOFOLLOW) < 0)
        return transfer_error (error, _("Could not confirm the recovery folder location"));
    if (!same_inode (&recovery->identity, &current) || !same_inode (&current, &named) ||
        current.st_uid != geteuid () || (current.st_mode & (S_IWGRP | S_IWOTH)))
        return changed (error);
    return TRUE;
}

static gboolean
recovery_cleanup (NemoTransferGuard *guard, TransferRecovery *recovery, GError **error);

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
    if (recovery_finish (&transaction->recovery, error)) {
        if (!transaction->publication_uncertain && !transaction->backup &&
            (!transaction->guard->move || transaction->native_source))
            return recovery_cleanup (transaction->guard, &transaction->recovery, error);
        return TRUE;
    }
    report_recovery (transaction->guard, &transaction->recovery,
                     _("The published transaction's folder handle could not be closed successfully."));
    return FALSE;
}

static gboolean
recovery_reopen (TransferRecovery *recovery, GError **error)
{
    if (recovery->fd >= 0)
        return TRUE;
    if (!recovery_container_open (recovery, FALSE, NULL, error) ||
        !recovery_bucket_open (recovery, FALSE, NULL, error))
        return FALSE;
    if (recovery->cleaned)
        return TRUE;
    if (!recovery_mount_path (recovery, recovery->bucket_name, recovery->name, error))
        return FALSE;
    int fd = openat (recovery->bucket_fd, recovery->name,
                     O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return transfer_error (error, _("Could not reopen the retained recovery folder"));
    if (!recovery_same_mount (recovery, fd, error)) {
        close (fd);
        return FALSE;
    }
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
recovery_cleanup (NemoTransferGuard *guard, TransferRecovery *recovery, GError **error)
{
    if (recovery->cleaned)
        return TRUE;
    gboolean ok = recovery_reopen (recovery, error) && recovery_identity_check (recovery, error);
    gboolean metadata_only = TRUE;
    guint count = 0;
    DIR *entries = NULL;
    if (ok) {
        int fd = openat (recovery->fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0 || !(entries = fdopendir (fd))) {
            if (fd >= 0)
                close (fd);
            ok = transfer_error (error, _("Could not inspect completed recovery storage"));
        }
    }
    while (ok && entries) {
        errno = 0;
        struct dirent *entry = readdir (entries);
        if (!entry) {
            if (errno)
                ok = transfer_error (error, _("Could not read completed recovery storage"));
            break;
        }
        if (!strcmp (entry->d_name, ".") || !strcmp (entry->d_name, ".."))
            continue;
        TransferSnapshot *owned = g_hash_table_lookup (recovery->records, entry->d_name);
        if (!owned) {
            metadata_only = FALSE;
            continue;
        }
        TransferSnapshot current = { 0 };
        ok = snapshot_at (recovery->fd, entry->d_name, &current, TRUE, FALSE, NULL, error);
        if (ok && !recovery_owned_snapshot (owned, &current))
            metadata_only = FALSE;
        snapshot_clear (&current);
        count++;
    }
    if (entries && closedir (entries) < 0) {
        if (ok)
            transfer_error (error, _("Could not close completed recovery storage enumeration"));
        ok = FALSE;
    }
    if (count != g_hash_table_size (recovery->records))
        metadata_only = FALSE;
    if (ok && !metadata_only) {
        report_recovery (guard, recovery,
                         _("Recovery storage contains retained data or changed entries and was not removed."));
        return recovery_finish (recovery, error);
    }
    if (ok)
        ok = recovery_identity_check (recovery, error) &&
             check_directory_close (recovery->fd, error) &&
             check_directory_close (recovery->bucket_fd, error) &&
             check_directory_close (recovery->container_fd, error);
    GHashTableIter iter;
    gpointer name, value;
    g_hash_table_iter_init (&iter, recovery->records);
    while (ok && g_hash_table_iter_next (&iter, &name, &value)) {
        TransferSnapshot current = { 0 };
        ok = snapshot_at (recovery->fd, name, &current, TRUE, FALSE, NULL, error);
        if (ok && !recovery_owned_snapshot (value, &current))
            ok = changed (error);
        snapshot_clear (&current);
        if (ok && unlinkat (recovery->fd, name, 0) < 0)
            ok = transfer_error (error, _("Could not remove a completed transaction record"));
    }
    if (ok)
        ok = sync_fd (recovery->fd, NULL, error) && recovery_identity_check (recovery, error);
    if (recovery->fd >= 0) {
        int fd = recovery->fd;
        recovery->fd = -1;
        if (!close_checked (fd, ok ? error : NULL))
            ok = FALSE;
    }
    /* The public parent is never unlinked. This name is inside a validated,
     * owner-controlled container, and AT_REMOVEDIR preserves every new child. */
    if (ok && unlinkat (recovery->bucket_fd, recovery->name, AT_REMOVEDIR) < 0)
        ok = transfer_error (error, _("Could not remove completed private recovery storage"));
    if (ok) {
        recovery->cleaned = TRUE;
        g_hash_table_remove (guard->record_parents, recovery->file);
        g_hash_table_remove_all (recovery->records);
        ok = sync_fd (recovery->bucket_fd, NULL, error) &&
             sync_fd (recovery->container_fd, NULL, error);
    }
    if (!recovery_finish (recovery, ok ? error : NULL))
        ok = FALSE;
    if (!ok) {
        if (recovery->cleaned)
            g_string_append (guard->details,
                             _("\nThe transfer completed, but removal of its metadata-only recovery "
                               "folder could not be confirmed durable."));
        else
            report_recovery (guard, recovery,
                             _("Completed recovery storage could not be cleaned safely."));
    }
    return ok;
}

static gboolean
recovery_prepare_undo (TransferRecovery *recovery, GCancellable *cancel, GError **error)
{
    if (!recovery->cleaned)
        return recovery_reopen (recovery, error);
    TransferDirectory *parent = recovery->parent;
    g_autofree char *bucket_name = g_strdup (recovery->bucket_name);
    recovery_close (recovery);
    return recovery_new_in_bucket (parent, recovery, bucket_name, cancel, error);
}

static gboolean
unsupported_replacement (GError **error)
{
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                         _("Safe replacement is not supported by this filesystem. "
                           "Choose another name; existing files were not replaced."));
    return FALSE;
}

static void
report_replacement_error (NemoTransferTransaction *transaction, GError *error)
{
    transaction->guard->replacement_unsupported = TRUE;
    g_autofree char *destination = g_file_get_parse_name (transaction->destination);
    g_string_append_printf (transaction->guard->details, "\n%s\n%s",
                            destination, error->message);
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
    if (flags == RENAME_EXCHANGE &&
        (errno == EINVAL || errno == EOPNOTSUPP || errno == ENOSYS))
        return unsupported_replacement (error);
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

gboolean
nemo_transfer_guard_can_copy_fallback (NemoTransferGuard *guard)
{
    return !guard->replacement_unsupported;
}

static gboolean
probe_exchange (NemoTransferTransaction *transaction, GError **error)
{
    TransferDirectory *root = transaction->recovery.parent;
    if (root->exchange_capability == TRANSFER_EXCHANGE_SUPPORTED)
        return TRUE;
    const char *names[] = { "probe-a", "probe-b" };
    struct stat identities[2];
    int parent = transaction->recovery.fd;
    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        int fd = openat (parent, names[i],
                         O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            return transfer_error (error, _("Could not create the safe replacement probe"));
        gboolean ok = fstat (fd, &identities[i]) == 0;
        if (!ok)
            transfer_error (error, _("Could not identify the safe replacement probe"));
        if (!close_checked (fd, ok ? error : NULL))
            ok = FALSE;
        if (!ok)
            return FALSE;
    }
    if (!rename_entry (parent, names[0], parent, names[1], RENAME_EXCHANGE, error)) {
        if (g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
            root->exchange_capability = TRANSFER_EXCHANGE_UNSUPPORTED;
            report_replacement_error (transaction, *error);
        }
        return FALSE;
    }
    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        struct stat current;
        if (fstatat (parent, names[i], &current, AT_SYMLINK_NOFOLLOW) < 0 ||
            !same_contents_metadata (&identities[1 - i], &current))
            return changed (error);
        if (unlinkat (parent, names[i], 0) < 0)
            return transfer_error (error, _("Could not remove the owned replacement probe"));
    }
    root->exchange_capability = TRANSFER_EXCHANGE_SUPPORTED;
    return TRUE;
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
    TransferDirectory *root = recovery_parent (guard, transaction->destination_parent, TRUE);
    gboolean replacement = g_hash_table_contains (guard->expected, destination);
    if (replacement && root->exchange_capability == TRANSFER_EXCHANGE_UNSUPPORTED) {
        unsupported_replacement (error);
        report_replacement_error (transaction, *error);
        goto failed;
    }
    if (!recovery_new (root, &transaction->recovery, cancel, error))
        goto failed;
    transaction->stage = file_at (transaction->recovery.fd, "payload");
    if (!recovery_record (transaction, &transaction->recovery, "prepared", cancel, error))
        goto failed;
    if (replacement && !probe_exchange (transaction, error))
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
    if (overwrite && expected &&
        g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        report_replacement_error (transaction, *error);
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
        !recovery_identity_check (&capture, error) ||
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
    if (ok)
        ok = recovery_cleanup (transaction->guard, &capture, error);
    if (ok && !transaction->backup)
        ok = recovery_cleanup (transaction->guard, &transaction->recovery, error);
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
    g_autoptr (TransferDirectory) parent_pin = directory_hold (parent);
    TransferDirectory *directory = guard_directory (guard, source, FALSE, FALSE, error);
    if (!directory)
        return FALSE;
    g_autoptr (TransferDirectory) directory_pin = directory_hold (directory);
    g_autofree char *name = g_file_get_basename (source);
    TransferRecovery capture = { .fd = -1 };
    NemoTransferTransaction record = { .guard = guard, .source = source, .destination = source };
    gboolean captured = FALSE, ok = FALSE;
    if (!recovery_new (recovery_parent (guard, parent, FALSE), &capture, cancel, error) ||
        !snapshot_at (parent->fd, name, &record.source_before, FALSE, FALSE, cancel, error) ||
        !recovery_record (&record, &capture, "source-directory-prepared", cancel, error) ||
        !recovery_identity_check (&capture, error) ||
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
               STATX_BASIC_STATS | STATX_BTIME | STATX_MNT_ID | STATX_MNT_ID_UNIQUE, &current) < 0) {
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
    if (ok)
        ok = recovery_cleanup (guard, &capture, error);
    recovery_close (&capture);
    snapshot_clear (&record.source_before);
    return ok;
}

void
nemo_transfer_transaction_free (NemoTransferTransaction *transaction)
{
    if (!transaction)
        return;
    g_clear_object (&transaction->source_file);
    g_clear_object (&transaction->destination_file);
    if (!g_atomic_int_dec_and_test (&transaction->refs)) {
        /* Undo retains identity and recovery data, not one open descriptor per
         * copied photo. Reopening later must match the recorded directory. */
        recovery_finish (&transaction->recovery, NULL);
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
    guard->replacement_unsupported = FALSE;
    if (!nemo_transfer_guard_check (guard, error))
        return FALSE;
    g_autoptr (GFile) source_parent_file = g_file_get_parent (source);
    g_autoptr (GFile) destination_parent_file = g_file_get_parent (destination);
    TransferDirectory *source_parent = guard_directory (guard, source_parent_file, FALSE, FALSE, error);
    if (!source_parent)
        return FALSE;
    g_autoptr (TransferDirectory) source_pin = directory_hold (source_parent);
    TransferDirectory *destination_parent = guard_directory (guard, destination_parent_file, TRUE, FALSE, error);
    if (!destination_parent)
        return FALSE;
    g_autoptr (TransferDirectory) destination_pin = directory_hold (destination_parent);
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
    NemoTransferTransaction *transaction = nemo_transfer_transaction_new (guard, source, destination, cancel, error);
    if (!transaction)
        return FALSE;
    gboolean captured = FALSE, ok = FALSE;
    TransferSnapshot captured_snapshot = { 0 };
    transaction->native_source = TRUE;
    if (!sync_tree (source_parent->fd, source_name, source_parent->identity.stx_mnt_id, cancel, error) ||
        !sync_fd (source_parent->fd, cancel, error))
        goto out;
    if (!same_contents_metadata (&source_stat, &transaction->source_before.stat)) {
        changed (error);
        goto out;
    }
    if (!directory_check (source_parent, error) ||
        !recovery_identity_check (&transaction->recovery, error) ||
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
    gboolean ok = recovery_finish (recovery, error);
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
    g_autoptr (TransferDirectory) source_pin = directory_hold (transaction->source_parent);
    g_autoptr (TransferDirectory) destination_pin = directory_hold (transaction->destination_parent);
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
    if (!recovery_reopen (&transaction->recovery, error)) {
        recovery_finish (&transaction->recovery, NULL);
        g_prefix_error (error,
                        _("Undo or redo could not validate its recovery storage before changing this item. "
                          "Use Copy to restore files manually if this storage is no longer available. "));
        return FALSE;
    }
    if (!transaction->recovery.cleaned &&
        !recovery_identity_check (&transaction->recovery, error)) {
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
    g_autoptr (TransferDirectory) source_pin = directory_hold (transaction->source_parent);
    g_autoptr (TransferDirectory) destination_pin = directory_hold (transaction->destination_parent);
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
    if (!recovery_prepare_undo (&transaction->recovery, cancel, error))
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
    if (ok && ((!redo && undo->move) || (redo && !transaction->backup)))
        ok = recovery_cleanup (transaction->guard, &transaction->recovery, error);
    if (ok)
        undo->undone = !redo;
    if (ok && !redo && !undo->move)
        report_recovery (transaction->guard, &transaction->recovery,
                         _("The undone copy was retained for guarded redo."));
retained:
    if (!ok) {
        if (transaction->recovery.cleaned) {
            nemo_transfer_guard_describe_destination (transaction->guard, transaction->destination);
            g_autofree char *source = g_file_get_parse_name (transaction->source);
            g_string_append_printf (transaction->guard->details,
                                    _("\nUndo or redo changed file names, but metadata cleanup was not "
                                      "confirmed. No recovery payload remained in the removed folder. "
                                      "Also inspect the source location: %s"), source);
        } else {
            report_recovery (transaction->guard, &transaction->recovery,
                             _("Undo or redo was incomplete. Captured and prior destination entries were retained."));
        }
    }
    recovery_finish (&transaction->recovery, NULL);
    return ok;
}

#endif
