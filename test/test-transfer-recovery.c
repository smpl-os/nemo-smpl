/* Direct helper contracts: no UI, D-Bus, GVfs, or external filesystem fixtures. */
#define _GNU_SOURCE
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gunixmounts.h>
#include <glib/gstdio.h>
#include <libnemo-private/nemo-mount-operation.h>
#include <libnemo-private/nemo-transfer-safety.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#define BATCH_SIZE 256
#define QUEUED_ROOTS 1500

typedef struct {
    char *path;
    char *sources;
    char *destination;
    char *recovery;
    struct stat identity;
} Fixture;

typedef struct {
    guint buckets;
    guint transactions;
    guint max_children;
} Inventory;

typedef struct {
    Fixture *fixture;
    char *public_path;
    char *private_path;
    char *winner_owner;
    guint interleavings;
    guint denied;
    guint io_faults;
    guint io_operation;
    guint cancellations;
    GCancellable *cancel_initialization;
    struct stat unknown_identity;
    gboolean observing;
    gboolean deny_publication;
    gboolean swap_loser;
} NamespaceRace;

static const char *selected_bucket;
static const char *different_mount_root;
static guint different_mount_depth;
static guint mount_fault_hits;
static char *mount_fault_path;
static NamespaceRace *namespace_race;
static gboolean observe_mount_cache;
static gboolean invalidate_mount_cache;
static guint mount_table_parses;
static guint mount_table_checks;
static guint mount_table_invalidations;
static guint mount_statx_checks;
static gboolean real_mount_cache_changes;
static gboolean freeze_mount_clock;
static gint64 mount_clock_now;
static gboolean sample_transfer_fds;
static guint sampled_fd_peak;
static GHashTable *queued_source_roots;
static const char *queued_source_prefix;

static void interleave_namespace_publication (const char *private_path);
static guint descriptor_count (void);
static void change_byte (const char *path, gsize offset);

static void
sample_descriptor_peak (void)
{
    if (sample_transfer_fds) {
        guint count = descriptor_count ();
        sampled_fd_peak = MAX (sampled_fd_peak, count);
    }
}

static char *
path_at_fd (int fd, const char *name)
{
    g_autofree char *proc = g_strdup_printf ("/proc/self/fd/%d", fd);
    g_autofree char *parent = g_file_read_link (proc, NULL);
    return parent ? g_build_filename (parent, name, NULL) : NULL;
}

int __real_mkdirat (int fd, const char *path, mode_t mode);

int
__wrap_mkdirat (int fd, const char *path, mode_t mode)
{
    int result = __real_mkdirat (fd, path, mode);
    int saved_errno = errno;
    if (namespace_race && result == 0) {
        g_autofree char *created = path_at_fd (fd, path);
        if (g_strcmp0 (created, namespace_race->public_path) == 0) {
            /* A competing job must never see a freshly published, unmarked dir. */
            g_autofree char *owner = g_build_filename (created, "owner", NULL);
            g_assert_true (g_file_test (owner, G_FILE_TEST_IS_REGULAR));
        }
        if (namespace_race->cancel_initialization && !namespace_race->cancellations) {
            g_autofree char *prefix =
                g_build_filename (namespace_race->fixture->destination, ".nemo-recovery-init-", NULL);
            if (created && g_str_has_prefix (created, prefix)) {
                namespace_race->cancellations++;
                g_cancellable_cancel (namespace_race->cancel_initialization);
            }
        }
    }
    errno = saved_errno;
    return result;
}

int __real_renameat2 (int from_fd, const char *from, int to_fd, const char *to, unsigned int flags);

int
__wrap_renameat2 (int from_fd, const char *from, int to_fd, const char *to, unsigned int flags)
{
    gboolean interleaved = FALSE;
    if (namespace_race && !namespace_race->observing) {
        g_autofree char *destination = path_at_fd (to_fd, to);
        if (g_strcmp0 (destination, namespace_race->public_path) == 0) {
            g_assert_cmpuint (flags, ==, RENAME_NOREPLACE);
            if (namespace_race->deny_publication) {
                namespace_race->denied++;
                errno = EOPNOTSUPP;
                return -1;
            }
            g_autofree char *source = path_at_fd (from_fd, from);
            g_assert_nonnull (source);
            interleave_namespace_publication (source);
            interleaved = TRUE;
        }
    }
    int result = __real_renameat2 (from_fd, from, to_fd, to, flags);
    if (interleaved) {
        g_assert_cmpint (result, ==, -1);
        g_assert_cmpint (errno, ==, EEXIST);
    }
    return result;
}

static gboolean
candidate_owner_io (int fd, guint operation)
{
    if (!namespace_race || namespace_race->io_operation != operation || namespace_race->io_faults)
        return FALSE;
    g_autofree char *path = path_at_fd (fd, "");
    g_autofree char *prefix =
        g_build_filename (namespace_race->fixture->destination, ".nemo-recovery-init-", NULL);
    if (!path || !g_str_has_prefix (path, prefix) || !g_str_has_suffix (path, "/owner"))
        return FALSE;
    namespace_race->io_faults++;
    return TRUE;
}

int __real_fsync (int fd);

int
__wrap_fsync (int fd)
{
    sample_descriptor_peak ();
    if (candidate_owner_io (fd, 1)) {
        errno = EIO;
        return -1;
    }
    return __real_fsync (fd);
}

int __real_close (int fd);

int
__wrap_close (int fd)
{
    gboolean fail = candidate_owner_io (fd, 2);
    int result = __real_close (fd);
    if (fail) {
        g_assert_cmpint (result, ==, 0);
        errno = EIO;
        return -1;
    }
    return result;
}

GList *__real_g_unix_mounts_get (guint64 *time_read);
gboolean __real_g_unix_mounts_changed_since (guint64 time_read);
gint64 __real_g_get_monotonic_time (void);

gint64
__wrap_g_get_monotonic_time (void)
{
    return observe_mount_cache && freeze_mount_clock ? mount_clock_now :
                                                      __real_g_get_monotonic_time ();
}

GList *
__wrap_g_unix_mounts_get (guint64 *time_read)
{
    if (observe_mount_cache)
        mount_table_parses++;
    return __real_g_unix_mounts_get (time_read);
}

gboolean
__wrap_g_unix_mounts_changed_since (guint64 time_read)
{
    if (!observe_mount_cache)
        return __real_g_unix_mounts_changed_since (time_read);
    mount_table_checks++;
    if (real_mount_cache_changes)
        return __real_g_unix_mounts_changed_since (time_read);
    if (invalidate_mount_cache) {
        invalidate_mount_cache = FALSE;
        mount_table_invalidations++;
        return TRUE;
    }
    return FALSE;
}

static void
observe_stable_mount_table (void)
{
    observe_mount_cache = TRUE;
    invalidate_mount_cache = FALSE;
    mount_table_parses = 0;
    mount_table_checks = 0;
    mount_table_invalidations = 0;
    mount_statx_checks = 0;
    real_mount_cache_changes = FALSE;
    freeze_mount_clock = TRUE;
    mount_clock_now = __real_g_get_monotonic_time ();
}

gboolean
nemo_mount_operation_is_removing (void)
{
    return FALSE;
}

char *__real_g_uuid_string_random (void);

/* Only bucket selection is controlled; transaction names remain unique. */
char *
__wrap_g_uuid_string_random (void)
{
    char *uuid = __real_g_uuid_string_random ();
    if (selected_bucket) {
        uuid[0] = selected_bucket[0];
        uuid[1] = selected_bucket[1];
    }
    return uuid;
}

int __real_statx (int fd, const char *path, int flags, unsigned int mask, struct statx *identity);

int
__wrap_statx (int fd, const char *path, int flags, unsigned int mask, struct statx *identity)
{
    int result = __real_statx (fd, path, flags, mask, identity);
    int saved_errno = errno;
    if (observe_mount_cache)
        mount_statx_checks++;
    sample_descriptor_peak ();
    if (result == 0 && queued_source_roots && path[0] == '\0' && (flags & AT_EMPTY_PATH)) {
        g_autofree char *resolved = path_at_fd (fd, "");
        if (resolved && g_str_has_prefix (resolved, queued_source_prefix)) {
            const char *relative = resolved + strlen (queued_source_prefix);
            if (*relative && !strchr (relative, '/'))
                g_hash_table_add (queued_source_roots, g_strdup (resolved));
        }
    }
    if (result == 0 && different_mount_root && path[0] == '\0' && (flags & AT_EMPTY_PATH)) {
        g_autofree char *proc = g_strdup_printf ("/proc/self/fd/%d", fd);
        g_autofree char *resolved = g_file_read_link (proc, NULL);
        gsize length = strlen (different_mount_root);
        if (resolved && g_str_has_prefix (resolved, different_mount_root) &&
            (resolved[length] == '\0' || resolved[length] == '/')) {
            guint depth = 0;
            for (const char *p = resolved + length; *p; p++)
                depth += *p == '/';
            if (depth == different_mount_depth) {
                identity->stx_mnt_id ^= G_GUINT64_CONSTANT (0x8000000000000000);
                mount_fault_hits++;
                if (!mount_fault_path)
                    mount_fault_path = g_strdup (resolved);
            }
        }
    }
    errno = saved_errno;
    return result;
}

static GPtrArray *
children (const char *path)
{
    g_autoptr (GError) error = NULL;
    GDir *directory = g_dir_open (path, 0, &error);
    g_assert_no_error (error);
    g_assert_nonnull (directory);
    GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
    const char *name;
    while ((name = g_dir_read_name (directory)))
        g_ptr_array_add (names, g_strdup (name));
    g_dir_close (directory);
    return names;
}

static gint
compare_names (gconstpointer left, gconstpointer right)
{
    return g_strcmp0 (*(char * const *) left, *(char * const *) right);
}

static char *
read_file (const char *path)
{
    g_autoptr (GError) error = NULL;
    char *contents = NULL;
    g_assert_true (g_file_get_contents (path, &contents, NULL, &error));
    g_assert_no_error (error);
    return contents;
}

static void
assert_contents (const char *path, const char *expected)
{
    g_autofree char *contents = read_file (path);
    g_assert_cmpstr (contents, ==, expected);
}

static void
write_fd (int fd, const char *contents)
{
    gsize remaining = strlen (contents);
    while (remaining) {
        ssize_t count = write (fd, contents, remaining);
        if (count < 0 && errno == EINTR)
            continue;
        g_assert_cmpint (count, >, 0);
        contents += count;
        remaining -= count;
    }
    g_assert_cmpint (fsync (fd), ==, 0);
}

static void
create_file (const char *path, const char *contents)
{
    int fd = g_open (path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    g_assert_cmpint (fd, >=, 0);
    write_fd (fd, contents);
    g_assert_cmpint (close (fd), ==, 0);
}

static char *
recovery_path (const char *destination)
{
    g_autofree char *name = g_strdup_printf (".nemo-recovery-%" G_GUINT64_FORMAT,
                                            (guint64) geteuid ());
    return g_build_filename (destination, name, NULL);
}

static Fixture *
fixture_new (void)
{
    Fixture *fixture = g_new0 (Fixture, 1);
    g_autofree char *cwd = g_get_current_dir ();
    fixture->path = g_build_filename (cwd, ".nemo-recovery-test-XXXXXX", NULL);
    g_assert_nonnull (g_mkdtemp_full (fixture->path, 0700));
    /* An assertion failure deliberately leaves this exact private fixture. */
    g_test_message ("Disposable recovery fixture: %s", fixture->path);
    g_assert_cmpint (g_lstat (fixture->path, &fixture->identity), ==, 0);
    fixture->sources = g_build_filename (fixture->path, "sources", NULL);
    fixture->destination = g_build_filename (fixture->path, "destination", NULL);
    fixture->recovery = recovery_path (fixture->destination);
    g_assert_cmpint (g_mkdir (fixture->sources, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (fixture->destination, 0700), ==, 0);
    return fixture;
}

static void
remove_fixture_entry (const char *path)
{
    struct stat identity;
    g_assert_cmpint (g_lstat (path, &identity), ==, 0);
    if (S_ISDIR (identity.st_mode)) {
        g_autoptr (GPtrArray) names = children (path);
        for (guint i = 0; i < names->len; i++) {
            g_autofree char *child = g_build_filename (path, names->pdata[i], NULL);
            remove_fixture_entry (child);
        }
        g_assert_cmpint (g_rmdir (path), ==, 0);
    } else {
        g_assert_cmpint (g_unlink (path), ==, 0);
    }
}

static void
fixture_free (Fixture *fixture)
{
    struct stat identity;
    g_assert_cmpint (g_lstat (fixture->path, &identity), ==, 0);
    g_assert_cmpuint (identity.st_dev, ==, fixture->identity.st_dev);
    g_assert_cmpuint (identity.st_ino, ==, fixture->identity.st_ino);
    g_assert_true (S_ISDIR (identity.st_mode));
    /* Never scan a shared prefix or follow symlinks out of this fixture. */
    remove_fixture_entry (fixture->path);
    g_free (fixture->path);
    g_free (fixture->sources);
    g_free (fixture->destination);
    g_free (fixture->recovery);
    g_free (fixture);
    selected_bucket = NULL;
}

static void
snapshot_entry (const char *path, GString *snapshot)
{
    struct stat st;
    g_assert_cmpint (g_lstat (path, &st), ==, 0);
    g_string_append_printf (snapshot,
                            "%s:%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT
                            ":%u:%u:%u:%" G_GUINT64_FORMAT ":%" G_GINT64_FORMAT
                            ":%" G_GINT64_FORMAT ":%ld:%" G_GINT64_FORMAT ":%ld\n",
                            path, (guint64) st.st_dev, (guint64) st.st_ino,
                            st.st_mode, st.st_uid, st.st_gid, (guint64) st.st_nlink,
                            (gint64) st.st_size, (gint64) st.st_mtim.tv_sec,
                            st.st_mtim.tv_nsec, (gint64) st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    if (S_ISDIR (st.st_mode)) {
        g_autoptr (GPtrArray) names = children (path);
        g_ptr_array_sort (names, compare_names);
        for (guint i = 0; i < names->len; i++) {
            g_autofree char *child = g_build_filename (path, names->pdata[i], NULL);
            snapshot_entry (child, snapshot);
        }
    } else {
        g_assert_true (S_ISREG (st.st_mode));
        g_autofree char *contents = read_file (path);
        g_autofree char *checksum = g_compute_checksum_for_string (G_CHECKSUM_SHA256, contents, -1);
        g_string_append_printf (snapshot, "%s\n", checksum);
    }
}

static char *
snapshot_tree (const char *path)
{
    GString *snapshot = g_string_new (NULL);
    snapshot_entry (path, snapshot);
    return g_string_free (snapshot, FALSE);
}

static void
assert_tree_unchanged (const char *path, const char *before)
{
    g_autofree char *after = snapshot_tree (path);
    g_assert_cmpstr (after, ==, before);
}

static NemoTransferGuard *
new_guard (Fixture *fixture, const char *destination_path)
{
    g_autoptr (GFile) source = g_file_new_for_path (fixture->sources);
    g_autoptr (GFile) destination = g_file_new_for_path (destination_path);
    GList sources = { .data = source };
    NemoTransferGuard *guard = nemo_transfer_guard_new (&sources, destination, FALSE);
    g_autoptr (GError) error = NULL;
    g_assert_true (nemo_transfer_guard_check (guard, &error));
    g_assert_no_error (error);
    return guard;
}

static void
release_guard (NemoTransferGuard *guard)
{
    g_autoptr (GError) error = NULL;
    g_assert_true (nemo_transfer_guard_release (guard, &error));
    g_assert_no_error (error);
    nemo_transfer_guard_unref (guard);
}

static void
release_undo (NemoTransferUndo *undo)
{
    g_autoptr (GError) error = NULL;
    g_assert_true (nemo_transfer_undo_release (undo, &error));
    g_assert_no_error (error);
    nemo_transfer_undo_unref (undo);
}

static NemoTransferTransaction *
publish_copy (NemoTransferGuard *guard, const char *source_path,
              const char *destination_path, gboolean replace, char **transaction_path)
{
    g_autoptr (GFile) source = g_file_new_for_path (source_path);
    g_autoptr (GFile) destination = g_file_new_for_path (destination_path);
    g_autoptr (GError) error = NULL;
    if (replace) {
        g_assert_true (nemo_transfer_guard_expect (guard, destination, &error));
        g_assert_no_error (error);
    }
    NemoTransferTransaction *transaction =
        nemo_transfer_transaction_new (guard, source, destination, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (transaction);
    g_autofree char *bound_source = g_file_get_path (nemo_transfer_transaction_source (transaction));
    g_autofree char *contents = read_file (bound_source);
    g_autofree char *stage = g_file_get_path (nemo_transfer_transaction_stage (transaction));
    if (transaction_path) {
        g_autofree char *parent = g_path_get_dirname (stage);
        *transaction_path = g_file_read_link (parent, &error);
        g_assert_no_error (error);
        g_assert_nonnull (*transaction_path);
    }
    int fd = g_open (stage, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    g_assert_cmpint (fd, >=, 0);
    g_assert_true (nemo_transfer_transaction_stage_created (transaction, &error));
    g_assert_no_error (error);
    /* The actual writer descriptor, not a reopened reader, establishes durability. */
    write_fd (fd, contents);
    g_assert_cmpint (close (fd), ==, 0);
    gboolean published = FALSE;
    g_assert_true (nemo_transfer_transaction_publish (transaction, replace, &published, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (published);
    assert_contents (source_path, contents);
    assert_contents (destination_path, contents);
    return transaction;
}

static NemoTransferUndo *
finish_copy (NemoTransferTransaction *transaction)
{
    g_autoptr (GError) error = NULL;
    g_assert_true (nemo_transfer_transaction_finish (transaction, &error));
    g_assert_no_error (error);
    NemoTransferUndo *undo = nemo_transfer_transaction_undo (transaction, FALSE);
    g_assert_nonnull (undo);
    nemo_transfer_transaction_free (transaction);
    return undo;
}

static char *
assert_owner_marker (const char *recovery)
{
    g_autofree char *owner = g_build_filename (recovery, "owner", NULL);
    char *contents = read_file (owner);
    g_autofree char *prefix = g_strdup_printf ("Nemo recovery storage\nversion=1\nowner=%"
                                              G_GUINT64_FORMAT "\ntoken=", (guint64) geteuid ());
    g_assert_true (g_str_has_prefix (contents, prefix));
    g_assert_cmpuint (strlen (contents), ==, strlen (prefix) + 37);
    g_autofree char *token = g_strndup (contents + strlen (prefix), 36);
    g_assert_true (g_uuid_string_is_valid (token));
    g_assert_cmpint (contents[strlen (contents) - 1], ==, '\n');
    /* Exact grammar above excludes persisted device/inode identity. */
    return contents;
}

static Inventory
inventory (const char *recovery)
{
    Inventory result = { 0 };
    g_autofree char *owner = assert_owner_marker (recovery);
    g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, owner, -1);
    g_autoptr (GPtrArray) buckets = children (recovery);
    for (guint i = 0; i < buckets->len; i++) {
        const char *name = buckets->pdata[i];
        if (!strcmp (name, "owner"))
            continue;
        g_assert_cmpuint (strlen (name), ==, 2);
        g_assert_nonnull (strchr ("0123456789abcdef", name[0]));
        g_assert_nonnull (strchr ("0123456789abcdef", name[1]));
        result.buckets++;
        g_autofree char *bucket = g_build_filename (recovery, name, NULL);
        g_autofree char *marker = g_build_filename (bucket, "owner", NULL);
        g_autofree char *expected = g_strdup_printf (
            "Nemo recovery bucket\nversion=1\nowner=%" G_GUINT64_FORMAT
            "\ncontainer-sha256=%s\nbucket=%s\n", (guint64) geteuid (), hash, name);
        assert_contents (marker, expected);
        g_autoptr (GPtrArray) entries = children (bucket);
        result.max_children = MAX (result.max_children, entries->len);
        for (guint j = 0; j < entries->len; j++) {
            const char *entry = entries->pdata[j];
            if (!strcmp (entry, "owner"))
                continue;
            g_assert_true (g_str_has_prefix (entry, "transaction-"));
            g_assert_true (g_uuid_string_is_valid (entry + strlen ("transaction-")));
            g_assert_cmpmem (entry + strlen ("transaction-"), 2, name, 2);
            g_autofree char *transaction = g_build_filename (bucket, entry, NULL);
            struct stat st;
            g_assert_cmpint (g_lstat (transaction, &st), ==, 0);
            g_assert_true (S_ISDIR (st.st_mode));
            result.transactions++;
        }
    }
    g_assert_cmpuint (result.buckets, <=, 256);
    return result;
}

static guint
descriptor_count (void)
{
    g_autoptr (GPtrArray) descriptors = children ("/proc/self/fd");
    return descriptors->len;
}

static void
queued_roots_low_fd (void)
{
    Fixture *fixture = fixture_new ();
    selected_bucket = "c4";
    g_autoptr (GPtrArray) sources = g_ptr_array_new_with_free_func (g_object_unref);
    GList *selection = NULL;
    for (guint i = 0; i < QUEUED_ROOTS; i++) {
        g_autofree char *name = g_strdup_printf ("parent-%04u", i);
        g_autofree char *parent = g_build_filename (fixture->sources, name, NULL);
        g_autofree char *source = g_build_filename (parent, "item", NULL);
        g_autofree char *payload = g_strdup_printf ("original queued root %04u\n", i);
        g_assert_cmpint (g_mkdir (parent, 0700), ==, 0);
        create_file (source, payload);
        GFile *file = g_file_new_for_path (source);
        g_ptr_array_add (sources, file);
        selection = g_list_prepend (selection, file);
    }
    selection = g_list_reverse (selection);
    NemoTransferGuard *warmup = new_guard (fixture, fixture->destination);
    release_guard (warmup);
    guint baseline = descriptor_count ();
    struct rlimit original_limit, low_limit;
    g_assert_cmpint (getrlimit (RLIMIT_NOFILE, &original_limit), ==, 0);
    g_assert_cmpuint (original_limit.rlim_max, >=, 64);
    low_limit = original_limit;
    low_limit.rlim_cur = 64;
    g_assert_cmpint (setrlimit (RLIMIT_NOFILE, &low_limit), ==, 0);
    g_autoptr (GHashTable) captured = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    g_autofree char *source_prefix = g_strconcat (fixture->sources, "/", NULL);
    queued_source_roots = captured;
    queued_source_prefix = source_prefix;
    sample_transfer_fds = TRUE;
    sampled_fd_peak = baseline;
    g_autoptr (GFile) destination_root = g_file_new_for_path (fixture->destination);
    NemoTransferGuard *guard = nemo_transfer_guard_new (selection, destination_root, FALSE);
    queued_source_roots = NULL;
    queued_source_prefix = NULL;
    g_list_free (selection);
    /* Every parent identity must be captured at queue time, not first use. */
    g_assert_cmpuint (g_hash_table_size (captured), ==, QUEUED_ROOTS);
    g_assert_cmpuint (sampled_fd_peak, <=, baseline + 24);
    g_autoptr (GError) error = NULL;
    g_assert_true (nemo_transfer_guard_check (guard, &error));
    g_assert_no_error (error);
    g_autoptr (GPtrArray) history =
        g_ptr_array_new_with_free_func ((GDestroyNotify) nemo_transfer_undo_unref);
    for (guint i = 0; i < QUEUED_ROOTS; i++) {
        g_autofree char *source = g_file_get_path (sources->pdata[i]);
        g_autofree char *name = g_strdup_printf ("copy-%04u", i);
        g_autofree char *destination = g_build_filename (fixture->destination, name, NULL);
        g_ptr_array_add (history, finish_copy (publish_copy (guard, source, destination, FALSE, NULL)));
        g_assert_cmpuint (sampled_fd_peak, <=, baseline + 24);
    }
    g_assert_cmpuint (history->len, ==, QUEUED_ROOTS);
    Inventory completed = inventory (fixture->recovery);
    g_assert_cmpuint (completed.transactions, ==, 0);
    g_assert_cmpuint (completed.buckets, ==, 1);

    g_autofree char *evicted_parent = g_build_filename (fixture->sources, "parent-0000", NULL);
    g_autoptr (GPtrArray) descriptors = children ("/proc/self/fd");
    for (guint i = 0; i < descriptors->len; i++) {
        g_autofree char *proc = g_build_filename ("/proc/self/fd", descriptors->pdata[i], NULL);
        g_autofree char *resolved = g_file_read_link (proc, NULL);
        g_assert_cmpstr (resolved, !=, evicted_parent);
    }
    g_autofree char *retired = g_build_filename (fixture->sources, "retired-parent-0000", NULL);
    g_assert_cmpint (g_rename (evicted_parent, retired), ==, 0);
    g_assert_cmpint (g_mkdir (evicted_parent, 0700), ==, 0);
    g_autofree char *replacement = g_build_filename (evicted_parent, "item", NULL);
    create_file (replacement, "new occupant must not inherit the queued identity\n");
    g_autofree char *recovery_before = snapshot_tree (fixture->recovery);
    g_autofree char *replacement_before = snapshot_tree (evicted_parent);
    g_autofree char *retired_before = snapshot_tree (retired);
    g_autofree char *blocked_path = g_build_filename (fixture->destination, "blocked-copy", NULL);
    g_autoptr (GFile) blocked = g_file_new_for_path (blocked_path);
    NemoTransferTransaction *transaction =
        nemo_transfer_transaction_new (guard, sources->pdata[0], blocked, NULL, &error);
    g_assert_null (transaction);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_false (nemo_transfer_guard_can_copy_fallback (guard));
    g_assert_false (g_file_test (blocked_path, G_FILE_TEST_EXISTS));
    assert_tree_unchanged (fixture->recovery, recovery_before);
    assert_tree_unchanged (evicted_parent, replacement_before);
    assert_tree_unchanged (retired, retired_before);
    for (guint i = 0; i < QUEUED_ROOTS; i++) {
        g_autofree char *source = i == 0 ? g_build_filename (retired, "item", NULL) :
                                          g_file_get_path (sources->pdata[i]);
        g_autofree char *name = g_strdup_printf ("copy-%04u", i);
        g_autofree char *destination = g_build_filename (fixture->destination, name, NULL);
        g_autofree char *payload = g_strdup_printf ("original queued root %04u\n", i);
        assert_contents (source, payload);
        assert_contents (destination, payload);
    }
    release_guard (guard);
    g_ptr_array_set_size (history, 0);
    g_assert_cmpuint (descriptor_count (), <=, baseline);
    sample_transfer_fds = FALSE;
    g_assert_cmpint (setrlimit (RLIMIT_NOFILE, &original_limit), ==, 0);
    struct rlimit restored;
    g_assert_cmpint (getrlimit (RLIMIT_NOFILE, &restored), ==, 0);
    g_assert_cmpuint (restored.rlim_cur, ==, original_limit.rlim_cur);
    g_assert_cmpuint (restored.rlim_max, ==, original_limit.rlim_max);
    g_test_message ("%u queued parents captured and copied under RLIMIT_NOFILE=64; FD peak %u (+%u); "
                    "evicted root swap refused before staging; original limit restored",
                    QUEUED_ROOTS, sampled_fd_peak, sampled_fd_peak - baseline);
    fixture_free (fixture);
}

static void
batch_copy (gconstpointer data)
{
    gboolean replace = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new ();
    /* Undo keeps the guard's shared monitor alive after directory pins close. */
    g_autoptr (GUnixMountMonitor) baseline_monitor = g_unix_mount_monitor_get ();
    g_assert_nonnull (baseline_monitor);
    /* Warm up GIO before measuring only transfer-owned descriptors. */
    NemoTransferGuard *guard = new_guard (fixture, fixture->destination);
    release_guard (guard);
    guint baseline = descriptor_count ();
    guard = new_guard (fixture, fixture->destination);
    g_autoptr (GPtrArray) history = g_ptr_array_new_with_free_func ((GDestroyNotify) nemo_transfer_undo_unref);
    g_autoptr (GPtrArray) payloads = g_ptr_array_new_with_free_func (g_free);
    guint peak = baseline;
    for (guint i = 0; i < BATCH_SIZE; i++) {
        g_autofree char *name = g_strdup_printf ("item-%03u", i);
        g_autofree char *source_parent = g_build_filename (fixture->sources, name, NULL);
        g_autofree char *destination_parent = g_build_filename (fixture->destination, name, NULL);
        g_assert_cmpint (g_mkdir (source_parent, 0700), ==, 0);
        g_assert_cmpint (g_mkdir (destination_parent, 0700), ==, 0);
        g_autofree char *source = g_build_filename (source_parent, "data", NULL);
        g_autofree char *destination = g_build_filename (destination_parent, "data", NULL);
        g_autofree char *fresh = g_strdup_printf ("new payload %03u\n", i);
        g_autofree char *old = g_strdup_printf ("irreplaceable fixture payload %03u\n", i);
        create_file (source, fresh);
        if (replace)
            create_file (destination, old);
        char *transaction_path = NULL;
        NemoTransferTransaction *transaction =
            publish_copy (guard, source, destination, replace, &transaction_path);
        g_autofree char *directory = transaction_path;
        g_assert_true (g_str_has_prefix (directory, fixture->recovery));
        g_ptr_array_add (payloads, g_build_filename (directory, "payload", NULL));
        g_ptr_array_add (history, finish_copy (transaction));
        if (replace)
            assert_contents (payloads->pdata[i], old);
        else
            g_assert_false (g_file_test (directory, G_FILE_TEST_EXISTS));
        peak = MAX (peak, descriptor_count ());
        /* Hundreds of distinct parents must exercise the bounded LRU. */
        g_assert_cmpuint (peak, <=, baseline + 40);
    }
    Inventory before = inventory (fixture->recovery);
    g_assert_cmpuint (before.buckets, >, 1);
    g_assert_cmpuint (before.transactions, ==, replace ? BATCH_SIZE : 0);
    g_assert_cmpuint (before.max_children, <, 64);
    if (!replace)
        g_assert_cmpuint (before.max_children, ==, 1);
    release_guard (guard);
    g_assert_cmpuint (descriptor_count (), <=, baseline);
    /* Finalize the entire undo history without applying it or running GC. */
    g_ptr_array_set_size (history, 0);
    g_assert_cmpuint (descriptor_count (), <=, baseline);
    Inventory after = inventory (fixture->recovery);
    g_assert_cmpuint (after.buckets, ==, before.buckets);
    g_assert_cmpuint (after.transactions, ==, before.transactions);
    for (guint i = 0; i < BATCH_SIZE; i++) {
        g_autofree char *name = g_strdup_printf ("item-%03u", i);
        g_autofree char *source = g_build_filename (fixture->sources, name, "data", NULL);
        g_autofree char *destination = g_build_filename (fixture->destination, name, "data", NULL);
        g_autofree char *fresh = g_strdup_printf ("new payload %03u\n", i);
        g_autofree char *old = g_strdup_printf ("irreplaceable fixture payload %03u\n", i);
        assert_contents (source, fresh);
        assert_contents (destination, fresh);
        if (replace)
            assert_contents (payloads->pdata[i], old);
    }
    g_test_message ("%u copies: %u buckets, %u retained transactions, max bucket children %u; FD peak +%u",
                    BATCH_SIZE, after.buckets, after.transactions, after.max_children, peak - baseline);
    fixture_free (fixture);
}

static void
copy_job (Fixture *fixture, const char *destination_parent, const char *name)
{
    g_autofree char *source = g_build_filename (fixture->sources, name, NULL);
    g_autofree char *destination = g_build_filename (destination_parent, name, NULL);
    create_file (source, name);
    NemoTransferGuard *guard = new_guard (fixture, destination_parent);
    NemoTransferUndo *undo = finish_copy (publish_copy (guard, source, destination, FALSE, NULL));
    release_guard (guard);
    release_undo (undo);
}

static void
portable_owner (void)
{
    Fixture *fixture = fixture_new ();
    selected_bucket = "42";
    copy_job (fixture, fixture->destination, "first");
    g_autofree char *owner_path = g_build_filename (fixture->recovery, "owner", NULL);
    g_autofree char *bucket_path = g_build_filename (fixture->recovery, "42", "owner", NULL);
    g_autofree char *owner_before = snapshot_tree (owner_path);
    g_autofree char *bucket_before = snapshot_tree (bucket_path);
    g_autofree char *owner = assert_owner_marker (fixture->recovery);
    g_autofree char *bucket = read_file (bucket_path);
    copy_job (fixture, fixture->destination, "second");
    assert_tree_unchanged (owner_path, owner_before);
    assert_tree_unchanged (bucket_path, bucket_before);
    g_assert_cmpuint (inventory (fixture->recovery).transactions, ==, 0);

    /* Reuse the serialized markers at different inodes in a new job. */
    g_autofree char *portable = g_build_filename (fixture->path, "portable", NULL);
    g_autofree char *recovery = recovery_path (portable);
    g_autofree char *portable_bucket = g_build_filename (recovery, "42", NULL);
    g_assert_cmpint (g_mkdir_with_parents (portable_bucket, 0700), ==, 0);
    g_autofree char *portable_owner = g_build_filename (recovery, "owner", NULL);
    g_autofree char *portable_marker = g_build_filename (portable_bucket, "owner", NULL);
    create_file (portable_owner, owner);
    create_file (portable_marker, bucket);
    struct stat original, copied;
    g_assert_cmpint (g_lstat (owner_path, &original), ==, 0);
    g_assert_cmpint (g_lstat (portable_owner, &copied), ==, 0);
    g_assert_cmpuint (original.st_ino, !=, copied.st_ino);
    g_autofree char *portable_before = snapshot_tree (portable_owner);
    g_autofree char *portable_bucket_before = snapshot_tree (portable_marker);
    copy_job (fixture, portable, "third");
    assert_tree_unchanged (portable_owner, portable_before);
    assert_tree_unchanged (portable_marker, portable_bucket_before);
    g_assert_cmpuint (inventory (recovery).transactions, ==, 0);
    fixture_free (fixture);
}

typedef enum {
    UNKNOWN_ROOT_MARKER,
    UNKNOWN_BUCKET_FILE,
    UNKNOWN_BUCKET_OWNER,
    UNKNOWN_BUCKET_NO_OWNER,
    UNKNOWN_BUCKET_WRONG_ROOT
} UnknownNamespace;

static void
unknown_namespace (gconstpointer data)
{
    UnknownNamespace kind = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new ();
    selected_bucket = "71";
    if (kind == UNKNOWN_ROOT_MARKER) {
        g_assert_cmpint (g_mkdir (fixture->recovery, 0700), ==, 0);
        g_autofree char *owner = g_build_filename (fixture->recovery, "owner", NULL);
        create_file (owner, "unrecognized recovery owner; preserve this data\n");
    } else {
        copy_job (fixture, fixture->destination, "initial");
        selected_bucket = "72";
        g_autofree char *bucket = g_build_filename (fixture->recovery, "72", NULL);
        if (kind == UNKNOWN_BUCKET_FILE) {
            create_file (bucket, "not an owned recovery bucket\n");
        } else {
            g_assert_cmpint (g_mkdir (bucket, 0700), ==, 0);
            g_autofree char *sentinel = g_build_filename (bucket, "unrelated", NULL);
            create_file (sentinel, "unknown bucket child must not be removed\n");
            g_autofree char *owner = g_build_filename (bucket, "owner", NULL);
            if (kind == UNKNOWN_BUCKET_OWNER)
                create_file (owner, "not a Nemo owner marker\n");
            else if (kind == UNKNOWN_BUCKET_WRONG_ROOT) {
                g_autofree char *marker = g_strdup_printf (
                    "Nemo recovery bucket\nversion=1\nowner=%" G_GUINT64_FORMAT
                    "\ncontainer-sha256=%064u\nbucket=72\n", (guint64) geteuid (), 0);
                create_file (owner, marker);
            }
        }
    }
    g_autofree char *source_path = g_build_filename (fixture->sources, "untouched", NULL);
    g_autofree char *destination_path = g_build_filename (fixture->destination, "untouched", NULL);
    create_file (source_path, "source must survive refused publication\n");
    create_file (destination_path, "destination must survive refused publication\n");
    g_autofree char *before = snapshot_tree (fixture->path);
    NemoTransferGuard *guard = new_guard (fixture, fixture->destination);
    g_autoptr (GFile) source = g_file_new_for_path (source_path);
    g_autoptr (GFile) destination = g_file_new_for_path (destination_path);
    g_autoptr (GError) error = NULL;
    g_assert_true (nemo_transfer_guard_expect (guard, destination, &error));
    g_assert_no_error (error);
    NemoTransferTransaction *transaction =
        nemo_transfer_transaction_new (guard, source, destination, NULL, &error);
    g_assert_null (transaction);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_false (nemo_transfer_guard_can_copy_fallback (guard));
    release_guard (guard);
    assert_tree_unchanged (fixture->path, before);
    fixture_free (fixture);
}

static void
recovery_mount_boundary (gconstpointer data)
{
    Fixture *fixture = fixture_new ();
    selected_bucket = "85";
    g_autofree char *source_path = g_build_filename (fixture->sources, "item", NULL);
    g_autofree char *destination_path = g_build_filename (fixture->destination, "item", NULL);
    create_file (source_path, "source remains before any stage writer exists\n");
    create_file (destination_path, "original destination remains before publication\n");
    g_autofree char *source_before = snapshot_tree (source_path);
    g_autofree char *destination_before = snapshot_tree (destination_path);
    observe_stable_mount_table ();
    NemoTransferGuard *guard = new_guard (fixture, fixture->destination);
    g_autoptr (GFile) source = g_file_new_for_path (source_path);
    g_autoptr (GFile) destination = g_file_new_for_path (destination_path);
    g_autoptr (GError) error = NULL;
    g_assert_true (nemo_transfer_guard_expect (guard, destination, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (mount_table_parses, ==, 1);

    /* Simulate a nested mount only at an fd inside this disposable namespace. */
    different_mount_root = fixture->recovery;
    different_mount_depth = GPOINTER_TO_UINT (data);
    mount_fault_hits = 0;
    g_assert_null (mount_fault_path);
    NemoTransferTransaction *transaction =
        nemo_transfer_transaction_new (guard, source, destination, NULL, &error);
    different_mount_root = NULL;
    g_assert_cmpuint (mount_fault_hits, >, 0);
    g_assert_null (transaction);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull (strstr (error->message, "mount boundary"));
    g_assert_cmpuint (mount_table_parses, ==, 1);
    g_assert_cmpuint (mount_table_checks, >, 0);
    observe_mount_cache = FALSE;
    g_assert_nonnull (mount_fault_path);
    g_autoptr (GPtrArray) entries = children (mount_fault_path);
    /* Atomic namespace initialization may already have published its owner. */
    for (guint i = 0; i < entries->len; i++) {
        g_assert_cmpuint (different_mount_depth, <, 2);
        g_assert_cmpstr (entries->pdata[i], ==, "owner");
    }
    release_guard (guard);
    assert_tree_unchanged (source_path, source_before);
    assert_tree_unchanged (destination_path, destination_before);
    g_clear_pointer (&mount_fault_path, g_free);
    fixture_free (fixture);
}

static void
interleave_namespace_publication (const char *private_path)
{
    NamespaceRace *race = namespace_race;
    g_assert_cmpuint (race->interleavings, ==, 0);
    g_assert_true (g_str_has_prefix (private_path, race->fixture->destination));
    g_assert_cmpstr (private_path, !=, race->public_path);
    struct stat identity;
    g_assert_cmpint (g_lstat (race->public_path, &identity), ==, -1);
    g_assert_cmpint (errno, ==, ENOENT);
    g_autofree char *private_owner = g_build_filename (private_path, "owner", NULL);
    g_autofree char *contents = read_file (private_owner);
    g_assert_true (g_str_has_prefix (contents, "Nemo recovery "));
    race->private_path = g_strdup (private_path);
    race->interleavings++;
    race->observing = TRUE;
    /* The first guard is paused before publication while an independent job wins. */
    copy_job (race->fixture, race->fixture->destination, "second-job");
    race->observing = FALSE;
    g_autofree char *root_owner = assert_owner_marker (race->fixture->recovery);
    g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, root_owner, -1);
    g_autofree char *bucket = g_build_filename (race->fixture->recovery, "96", NULL);
    g_autofree char *bucket_owner = g_build_filename (bucket, "owner", NULL);
    g_autofree char *expected = g_strdup_printf (
        "Nemo recovery bucket\nversion=1\nowner=%" G_GUINT64_FORMAT
        "\ncontainer-sha256=%s\nbucket=96\n", (guint64) geteuid (), hash);
    assert_contents (bucket_owner, expected);
    g_autoptr (GPtrArray) entries = children (bucket);
    g_assert_cmpuint (entries->len, ==, 1);
    g_autofree char *public_owner = g_build_filename (race->public_path, "owner", NULL);
    race->winner_owner = snapshot_tree (public_owner);
    if (race->swap_loser) {
        g_autofree char *retired = g_build_filename (race->fixture->path, "retired-initializer", NULL);
        g_assert_cmpint (g_rename (private_path, retired), ==, 0);
        g_assert_cmpint (g_mkdir (private_path, 0700), ==, 0);
        g_autofree char *owner = g_build_filename (private_path, "owner", NULL);
        g_autofree char *data = g_build_filename (private_path, "do-not-delete", NULL);
        create_file (owner, "unrecognized swapped candidate owner\n");
        create_file (data, "unknown swapped candidate data must survive\n");
        g_assert_cmpint (g_lstat (private_path, &race->unknown_identity), ==, 0);
    }
}

static void
concurrent_namespace_initialization (gconstpointer data)
{
    gboolean bucket = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new ();
    selected_bucket = "96";
    NamespaceRace race = {
        .fixture = fixture,
        .public_path = bucket ? g_build_filename (fixture->recovery, "96", NULL) :
                                g_strdup (fixture->recovery)
    };
    namespace_race = &race;
    copy_job (fixture, fixture->destination, "first-job");
    namespace_race = NULL;
    g_assert_cmpuint (race.interleavings, ==, 1);
    g_assert_nonnull (race.private_path);
    g_assert_false (g_file_test (race.private_path, G_FILE_TEST_EXISTS));
    g_autofree char *owner = g_build_filename (race.public_path, "owner", NULL);
    assert_tree_unchanged (owner, race.winner_owner);
    Inventory final = inventory (fixture->recovery);
    g_assert_cmpuint (final.buckets, ==, 1);
    g_assert_cmpuint (final.transactions, ==, 0);
    g_autoptr (GPtrArray) entries = children (fixture->destination);
    /* Both public files and one recovery root; no losing initialization temps. */
    g_assert_cmpuint (entries->len, ==, 3);
    const char *jobs[] = { "first-job", "second-job" };
    for (guint i = 0; i < G_N_ELEMENTS (jobs); i++) {
        g_autofree char *source = g_build_filename (fixture->sources, jobs[i], NULL);
        g_autofree char *destination = g_build_filename (fixture->destination, jobs[i], NULL);
        assert_contents (source, jobs[i]);
        assert_contents (destination, jobs[i]);
    }
    g_free (race.public_path);
    g_free (race.private_path);
    g_free (race.winner_owner);
    fixture_free (fixture);
}

static void
assert_no_native_fallback (NemoTransferGuard *guard, GError *error)
{
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_false (nemo_transfer_guard_can_copy_fallback (guard));
}

static void
mount_table_cache_invalidation (void)
{
    Fixture *fixture = fixture_new ();
    selected_bucket = "af";
    g_autofree char *source = g_build_filename (fixture->sources, "source", NULL);
    create_file (source, "cached mount classification never replaces fd validation\n");
    observe_stable_mount_table ();
    NemoTransferGuard *guard = new_guard (fixture, fixture->destination);
    g_assert_cmpuint (mount_table_parses, ==, 1);
    for (guint i = 0; i < 12; i++) {
        if (i == 6)
            invalidate_mount_cache = TRUE;
        g_autofree char *name = g_strdup_printf ("copy-%u", i);
        g_autofree char *destination = g_build_filename (fixture->destination, name, NULL);
        NemoTransferUndo *undo =
            finish_copy (publish_copy (guard, source, destination, FALSE, NULL));
        nemo_transfer_undo_unref (undo);
        g_assert_cmpuint (mount_table_parses, ==, i < 6 ? 1 : 2);
        g_assert_cmpuint (mount_table_invalidations, ==, i < 6 ? 0 : 1);
        g_assert_false (invalidate_mount_cache);
    }
    g_assert_cmpuint (mount_table_checks, >, 12);
    NemoTransferGuard *independent = new_guard (fixture, fixture->destination);
    g_assert_cmpuint (mount_table_parses, ==, 3);
    release_guard (independent);
    release_guard (guard);
    observe_mount_cache = FALSE;
    g_assert_cmpuint (inventory (fixture->recovery).transactions, ==, 0);
    g_test_message ("One parse per guard; one controlled change caused exactly one refresh (%u checks)",
                    mount_table_checks);
    fixture_free (fixture);
}

static void
mount_table_cache_real_monitor (void)
{
    Fixture *fixture = fixture_new ();
    selected_bucket = "b0";
    g_autofree char *source = g_build_filename (fixture->sources, "source", NULL);
    create_file (source, "real mount monitor cache still validates actual descriptors\n");
    observe_stable_mount_table ();
    real_mount_cache_changes = TRUE;
    freeze_mount_clock = FALSE;
    gint64 started = __real_g_get_monotonic_time ();
    NemoTransferGuard *guard = new_guard (fixture, fixture->destination);
    for (guint i = 0; i < 8; i++) {
        g_autofree char *name = g_strdup_printf ("copy-%u", i);
        g_autofree char *destination = g_build_filename (fixture->destination, name, NULL);
        NemoTransferUndo *undo =
            finish_copy (publish_copy (guard, source, destination, FALSE, NULL));
        nemo_transfer_undo_unref (undo);
    }
    gint64 elapsed = __real_g_get_monotonic_time () - started;
    g_assert_cmpuint (mount_table_parses, >=, 1);
    g_assert_cmpuint (mount_table_parses, <=, 2 + elapsed / G_TIME_SPAN_SECOND);
    g_assert_cmpuint (mount_table_checks, >, 8);
    g_assert_cmpuint (mount_statx_checks, >, 8);
    release_guard (guard);
    observe_mount_cache = FALSE;
    real_mount_cache_changes = FALSE;
    g_test_message ("Unmocked changed_since: %u parses, %u cache checks, %u actual statx calls",
                    mount_table_parses, mount_table_checks, mount_statx_checks);
    fixture_free (fixture);
}

static void
mount_table_cache_clock_expiry (void)
{
    Fixture *fixture = fixture_new ();
    g_autofree char *before = snapshot_tree (fixture->path);
    observe_stable_mount_table ();
    NemoTransferGuard *guard = new_guard (fixture, fixture->destination);
    g_assert_cmpuint (mount_table_parses, ==, 1);
    g_autoptr (GError) error = NULL;
    guint initial_statx = mount_statx_checks;
    mount_clock_now += G_TIME_SPAN_SECOND - 1;
    g_assert_true (nemo_transfer_guard_check (guard, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (mount_table_parses, ==, 1);
    g_assert_cmpuint (mount_statx_checks, >, initial_statx);
    guint before_expiry = mount_statx_checks;
    /* changed_since stays FALSE, and no GLib main context is dispatched. */
    mount_clock_now++;
    g_assert_true (nemo_transfer_guard_check (guard, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (mount_table_parses, ==, 2);
    g_assert_cmpuint (mount_statx_checks, >, before_expiry);
    g_assert_cmpuint (mount_table_invalidations, ==, 0);
    g_assert_true (nemo_transfer_guard_check (guard, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (mount_table_parses, ==, 2);
    release_guard (guard);
    observe_mount_cache = FALSE;
    assert_tree_unchanged (fixture->path, before);
    fixture_free (fixture);
}

static char *
find_fixture_inode (const char *path, const struct stat *expected)
{
    struct stat current;
    g_assert_cmpint (g_lstat (path, &current), ==, 0);
    if (current.st_dev == expected->st_dev && current.st_ino == expected->st_ino)
        return g_strdup (path);
    if (!S_ISDIR (current.st_mode))
        return NULL;
    g_autoptr (GPtrArray) entries = children (path);
    for (guint i = 0; i < entries->len; i++) {
        g_autofree char *child = g_build_filename (path, entries->pdata[i], NULL);
        char *found = find_fixture_inode (child, expected);
        if (found)
            return found;
    }
    return NULL;
}

static void
initialization_preserves_swapped_candidate (void)
{
    Fixture *fixture = fixture_new ();
    selected_bucket = "96";
    g_autofree char *source_path = g_build_filename (fixture->sources, "first-job", NULL);
    g_autofree char *destination_path = g_build_filename (fixture->destination, "first-job", NULL);
    create_file (source_path, "native source stays before capture\n");
    create_file (destination_path, "old native destination stays before replacement\n");
    g_autofree char *source_before = snapshot_tree (source_path);
    g_autofree char *destination_before = snapshot_tree (destination_path);
    g_autoptr (GFile) source = g_file_new_for_path (source_path);
    g_autoptr (GFile) destination = g_file_new_for_path (destination_path);
    g_autoptr (GFile) destination_parent = g_file_new_for_path (fixture->destination);
    GList sources = { .data = source };
    NemoTransferGuard *guard = nemo_transfer_guard_new (&sources, destination_parent, TRUE);
    NamespaceRace race = {
        .fixture = fixture,
        .public_path = g_strdup (fixture->recovery),
        .swap_loser = TRUE
    };
    namespace_race = &race;
    gboolean published = FALSE;
    NemoTransferUndo *undo = NULL;
    g_autoptr (GError) error = NULL;
    g_assert_false (nemo_transfer_native_move (guard, source, destination, TRUE,
                                              &published, &undo, NULL, &error));
    namespace_race = NULL;
    assert_no_native_fallback (guard, error);
    g_assert_false (published);
    g_assert_null (undo);
    g_assert_cmpuint (race.interleavings, ==, 1);
    release_guard (guard);
    assert_tree_unchanged (source_path, source_before);
    assert_tree_unchanged (destination_path, destination_before);
    g_autofree char *owner = g_build_filename (race.public_path, "owner", NULL);
    assert_tree_unchanged (owner, race.winner_owner);
    g_autofree char *retained = find_fixture_inode (fixture->path, &race.unknown_identity);
    g_assert_nonnull (retained);
    g_autofree char *unknown_owner = g_build_filename (retained, "owner", NULL);
    g_autofree char *unknown_data = g_build_filename (retained, "do-not-delete", NULL);
    assert_contents (unknown_owner, "unrecognized swapped candidate owner\n");
    assert_contents (unknown_data, "unknown swapped candidate data must survive\n");
    g_autoptr (GPtrArray) entries = children (retained);
    g_assert_cmpuint (entries->len, ==, 2);
    g_free (race.public_path);
    g_free (race.private_path);
    g_free (race.winner_owner);
    fixture_free (fixture);
}

static void
namespace_refusal_disables_move_fallback (gconstpointer data)
{
    guint kind = GPOINTER_TO_UINT (data);
    gboolean unsupported = kind == 1;
    Fixture *fixture = fixture_new ();
    selected_bucket = "97";
    g_autofree char *source_path = g_build_filename (fixture->sources, "item", NULL);
    g_autofree char *destination_path = g_build_filename (fixture->destination, "item", NULL);
    create_file (source_path, "native source must not become a fallback copy\n");
    create_file (destination_path, "original native destination must remain unchanged\n");
    if (kind == 0) {
        g_assert_cmpint (g_mkdir (fixture->recovery, 0700), ==, 0);
        g_autofree char *owner = g_build_filename (fixture->recovery, "owner", NULL);
        create_file (owner, "unknown namespace owner; never replace or initialize\n");
    }
    g_autofree char *source_before = snapshot_tree (source_path);
    g_autofree char *destination_before = snapshot_tree (destination_path);
    g_autofree char *namespace_before = kind == 0 ? snapshot_tree (fixture->recovery) : NULL;
    g_autoptr (GFile) source = g_file_new_for_path (source_path);
    g_autoptr (GFile) destination = g_file_new_for_path (destination_path);
    g_autoptr (GFile) destination_parent = g_file_new_for_path (fixture->destination);
    GList sources = { .data = source };
    NemoTransferGuard *guard = nemo_transfer_guard_new (&sources, destination_parent, TRUE);
    g_autoptr (GError) error = NULL;
    g_assert_true (nemo_transfer_guard_check (guard, &error));
    g_assert_no_error (error);
    NamespaceRace race = {
        .fixture = fixture,
        .public_path = g_strdup (fixture->recovery),
        .deny_publication = unsupported,
        .io_operation = kind > 1 ? kind - 1 : 0
    };
    if (kind != 0)
        namespace_race = &race;
    gboolean published = FALSE;
    NemoTransferUndo *undo = NULL;
    g_assert_false (nemo_transfer_native_move (guard, source, destination, TRUE,
                                              &published, &undo, NULL, &error));
    namespace_race = NULL;
    assert_no_native_fallback (guard, error);
    g_assert_false (published);
    g_assert_null (undo);
    g_assert_cmpuint (race.denied, ==, unsupported ? 1 : 0);
    g_assert_cmpuint (race.io_faults, ==, kind > 1 ? 1 : 0);
    release_guard (guard);
    assert_tree_unchanged (source_path, source_before);
    assert_tree_unchanged (destination_path, destination_before);
    if (kind == 0) {
        assert_tree_unchanged (fixture->recovery, namespace_before);
    } else {
        g_assert_false (g_file_test (fixture->recovery, G_FILE_TEST_EXISTS));
        g_autoptr (GPtrArray) entries = children (fixture->destination);
        for (guint i = 0; i < entries->len; i++) {
            const char *name = entries->pdata[i];
            if (!strcmp (name, "item"))
                continue;
            /* Failed public initialization is retained rather than path-deleted. */
            g_assert_true (g_str_has_prefix (name, ".nemo-recovery-init-"));
            g_autofree char *candidate = g_build_filename (fixture->destination, name, NULL);
            g_autofree char *owner = assert_owner_marker (candidate);
            g_autoptr (GPtrArray) children_left = children (candidate);
            g_assert_cmpuint (children_left->len, ==, 1);
        }
    }
    g_free (race.public_path);
    fixture_free (fixture);
}

static char *
only_failed_initializer (Fixture *fixture)
{
    g_autoptr (GPtrArray) entries = children (fixture->destination);
    char *candidate = NULL;
    for (guint i = 0; i < entries->len; i++) {
        const char *name = entries->pdata[i];
        if (!g_str_has_prefix (name, ".nemo-recovery-init-"))
            continue;
        g_assert_null (candidate);
        candidate = g_build_filename (fixture->destination, name, NULL);
    }
    g_assert_nonnull (candidate);
    return candidate;
}

static void
initialization_failure_latched (gconstpointer data)
{
    gboolean foreign = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new ();
    selected_bucket = "b1";
    for (guint i = 0; i < 8; i++) {
        g_autofree char *name = g_strdup_printf ("item-%u", i);
        g_autofree char *source = g_build_filename (fixture->sources, name, NULL);
        g_autofree char *destination = g_build_filename (fixture->destination, name, NULL);
        create_file (source, "original source survives repeated initialization failure\n");
        create_file (destination, "original destination survives repeated initialization failure\n");
    }
    NemoTransferGuard *guard = new_guard (fixture, fixture->destination);
    NamespaceRace race = {
        .fixture = fixture,
        .public_path = g_strdup (fixture->recovery),
        .io_operation = 1,
        .observing = TRUE
    };
    namespace_race = &race;
    g_autoptr (GError) first_error = NULL;
    g_autofree char *candidate = NULL;
    g_autofree char *candidate_before = NULL;
    for (guint i = 0; i < 8; i++) {
        g_autofree char *name = g_strdup_printf ("item-%u", i);
        g_autofree char *source_path = g_build_filename (fixture->sources, name, NULL);
        g_autofree char *destination_path = g_build_filename (fixture->destination, name, NULL);
        g_autoptr (GFile) source = g_file_new_for_path (source_path);
        g_autoptr (GFile) destination = g_file_new_for_path (destination_path);
        g_autoptr (GError) error = NULL;
        NemoTransferTransaction *transaction =
            nemo_transfer_transaction_new (guard, source, destination, NULL, &error);
        g_assert_null (transaction);
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
        g_assert_nonnull (strstr (error->message, g_strerror (EIO)));
        g_assert_false (nemo_transfer_guard_can_copy_fallback (guard));
        if (i == 0) {
            first_error = g_error_copy (error);
            candidate = only_failed_initializer (fixture);
            if (foreign) {
                g_autofree char *owner = g_build_filename (candidate, "owner", NULL);
                g_autofree char *unknown = g_build_filename (candidate, "foreign-data", NULL);
                change_byte (owner, 0);
                create_file (unknown, "foreign changed candidate data must never be deleted\n");
            }
            candidate_before = snapshot_tree (candidate);
        } else {
            g_assert_cmpuint (error->domain, ==, first_error->domain);
            g_assert_cmpint (error->code, ==, first_error->code);
            g_assert_cmpstr (error->message, ==, first_error->message);
        }
        g_autofree char *only_candidate = only_failed_initializer (fixture);
        g_assert_cmpstr (only_candidate, ==, candidate);
        assert_tree_unchanged (candidate, candidate_before);
        g_assert_false (g_file_test (fixture->recovery, G_FILE_TEST_EXISTS));
        assert_contents (source_path, "original source survives repeated initialization failure\n");
        assert_contents (destination_path, "original destination survives repeated initialization failure\n");
    }
    namespace_race = NULL;
    g_assert_cmpuint (race.io_faults, ==, 1);
    g_autoptr (GError) error = NULL;
    g_assert_true (nemo_transfer_guard_release (guard, &error));
    g_assert_no_error (error);
    g_autofree char *source_path = g_build_filename (fixture->sources, "item-0", NULL);
    g_autofree char *destination_path = g_build_filename (fixture->destination, "after-release", NULL);
    NemoTransferUndo *undo =
        finish_copy (publish_copy (guard, source_path, destination_path, FALSE, NULL));
    release_guard (guard);
    release_undo (undo);
    assert_tree_unchanged (candidate, candidate_before);
    g_autofree char *only_candidate = only_failed_initializer (fixture);
    g_assert_cmpstr (only_candidate, ==, candidate);
    g_assert_cmpuint (inventory (fixture->recovery).transactions, ==, 0);
    g_free (race.public_path);
    fixture_free (fixture);
}

static void
initialization_cancellation_not_latched (void)
{
    Fixture *fixture = fixture_new ();
    selected_bucket = "b2";
    g_autofree char *source_path = g_build_filename (fixture->sources, "item", NULL);
    g_autofree char *destination_path = g_build_filename (fixture->destination, "item", NULL);
    create_file (source_path, "cancelled initialization can be retried in the same guard\n");
    NemoTransferGuard *guard = new_guard (fixture, fixture->destination);
    g_autoptr (GFile) source = g_file_new_for_path (source_path);
    g_autoptr (GFile) destination = g_file_new_for_path (destination_path);
    g_autoptr (GCancellable) cancel = g_cancellable_new ();
    NamespaceRace race = {
        .fixture = fixture,
        .public_path = g_strdup (fixture->recovery),
        .cancel_initialization = cancel,
        .observing = TRUE
    };
    namespace_race = &race;
    g_autoptr (GError) error = NULL;
    NemoTransferTransaction *transaction =
        nemo_transfer_transaction_new (guard, source, destination, cancel, &error);
    namespace_race = NULL;
    g_assert_null (transaction);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint (race.cancellations, ==, 1);
    g_assert_false (g_file_test (destination_path, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (fixture->recovery, G_FILE_TEST_EXISTS));
    g_autofree char *candidate = only_failed_initializer (fixture);
    g_autofree char *candidate_before = snapshot_tree (candidate);
    /* No guard_release here: cancellation must not poison this job's namespace. */
    NemoTransferUndo *undo =
        finish_copy (publish_copy (guard, source_path, destination_path, FALSE, NULL));
    release_guard (guard);
    release_undo (undo);
    assert_tree_unchanged (candidate, candidate_before);
    g_free (race.public_path);
    fixture_free (fixture);
}

typedef enum {
    ROOT_CONTENT_CHANGED,
    ROOT_INODE_CHANGED,
    BUCKET_CONTENT_CHANGED,
    BUCKET_INODE_CHANGED,
    BUCKET_OWNER_INODE_CHANGED
} NamespaceChange;

static void
replace_inode (const char *path)
{
    g_autofree char *contents = read_file (path);
    g_autofree char *replacement = g_strconcat (path, "-replacement", NULL);
    struct stat before, after;
    g_assert_cmpint (g_lstat (path, &before), ==, 0);
    create_file (replacement, contents);
    g_assert_cmpint (g_rename (replacement, path), ==, 0);
    g_assert_cmpint (g_lstat (path, &after), ==, 0);
    g_assert_cmpuint (before.st_ino, !=, after.st_ino);
}

static void
change_byte (const char *path, gsize offset)
{
    g_autofree char *contents = read_file (path);
    g_assert_cmpuint (offset, <, strlen (contents));
    contents[offset] = contents[offset] == 'a' ? 'b' : 'a';
    struct stat before, after;
    g_assert_cmpint (g_lstat (path, &before), ==, 0);
    int fd = g_open (path, O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0);
    g_assert_cmpint (fd, >=, 0);
    write_fd (fd, contents);
    /* Keep inode, length, and mtime: the checksum/ctime guard must still notice. */
    struct timespec times[2] = { before.st_atim, before.st_mtim };
    g_assert_cmpint (futimens (fd, times), ==, 0);
    g_assert_cmpint (close (fd), ==, 0);
    g_assert_cmpint (g_lstat (path, &after), ==, 0);
    g_assert_cmpuint (before.st_ino, ==, after.st_ino);
    g_assert_cmpint (before.st_size, ==, after.st_size);
}

static void
undo_preflight_changed_namespace (gconstpointer data)
{
    NamespaceChange kind = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new ();
    NemoTransferGuard *guard = new_guard (fixture, fixture->destination);
    NemoTransferUndo *history[2];
    for (guint i = 0; i < G_N_ELEMENTS (history); i++) {
        selected_bucket = i ? "22" : "11";
        g_autofree char *name = g_strdup_printf ("public-%u", i);
        g_autofree char *source = g_build_filename (fixture->sources, name, NULL);
        g_autofree char *destination = g_build_filename (fixture->destination, name, NULL);
        create_file (source, name);
        history[i] = finish_copy (publish_copy (guard, source, destination, FALSE, NULL));
    }
    selected_bucket = NULL;
    release_guard (guard);
    g_assert_cmpuint (inventory (fixture->recovery).transactions, ==, 0);
    g_autofree char *root_owner = g_build_filename (fixture->recovery, "owner", NULL);
    g_autofree char *bucket = g_build_filename (fixture->recovery, "22", NULL);
    g_autofree char *bucket_owner = g_build_filename (bucket, "owner", NULL);
    if (kind == ROOT_CONTENT_CHANGED) {
        g_autofree char *contents = read_file (root_owner);
        char *token = strstr (contents, "token=");
        g_assert_nonnull (token);
        change_byte (root_owner, token + strlen ("token=") - contents);
    } else if (kind == ROOT_INODE_CHANGED) {
        replace_inode (root_owner);
    } else if (kind == BUCKET_CONTENT_CHANGED) {
        g_autofree char *contents = read_file (bucket_owner);
        char *hash = strstr (contents, "container-sha256=");
        g_assert_nonnull (hash);
        change_byte (bucket_owner, hash + strlen ("container-sha256=") - contents);
    } else if (kind == BUCKET_OWNER_INODE_CHANGED) {
        replace_inode (bucket_owner);
    } else {
        g_autofree char *contents = read_file (bucket_owner);
        g_autofree char *retired = g_build_filename (fixture->path, "retired-bucket", NULL);
        g_assert_cmpint (g_rename (bucket, retired), ==, 0);
        g_assert_cmpint (g_mkdir (bucket, 0700), ==, 0);
        create_file (bucket_owner, contents);
    }
    g_autofree char *before = snapshot_tree (fixture->path);
    guint passed = 0;
    guint applied = 0;
    gboolean all_valid = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS (history); i++) {
        g_autoptr (GError) error = NULL;
        gboolean ok = nemo_transfer_undo_check (history[i], FALSE, NULL, &error);
        gboolean expected = i == 0 && kind >= BUCKET_CONTENT_CHANGED;
        g_assert_cmpint (ok, ==, expected);
        if (ok) {
            g_assert_no_error (error);
            passed++;
        } else {
            g_assert_nonnull (error);
            g_assert_cmpuint (error->domain, ==, G_IO_ERROR);
            g_assert_nonnull (strstr (error->message, "before changing this item"));
        }
        all_valid &= ok;
        assert_tree_unchanged (fixture->path, before);
    }
    /* Match the manager's all-record preflight: no apply before all checks pass. */
    if (all_valid) {
        for (guint i = 0; i < G_N_ELEMENTS (history); i++) {
            g_autoptr (GError) error = NULL;
            g_assert_true (nemo_transfer_undo_apply (history[i], FALSE, NULL, &error));
            g_assert_no_error (error);
            applied++;
        }
    }
    g_assert_cmpuint (passed, ==, kind >= BUCKET_CONTENT_CHANGED ? 1 : 0);
    g_assert_cmpuint (applied, ==, 0);
    for (guint i = 0; i < G_N_ELEMENTS (history); i++)
        release_undo (history[i]);
    assert_tree_unchanged (fixture->path, before);
    fixture_free (fixture);
}

typedef enum {
    RECORD_CONTENT_CHANGED,
    RECORD_INODE_CHANGED,
    UNKNOWN_RECORD,
    UNKNOWN_DIRECTORY
} MetadataChange;

static void
metadata_cleanup_preserves_changes (gconstpointer data)
{
    MetadataChange kind = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new ();
    g_autofree char *source = g_build_filename (fixture->sources, "item", NULL);
    g_autofree char *destination = g_build_filename (fixture->destination, "item", NULL);
    create_file (source, "successfully published copy\n");
    NemoTransferGuard *guard = new_guard (fixture, fixture->destination);
    char *transaction_path = NULL;
    NemoTransferTransaction *transaction =
        publish_copy (guard, source, destination, FALSE, &transaction_path);
    g_autofree char *directory = transaction_path;
    g_autofree char *prepared = g_build_filename (directory, "record-prepared", NULL);
    g_autofree char *published = g_build_filename (directory, "record-published", NULL);
    g_assert_true (g_file_test (prepared, G_FILE_TEST_IS_REGULAR));
    g_assert_true (g_file_test (published, G_FILE_TEST_IS_REGULAR));
    if (kind == RECORD_CONTENT_CHANGED)
        change_byte (prepared, 0);
    else if (kind == RECORD_INODE_CHANGED)
        replace_inode (prepared);
    else {
        g_autofree char *unknown = g_build_filename (directory, "record-not-owned", NULL);
        if (kind == UNKNOWN_DIRECTORY) {
            g_assert_cmpint (g_mkdir (unknown, 0700), ==, 0);
            g_autofree char *child = g_build_filename (unknown, "data", NULL);
            create_file (child, "unknown nested data is not metadata\n");
        } else {
            create_file (unknown, "record-like name does not establish ownership\n");
        }
    }
    g_autofree char *before = snapshot_tree (fixture->path);
    NemoTransferUndo *undo = finish_copy (transaction);
    assert_tree_unchanged (fixture->path, before);
    g_autofree char *details = nemo_transfer_guard_take_details (guard);
    g_assert_nonnull (strstr (details, directory));
    g_assert_nonnull (strstr (details, "changed entries"));
    release_guard (guard);
    release_undo (undo);
    assert_tree_unchanged (fixture->path, before);
    g_assert_cmpuint (inventory (fixture->recovery).transactions, ==, 1);
    fixture_free (fixture);
}

static void
undo_redo_cleaned_metadata (void)
{
    Fixture *fixture = fixture_new ();
    selected_bucket = "63";
    g_autofree char *source = g_build_filename (fixture->sources, "item", NULL);
    g_autofree char *destination = g_build_filename (fixture->destination, "item", NULL);
    const char *payload = "undo and redo preserve copied bytes\n";
    create_file (source, payload);
    NemoTransferGuard *guard = new_guard (fixture, fixture->destination);
    NemoTransferUndo *undo = finish_copy (publish_copy (guard, source, destination, FALSE, NULL));
    release_guard (guard);
    g_assert_cmpuint (inventory (fixture->recovery).transactions, ==, 0);
    g_autofree char *before = snapshot_tree (fixture->path);
    g_autoptr (GError) error = NULL;
    g_assert_true (nemo_transfer_undo_check (undo, FALSE, NULL, &error));
    g_assert_no_error (error);
    assert_tree_unchanged (fixture->path, before);
    g_assert_true (nemo_transfer_undo_apply (undo, FALSE, NULL, &error));
    g_assert_no_error (error);
    assert_contents (source, payload);
    g_assert_false (g_file_test (destination, G_FILE_TEST_EXISTS));
    g_assert_cmpuint (inventory (fixture->recovery).transactions, ==, 1);
    g_clear_pointer (&before, g_free);
    before = snapshot_tree (fixture->path);
    g_assert_true (nemo_transfer_undo_check (undo, TRUE, NULL, &error));
    g_assert_no_error (error);
    assert_tree_unchanged (fixture->path, before);
    g_assert_true (nemo_transfer_undo_apply (undo, TRUE, NULL, &error));
    g_assert_no_error (error);
    assert_contents (source, payload);
    assert_contents (destination, payload);
    g_assert_cmpuint (inventory (fixture->recovery).transactions, ==, 0);
    release_undo (undo);
    fixture_free (fixture);
}

int
main (int argc, char **argv)
{
    g_setenv ("GIO_USE_VFS", "local", TRUE);
    g_test_init (&argc, &argv, NULL);
    g_test_add_data_func ("/transfer-recovery/batch/replacements-retain-every-payload",
                          GINT_TO_POINTER (TRUE), batch_copy);
    g_test_add_data_func ("/transfer-recovery/batch/ordinary-bounded-metadata",
                          GINT_TO_POINTER (FALSE), batch_copy);
    g_test_add_func ("/transfer-recovery/queue-roots/1500-low-fd-copy-and-late-swap",
                     queued_roots_low_fd);
    g_test_add_func ("/transfer-recovery/owner/portable-across-jobs", portable_owner);
    g_test_add_data_func ("/transfer-recovery/refuse/unknown-root-marker",
                          GINT_TO_POINTER (UNKNOWN_ROOT_MARKER), unknown_namespace);
    g_test_add_data_func ("/transfer-recovery/refuse/unknown-bucket-file",
                          GINT_TO_POINTER (UNKNOWN_BUCKET_FILE), unknown_namespace);
    g_test_add_data_func ("/transfer-recovery/refuse/unknown-bucket-owner",
                          GINT_TO_POINTER (UNKNOWN_BUCKET_OWNER), unknown_namespace);
    g_test_add_data_func ("/transfer-recovery/refuse/missing-bucket-owner",
                          GINT_TO_POINTER (UNKNOWN_BUCKET_NO_OWNER), unknown_namespace);
    g_test_add_data_func ("/transfer-recovery/refuse/bucket-bound-to-other-root",
                          GINT_TO_POINTER (UNKNOWN_BUCKET_WRONG_ROOT), unknown_namespace);
    g_test_add_data_func ("/transfer-recovery/mount-boundary/container",
                          GUINT_TO_POINTER (0), recovery_mount_boundary);
    g_test_add_data_func ("/transfer-recovery/mount-boundary/bucket",
                          GUINT_TO_POINTER (1), recovery_mount_boundary);
    g_test_add_data_func ("/transfer-recovery/mount-boundary/transaction",
                          GUINT_TO_POINTER (2), recovery_mount_boundary);
    g_test_add_func ("/transfer-recovery/mount-cache/per-guard-invalidation",
                     mount_table_cache_invalidation);
    g_test_add_func ("/transfer-recovery/mount-cache/real-monitor",
                     mount_table_cache_real_monitor);
    g_test_add_func ("/transfer-recovery/mount-cache/clock-expiry-without-main-loop",
                     mount_table_cache_clock_expiry);
    g_test_add_data_func ("/transfer-recovery/initialization/concurrent-container",
                          GINT_TO_POINTER (FALSE), concurrent_namespace_initialization);
    g_test_add_data_func ("/transfer-recovery/initialization/concurrent-bucket",
                          GINT_TO_POINTER (TRUE), concurrent_namespace_initialization);
    g_test_add_data_func ("/transfer-recovery/initialization/invalid-owner-no-move-fallback",
                          GINT_TO_POINTER (FALSE), namespace_refusal_disables_move_fallback);
    g_test_add_data_func ("/transfer-recovery/initialization/unsupported-publication-no-move-fallback",
                          GINT_TO_POINTER (TRUE), namespace_refusal_disables_move_fallback);
    g_test_add_data_func ("/transfer-recovery/initialization/owner-fsync-failure",
                          GUINT_TO_POINTER (2), namespace_refusal_disables_move_fallback);
    g_test_add_data_func ("/transfer-recovery/initialization/owner-close-failure",
                          GUINT_TO_POINTER (3), namespace_refusal_disables_move_fallback);
    g_test_add_func ("/transfer-recovery/initialization/swapped-losing-candidate",
                     initialization_preserves_swapped_candidate);
    g_test_add_data_func ("/transfer-recovery/initialization/failure-latched-until-release",
                          GINT_TO_POINTER (FALSE), initialization_failure_latched);
    g_test_add_data_func ("/transfer-recovery/initialization/foreign-failed-candidate-retained",
                          GINT_TO_POINTER (TRUE), initialization_failure_latched);
    g_test_add_func ("/transfer-recovery/initialization/cancellation-not-latched",
                     initialization_cancellation_not_latched);
    g_test_add_data_func ("/transfer-recovery/preflight/root-marker-content",
                          GINT_TO_POINTER (ROOT_CONTENT_CHANGED), undo_preflight_changed_namespace);
    g_test_add_data_func ("/transfer-recovery/preflight/root-marker-inode",
                          GINT_TO_POINTER (ROOT_INODE_CHANGED), undo_preflight_changed_namespace);
    g_test_add_data_func ("/transfer-recovery/preflight/bucket-marker-content",
                          GINT_TO_POINTER (BUCKET_CONTENT_CHANGED), undo_preflight_changed_namespace);
    g_test_add_data_func ("/transfer-recovery/preflight/bucket-directory-inode",
                          GINT_TO_POINTER (BUCKET_INODE_CHANGED), undo_preflight_changed_namespace);
    g_test_add_data_func ("/transfer-recovery/preflight/bucket-marker-inode",
                          GINT_TO_POINTER (BUCKET_OWNER_INODE_CHANGED), undo_preflight_changed_namespace);
    g_test_add_data_func ("/transfer-recovery/cleanup/changed-record-checksum",
                          GINT_TO_POINTER (RECORD_CONTENT_CHANGED), metadata_cleanup_preserves_changes);
    g_test_add_data_func ("/transfer-recovery/cleanup/changed-record-inode",
                          GINT_TO_POINTER (RECORD_INODE_CHANGED), metadata_cleanup_preserves_changes);
    g_test_add_data_func ("/transfer-recovery/cleanup/unknown-record",
                          GINT_TO_POINTER (UNKNOWN_RECORD), metadata_cleanup_preserves_changes);
    g_test_add_data_func ("/transfer-recovery/cleanup/unknown-directory",
                          GINT_TO_POINTER (UNKNOWN_DIRECTORY), metadata_cleanup_preserves_changes);
    g_test_add_func ("/transfer-recovery/undo-redo/cleaned-metadata", undo_redo_cleaned_metadata);
    return g_test_run ();
}
