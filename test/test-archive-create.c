/* Build the worker with NEMO_ARCHIVE_TEST_WORKER and the two archive_write_*
 * wrappers below to check the actual compression settings, not source text. */
#ifdef NEMO_ARCHIVE_TEST_WORKER
#include <archive.h>
#include <errno.h>
#include <string.h>

static int maximum_lzma2;
int __real_archive_write_set_options (struct archive *, const char *);
int __real_archive_write_open_fd (struct archive *, int);

int
__wrap_archive_write_set_options (struct archive *writer, const char *options)
{
    maximum_lzma2 = options &&
        strstr (options, "7zip:compression=lzma2") &&
        strstr (options, "7zip:compression-level=9");
    return __real_archive_write_set_options (writer, options);
}

int
__wrap_archive_write_open_fd (struct archive *writer, int fd)
{
    if (!maximum_lzma2) {
        archive_set_error (writer, EINVAL, "Regression: maximum LZMA2 compression was not selected");
        return ARCHIVE_FATAL;
    }
    return __real_archive_write_open_fd (writer, fd);
}

#else
#define _GNU_SOURCE
#include <config.h>
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <linux/magic.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <locale.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <libnemo-private/nemo-archive-create.h>
#include <libnemo-private/nemo-archive-file.h>
#include <libnemo-private/nemo-transfer-safety.h>

typedef struct {
    char *name;
    GBytes *bytes;
    char *link;
    struct stat original;
} Expected;

typedef struct {
    char *root;
    char *input;
    char *output;
    char *destination;
    struct stat identity;
    GHashTable *members;
    GList *sources;
    NemoProgressInfo *progress;
    gboolean done;
    gboolean success;
    NemoArchiveCreateResult result;
    GError *error;
} Fixture;

typedef enum {
    FAULT_NONE,
    CANCEL_BEFORE,
    CANCEL_COMPRESS,
    CANCEL_VERIFY,
    FAIL_WRITE,
    FAIL_VERIFY,
    CORRUPT_VERIFY,
    FAIL_SYNC,
    FAIL_NOSPACE,
    FAIL_PUBLISH,
    CHANGE_BEFORE_PUBLISH,
    REPLACE_BEFORE_PUBLISH,
    CHILD_BEFORE_PUBLISH,
    CHANGE_AFTER_PUBLISH,
    REPLACE_AFTER_PUBLISH,
    CHILD_AFTER_PUBLISH,
    FAIL_RETIRE_SECOND,
    FAIL_CAPTURE_UNLINK,
    EXIT_WORKER
} Fault;

static struct {
    Fixture *fixture;
    Fault fault;
    guint injected;
    guint headers;
    guint data_blocks;
    guint retirements;
    gboolean verified_eof;
    gboolean inspecting;
    gpointer cancellable;
} operation;
static GByteArray *corrupted_block;
static char *test_program;

static void inspect_archive (Fixture *fixture);

static void
write_new (const char *path, const void *bytes, gsize length)
{
    int fd = open (path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    g_assert_cmpint (fd, >=, 0);
    gsize offset = 0;
    while (offset < length) {
        ssize_t written = write (fd, (const char *) bytes + offset, length - offset);
        if (written < 0 && errno == EINTR)
            continue;
        g_assert_cmpint (written, >, 0);
        offset += written;
    }
    g_assert_cmpint (close (fd), ==, 0);
}

static void
cancel_operation (void)
{
    GCancellable *cancel;
    /* The worker can reach a hook before async() returns its progress object. */
    while (!(cancel = g_atomic_pointer_get (&operation.cancellable)))
        g_usleep (1000);
    g_cancellable_cancel (cancel);
    operation.injected++;
}

static void
mutate_source (void)
{
    Fixture *fixture = operation.fixture;
    g_autofree char *victim = g_build_filename (fixture->input, "victim.txt", NULL);
    if (operation.fault == CHILD_BEFORE_PUBLISH || operation.fault == CHILD_AFTER_PUBLISH) {
        g_autofree char *child = g_build_filename (fixture->input, "folder", "new-child.txt", NULL);
        write_new (child, "not in archive", 14);
    } else if (operation.fault == REPLACE_BEFORE_PUBLISH || operation.fault == REPLACE_AFTER_PUBLISH) {
        g_autofree char *saved = g_build_filename (fixture->input, "saved-original.txt", NULL);
        g_assert_cmpint (rename (victim, saved), ==, 0);
        write_new (victim, "replacement survives", 20);
    } else {
        int fd = open (victim, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
        g_assert_cmpint (fd, >=, 0);
        g_assert_cmpint (pwrite (fd, "changed", 7, 0), ==, 7);
        g_assert_cmpint (close (fd), ==, 0);
    }
    operation.injected++;
}

la_ssize_t __real_archive_write_data (struct archive *, const void *, size_t);
int __real_archive_read_next_header (struct archive *, struct archive_entry **);
int __real_archive_read_data_block (struct archive *, const void **, size_t *, la_int64_t *);
int __real_fsync (int);
int __real_renameat2 (int, const char *, int, const char *, unsigned int);
int __real_unlinkat (int, const char *, int);
gboolean __real_nemo_transfer_transaction_retire_archived (
    NemoTransferTransaction *, GFile *, const struct stat *, const char *,
    const char *, GCancellable *, GError **);

GSubprocess *
__wrap_g_subprocess_launcher_spawn (GSubprocessLauncher *launcher, GError **error,
                                    const char *argv0, ...)
{
    if (operation.fixture && operation.fault == EXIT_WORKER) {
        operation.injected++;
        const char *argv[] = { test_program, "--exit-worker", NULL };
        return g_subprocess_launcher_spawnv (launcher, argv, error);
    }
    g_autoptr (GPtrArray) argv = g_ptr_array_new ();
    g_ptr_array_add (argv, (gpointer) argv0);
    va_list args;
    va_start (args, argv0);
    const char *arg;
    while ((arg = va_arg (args, const char *)))
        g_ptr_array_add (argv, (gpointer) arg);
    va_end (args);
    g_ptr_array_add (argv, NULL);
    return g_subprocess_launcher_spawnv (launcher, (const char *const *) argv->pdata, error);
}

la_ssize_t
__wrap_archive_write_data (struct archive *writer, const void *bytes, size_t size)
{
    if (operation.fixture && !operation.injected) {
        if (operation.fault == FAIL_WRITE || operation.fault == FAIL_NOSPACE) {
            operation.injected++;
            archive_set_error (writer, operation.fault == FAIL_NOSPACE ? ENOSPC : EIO,
                               "Injected archive producer write error");
            return -1;
        }
        if (operation.fault == CANCEL_COMPRESS)
            cancel_operation ();
    }
    return __real_archive_write_data (writer, bytes, size);
}

int
__wrap_archive_read_next_header (struct archive *reader, struct archive_entry **entry)
{
    int status = __real_archive_read_next_header (reader, entry);
    if (operation.fixture && !operation.inspecting) {
        if (status == ARCHIVE_OK)
            operation.headers++;
        if (status == ARCHIVE_EOF) {
            operation.verified_eof = TRUE;
            if (!operation.injected && operation.fault >= CHANGE_BEFORE_PUBLISH &&
                operation.fault <= CHILD_BEFORE_PUBLISH)
                mutate_source ();
        }
    }
    return status;
}

int
__wrap_archive_read_data_block (struct archive *reader, const void **bytes,
                                 size_t *size, la_int64_t *offset)
{
    int status = __real_archive_read_data_block (reader, bytes, size, offset);
    if (operation.fixture && !operation.inspecting && status == ARCHIVE_OK && *size) {
        operation.data_blocks++;
        if (!operation.injected) {
            if (operation.fault == CANCEL_VERIFY)
                cancel_operation ();
            else if (operation.fault == FAIL_VERIFY) {
                operation.injected++;
                archive_set_error (reader, EIO, "Injected independent readback failure");
                return ARCHIVE_FATAL;
            } else if (operation.fault == CORRUPT_VERIFY) {
                corrupted_block = g_byte_array_sized_new (*size);
                g_byte_array_set_size (corrupted_block, *size);
                memcpy (corrupted_block->data, *bytes, *size);
                corrupted_block->data[0] ^= 0x80;
                *bytes = corrupted_block->data;
                operation.injected++;
            }
        }
    }
    return status;
}

int
__wrap_fsync (int fd)
{
    if (operation.fixture && operation.fault == FAIL_SYNC && !operation.injected) {
        unsigned char signature[6];
        static const unsigned char seven_zip[] = { '7', 'z', 0xbc, 0xaf, 0x27, 0x1c };
        /* Target only the completed staging archive, not unrelated SQLite,
         * settings, recovery-journal or fixture descriptors. */
        if (pread (fd, signature, sizeof signature, 0) == sizeof signature &&
            memcmp (signature, seven_zip, sizeof signature) == 0) {
            operation.injected++;
            errno = EIO;
            return -1;
        }
    }
    return __real_fsync (fd);
}

int
__wrap_renameat2 (int from_fd, const char *from, int to_fd, const char *to, unsigned int flags)
{
    if (operation.fixture && operation.fault == FAIL_PUBLISH && !operation.injected &&
        g_strcmp0 (from, "payload") == 0) {
        g_autofree char *descriptor = g_strdup_printf ("/proc/self/fd/%d", to_fd);
        g_autofree char *parent = g_file_read_link (descriptor, NULL);
        g_autofree char *name = g_path_get_basename (operation.fixture->destination);
        if (g_strcmp0 (parent, operation.fixture->output) == 0 && g_strcmp0 (to, name) == 0) {
            operation.injected++;
            errno = EIO;
            return -1;
        }
    }
    return __real_renameat2 (from_fd, from, to_fd, to, flags);
}

int
__wrap_unlinkat (int fd, const char *path, int flags)
{
    if (operation.fixture && operation.fault == FAIL_CAPTURE_UNLINK &&
        !operation.injected && operation.retirements > 0 &&
        g_strcmp0 (path, "captured-source") == 0) {
        operation.injected++;
        errno = EIO;
        return -1;
    }
    return __real_unlinkat (fd, path, flags);
}

gboolean
__wrap_nemo_transfer_transaction_retire_archived (
    NemoTransferTransaction *transaction, GFile *source, const struct stat *identity,
    const char *sha256, const char *link, GCancellable *cancel, GError **error)
{
    if (operation.fixture) {
        g_assert_true (operation.verified_eof);
        g_assert_cmpuint (operation.data_blocks, >, 0);
        if (operation.retirements++ == 0) {
            /* Independent readback before the first destructive operation
             * proves a complete published archive already exists. */
            operation.inspecting = TRUE;
            inspect_archive (operation.fixture);
            operation.inspecting = FALSE;
            if (!operation.injected && operation.fault >= CHANGE_AFTER_PUBLISH &&
                operation.fault <= CHILD_AFTER_PUBLISH)
                mutate_source ();
        }
        if (operation.fault == FAIL_RETIRE_SECOND && operation.retirements == 2) {
            operation.injected++;
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "Injected retirement failure after one safe removal");
            return FALSE;
        }
    }
    return __real_nemo_transfer_transaction_retire_archived (
        transaction, source, identity, sha256, link, cancel, error);
}

static void
expected_free (gpointer data)
{
    Expected *expected = data;
    g_free (expected->name);
    g_clear_pointer (&expected->bytes, g_bytes_unref);
    g_free (expected->link);
    g_free (expected);
}

static Fixture *
fixture_new (void)
{
    Fixture *fixture = g_new0 (Fixture, 1);
    g_autofree char *cwd = g_get_current_dir ();
    fixture->root = g_build_filename (cwd, ".nemo-archive-create-test-XXXXXX", NULL);
    g_assert_nonnull (g_mkdtemp_full (fixture->root, 0700));
    g_test_message ("Disposable archive fixture: %s", fixture->root);
    g_assert_cmpint (lstat (fixture->root, &fixture->identity), ==, 0);
    fixture->input = g_build_filename (fixture->root, "inputs", NULL);
    fixture->output = g_build_filename (fixture->root, "output", NULL);
    fixture->destination = g_build_filename (fixture->output, "result.7z", NULL);
    g_assert_cmpint (mkdir (fixture->input, 0700), ==, 0);
    g_assert_cmpint (mkdir (fixture->output, 0700), ==, 0);
    fixture->members = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, expected_free);
    return fixture;
}

static void
add_member (Fixture *fixture, const char *name, const char *contents, const char *link)
{
    Expected *expected = g_new0 (Expected, 1);
    expected->name = g_strdup (name);
    g_autofree char *path = g_build_filename (fixture->input, name, NULL);
    if (link) {
        expected->link = g_strdup (link);
        g_assert_cmpint (symlink (link, path), ==, 0);
    } else if (contents) {
        expected->bytes = g_bytes_new (contents, strlen (contents));
        write_new (path, contents, strlen (contents));
        g_assert_cmpint (chmod (path, 06710), ==, 0);
    } else {
        g_assert_cmpint (mkdir (path, 0710), ==, 0);
    }
    g_hash_table_insert (fixture->members, expected->name, expected);
    if (!strchr (name, '/'))
        fixture->sources = g_list_append (fixture->sources, g_file_new_for_path (path));
}

static void
snapshot_metadata (Fixture *fixture)
{
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init (&iter, fixture->members);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        Expected *expected = value;
        g_autofree char *path = g_build_filename (fixture->input, expected->name, NULL);
        struct timespec times[] = { { 1700000000, 123456700 }, { 1700000000, 123456700 } };
        g_assert_cmpint (utimensat (AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW), ==, 0);
        g_assert_cmpint (lstat (path, &expected->original), ==, 0);
    }
}

static void
make_small_dataset (Fixture *fixture)
{
    add_member (fixture, "victim.txt", "original victim bytes", NULL);
    add_member (fixture, "second.txt", "another independent input", NULL);
    add_member (fixture, "folder", NULL, NULL);
    add_member (fixture, "folder/child.txt", "recursive input bytes", NULL);
    snapshot_metadata (fixture);
}

static void
add_source_metadata (Fixture *fixture)
{
    g_autofree char *path = g_build_filename (fixture->input, "résumé 日本語.txt", NULL);
    g_assert_cmpint (setxattr (path, "user.nemo-archive-regression", "private", 7, 0), ==, 0);
    struct {
        struct posix_acl_xattr_header header;
        struct posix_acl_xattr_entry entries[5];
    } acl = {
        .header.a_version = GUINT32_TO_LE (POSIX_ACL_XATTR_VERSION),
        .entries = {
            { GUINT16_TO_LE (ACL_USER_OBJ), GUINT16_TO_LE (7), GUINT32_TO_LE (ACL_UNDEFINED_ID) },
            { GUINT16_TO_LE (ACL_USER), GUINT16_TO_LE (4), GUINT32_TO_LE (65534) },
            { GUINT16_TO_LE (ACL_GROUP_OBJ), GUINT16_TO_LE (1), GUINT32_TO_LE (ACL_UNDEFINED_ID) },
            { GUINT16_TO_LE (ACL_MASK), GUINT16_TO_LE (5), GUINT32_TO_LE (ACL_UNDEFINED_ID) },
            { GUINT16_TO_LE (ACL_OTHER), GUINT16_TO_LE (0), GUINT32_TO_LE (ACL_UNDEFINED_ID) }
        }
    };
    g_assert_cmpint (setxattr (path, "system.posix_acl_access", &acl, sizeof acl, 0), ==, 0);
    if (geteuid () == 0)
        g_assert_cmpint (chown (path, 65534, 65534), ==, 0);
    g_assert_cmpint (chmod (path, 06710), ==, 0);
    struct stat identity;
    g_assert_cmpint (lstat (path, &identity), ==, 0);
    g_assert_cmpuint (identity.st_uid, !=, 0);
    g_assert_cmpuint (identity.st_gid, !=, 0);
    g_assert_cmpuint (identity.st_mode & 07777, ==, 06710);
    g_assert_cmpint (getxattr (path, "system.posix_acl_access", NULL, 0), >, 0);
}

static void
make_full_dataset (Fixture *fixture)
{
    add_member (fixture, "résumé 日本語.txt", "UTF-8 source contents: élève 日本語\n", NULL);
    add_member (fixture, "-leading option.txt", "not a command-line option\n", NULL);
    add_member (fixture, "name with spaces", "white space is preserved\n", NULL);
    add_member (fixture, "tree 日本語", NULL, NULL);
    add_member (fixture, "tree 日本語/.hidden", "hidden contents\n", NULL);
    add_member (fixture, "tree 日本語/empty folder", NULL, NULL);
    add_member (fixture, "tree 日本語/zero bytes", "", NULL);
    add_member (fixture, "tree 日本語/nested", NULL, NULL);
    add_member (fixture, "tree 日本語/nested/child", "nested contents\n", NULL);
    add_member (fixture, "symbolic link", NULL, "résumé 日本語.txt");
    add_source_metadata (fixture);
    snapshot_metadata (fixture);
}

static void
assert_original (Fixture *fixture, Expected *expected)
{
    g_autofree char *path = g_build_filename (fixture->input, expected->name, NULL);
    struct stat identity;
    g_assert_cmpint (lstat (path, &identity), ==, 0);
    g_assert_cmpuint (identity.st_dev, ==, expected->original.st_dev);
    g_assert_cmpuint (identity.st_ino, ==, expected->original.st_ino);
    g_assert_cmpuint (identity.st_mode, ==, expected->original.st_mode);
    g_assert_cmpuint (identity.st_uid, ==, expected->original.st_uid);
    g_assert_cmpuint (identity.st_gid, ==, expected->original.st_gid);
    if (!S_ISDIR (identity.st_mode)) {
        g_assert_cmpint (identity.st_mtim.tv_sec, ==, expected->original.st_mtim.tv_sec);
        g_assert_cmpint (identity.st_mtim.tv_nsec, ==, expected->original.st_mtim.tv_nsec);
    }
    if (expected->bytes) {
        g_autofree char *bytes = NULL;
        gsize length, expected_length;
        g_assert_true (g_file_get_contents (path, &bytes, &length, NULL));
        gconstpointer original = g_bytes_get_data (expected->bytes, &expected_length);
        g_assert_cmpmem (bytes, length, original, expected_length);
    } else if (expected->link) {
        g_autofree char *link = g_file_read_link (path, NULL);
        g_assert_cmpstr (link, ==, expected->link);
    }
}

static void
assert_all_originals (Fixture *fixture)
{
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init (&iter, fixture->members);
    while (g_hash_table_iter_next (&iter, NULL, &value))
        assert_original (fixture, value);
}

static void
inspect_archive (Fixture *fixture)
{
    struct archive *reader = archive_read_new ();
    g_assert_cmpint (archive_read_support_format_7zip (reader), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_read_open_filename (reader, fixture->destination, 32768), ==, ARCHIVE_OK);
    g_autoptr (GHashTable) seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    struct archive_entry *entry;
    int status;
    while ((status = archive_read_next_header (reader, &entry)) == ARCHIVE_OK) {
        g_assert_cmpint (archive_format (reader), ==, ARCHIVE_FORMAT_7ZIP);
        const char *pathname = archive_entry_pathname_utf8 (entry);
        g_assert_nonnull (pathname);
        g_autofree char *name = g_strdup (pathname);
        gsize length = strlen (name);
        if (length && name[length - 1] == '/')
            name[length - 1] = '\0';
        Expected *expected = g_hash_table_lookup (fixture->members, name);
        g_assert_nonnull (expected);
        g_assert_true (g_hash_table_add (seen, g_strdup (name)));
        mode_t type = expected->original.st_mode & S_IFMT;
        g_assert_cmpuint (archive_entry_filetype (entry), ==, type);
        g_assert_cmpuint (archive_entry_perm (entry), ==, type == S_IFDIR ? 0755 : 0644);
        g_assert_cmpint (archive_entry_uid (entry), ==, 0);
        g_assert_cmpint (archive_entry_gid (entry), ==, 0);
        g_assert_true (!archive_entry_uname (entry) || !*archive_entry_uname (entry));
        g_assert_true (!archive_entry_gname (entry) || !*archive_entry_gname (entry));
        g_assert_null (archive_entry_hardlink (entry));
        g_assert_cmpint (archive_entry_xattr_count (entry), ==, 0);
        g_assert_cmpint (archive_entry_acl_count (entry, ARCHIVE_ENTRY_ACL_TYPE_ACCESS |
                         ARCHIVE_ENTRY_ACL_TYPE_DEFAULT | ARCHIVE_ENTRY_ACL_TYPE_NFS4), ==, 0);
        g_assert_cmpint (archive_entry_mtime (entry), ==, expected->original.st_mtim.tv_sec);
        g_assert_cmpint (archive_entry_mtime_nsec (entry) / 100, ==,
                         expected->original.st_mtim.tv_nsec / 100);
        if (expected->link) {
            g_assert_cmpstr (archive_entry_symlink_utf8 (entry), ==, expected->link);
            g_assert_cmpint (archive_read_data_skip (reader), ==, ARCHIVE_OK);
            continue;
        }
        g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);
        guint8 bytes[8192];
        la_ssize_t count;
        gsize total = 0;
        while ((count = archive_read_data (reader, bytes, sizeof bytes)) > 0) {
            g_checksum_update (checksum, bytes, count);
            total += count;
        }
        g_assert_cmpint (count, ==, 0);
        gsize expected_length = 0;
        gconstpointer original = expected->bytes ? g_bytes_get_data (expected->bytes, &expected_length) : "";
        g_autofree char *sha256 = g_compute_checksum_for_data (G_CHECKSUM_SHA256, original, expected_length);
        g_assert_cmpuint (total, ==, expected_length);
        g_assert_cmpstr (g_checksum_get_string (checksum), ==, sha256);
    }
    g_assert_cmpint (status, ==, ARCHIVE_EOF);
    g_assert_cmpuint (g_hash_table_size (seen), ==, g_hash_table_size (fixture->members));
    g_assert_cmpint (archive_read_close (reader), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_read_free (reader), ==, ARCHIVE_OK);
}

static void
archive_done (GObject *source, GAsyncResult *result, gpointer data)
{
    Fixture *fixture = data;
    g_assert_false (fixture->done);
    g_assert_true (source == G_OBJECT (fixture->progress));
    fixture->success = nemo_archive_create_finish (result, &fixture->result, &fixture->error);
    fixture->done = TRUE;
}

static void
run_job (Fixture *fixture, gboolean move, gboolean overwrite, Fault fault)
{
    operation.fixture = fixture;
    operation.fault = fault;
    g_autoptr (GFile) destination = g_file_new_for_path (fixture->destination);
    fixture->progress = nemo_archive_create_async (fixture->sources, destination, move,
                                                  overwrite, NULL, archive_done, fixture);
    g_assert_nonnull (fixture->progress);
    /* Check the public API owns the selected-file snapshot. */
    g_list_free_full (fixture->sources, g_object_unref);
    fixture->sources = NULL;
    GCancellable *cancel = nemo_progress_info_get_cancellable (fixture->progress);
    g_atomic_pointer_set (&operation.cancellable, cancel);
    if (fault == CANCEL_BEFORE)
        cancel_operation ();
    gint64 deadline = g_get_monotonic_time () + (fault == EXIT_WORKER ? 5000000 : 60000000);
    while (!fixture->done && g_get_monotonic_time () < deadline) {
        while (g_main_context_iteration (NULL, FALSE));
        g_usleep (1000);
    }
    g_assert_true (fixture->done);
    g_assert_true (nemo_progress_info_get_is_finished (fixture->progress));
    g_atomic_pointer_set (&operation.cancellable, NULL);
    g_object_unref (cancel);
    operation.fixture = NULL;
}

static void
remove_owned_entry (const char *path, dev_t device)
{
    struct stat identity;
    g_assert_cmpint (lstat (path, &identity), ==, 0);
    g_assert_cmpuint (identity.st_dev, ==, device);
    if (S_ISDIR (identity.st_mode)) {
        g_autoptr (GDir) directory = g_dir_open (path, 0, NULL);
        g_assert_nonnull (directory);
        const char *name;
        while ((name = g_dir_read_name (directory))) {
            g_autofree char *child = g_build_filename (path, name, NULL);
            remove_owned_entry (child, device);
        }
        g_assert_cmpint (rmdir (path), ==, 0);
    } else {
        g_assert_cmpint (unlink (path), ==, 0);
    }
}

static void
fixture_free (Fixture *fixture)
{
    g_assert_null (operation.fixture);
    g_list_free_full (fixture->sources, g_object_unref);
    g_clear_object (&fixture->progress);
    while (g_main_context_iteration (NULL, FALSE));
    g_clear_error (&fixture->error);
    g_hash_table_unref (fixture->members);
    struct stat identity;
    g_assert_cmpint (lstat (fixture->root, &identity), ==, 0);
    g_assert_true (S_ISDIR (identity.st_mode));
    g_assert_cmpuint (identity.st_dev, ==, fixture->identity.st_dev);
    g_assert_cmpuint (identity.st_ino, ==, fixture->identity.st_ino);
    /* Delete only the exact private fixture inode, without following links. */
    remove_owned_entry (fixture->root, identity.st_dev);
    g_free (fixture->root);
    g_free (fixture->input);
    g_free (fixture->output);
    g_free (fixture->destination);
    g_free (fixture);
    g_clear_pointer (&corrupted_block, g_byte_array_unref);
    memset (&operation, 0, sizeof operation);
}

static void
assert_success (Fixture *fixture, gboolean move)
{
    if (fixture->error)
        g_test_message ("%s", fixture->error->message);
    g_assert_no_error (fixture->error);
    g_assert_true (fixture->success);
    g_assert_true (fixture->result.published);
    g_assert_true (fixture->result.archive_ok);
    g_assert_cmpint (fixture->result.sources_removed, ==, move);
    g_assert_cmpuint (fixture->result.archived_items, ==, g_hash_table_size (fixture->members));
    g_assert_cmpuint (fixture->result.removed_items, ==,
                      move ? g_hash_table_size (fixture->members) : 0);
    NemoProgressResult progress;
    g_assert_true (nemo_progress_info_get_result (fixture->progress, &progress));
    g_assert_cmpint (progress.outcome, ==, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_true (progress.verification_requested);
    g_assert_cmpuint (progress.checksum_verified_files, ==, 1);
    inspect_archive (fixture);
}

static void
test_roundtrip (gconstpointer data)
{
    gboolean move = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new ();
    make_full_dataset (fixture);
    run_job (fixture, move, FALSE, FAULT_NONE);
    assert_success (fixture, move);
    if (move) {
        GHashTableIter iter;
        gpointer value;
        g_hash_table_iter_init (&iter, fixture->members);
        while (g_hash_table_iter_next (&iter, NULL, &value)) {
            Expected *expected = value;
            g_autofree char *path = g_build_filename (fixture->input, expected->name, NULL);
            struct stat identity;
            g_assert_cmpint (lstat (path, &identity), ==, -1);
            g_assert_cmpint (errno, ==, ENOENT);
        }
        g_assert_cmpuint (operation.retirements, ==, g_hash_table_size (fixture->members));
    } else {
        assert_all_originals (fixture);
        GHashTableIter iter;
        gpointer value;
        g_hash_table_iter_init (&iter, fixture->members);
        while (g_hash_table_iter_next (&iter, NULL, &value)) {
            Expected *expected = value;
            g_autofree char *path = g_build_filename (fixture->input, expected->name, NULL);
            struct stat identity;
            g_assert_cmpint (lstat (path, &identity), ==, 0);
            g_assert_cmpint (identity.st_mtim.tv_sec, ==, expected->original.st_mtim.tv_sec);
            g_assert_cmpint (identity.st_mtim.tv_nsec, ==, expected->original.st_mtim.tv_nsec);
        }
        g_autofree char *path = g_build_filename (fixture->input, "résumé 日本語.txt", NULL);
        char attribute[7];
        g_assert_cmpint (getxattr (path, "user.nemo-archive-regression", attribute, sizeof attribute), ==, 7);
        g_assert_cmpmem (attribute, 7, "private", 7);
        g_assert_cmpint (getxattr (path, "system.posix_acl_access", NULL, 0), >, 0);
    }
    fixture_free (fixture);
}

static void
test_uppercase_extension (void)
{
    Fixture *fixture = fixture_new ();
    make_small_dataset (fixture);
    g_free (fixture->destination);
    fixture->destination = g_build_filename (fixture->output, "result.7Z", NULL);
    run_job (fixture, FALSE, FALSE, FAULT_NONE);
    assert_success (fixture, FALSE);
    assert_all_originals (fixture);
    fixture_free (fixture);
}

static void
assert_no_retirement_failure (Fixture *fixture)
{
    g_assert_false (fixture->success);
    g_assert_nonnull (fixture->error);
    g_assert_false (fixture->result.archive_ok);
    g_assert_false (fixture->result.sources_removed);
    g_assert_cmpuint (fixture->result.removed_items, ==, 0);
    g_assert_cmpuint (operation.retirements, ==, 0);
}

static void
assert_unpublished_failure (Fixture *fixture)
{
    assert_no_retirement_failure (fixture);
    g_assert_false (fixture->result.published);
}

static void
test_fault (gconstpointer data)
{
    Fault fault = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new ();
    make_small_dataset (fixture);
    run_job (fixture, TRUE, FALSE, fault);
    g_assert_cmpuint (operation.injected, ==, 1);
    /* A failed rename may conservatively report an uncertain namespace as
     * published. It must never authorize source retirement or archive success. */
    if (fault == FAIL_PUBLISH)
        assert_no_retirement_failure (fixture);
    else
        assert_unpublished_failure (fixture);
    if (fault >= CANCEL_BEFORE && fault <= CANCEL_VERIFY)
        g_assert_error (fixture->error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_false (g_file_test (fixture->destination, G_FILE_TEST_EXISTS));
    assert_all_originals (fixture);
    fixture_free (fixture);
}

static gboolean
contains_old_destination (const char *path)
{
    struct stat identity;
    g_assert_cmpint (lstat (path, &identity), ==, 0);
    if (S_ISREG (identity.st_mode) && identity.st_size == 21) {
        g_autofree char *bytes = NULL;
        g_assert_true (g_file_get_contents (path, &bytes, NULL, NULL));
        return strcmp (bytes, "previous archive data") == 0;
    }
    if (S_ISDIR (identity.st_mode)) {
        g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
        g_assert_nonnull (dir);
        const char *name;
        while ((name = g_dir_read_name (dir))) {
            g_autofree char *child = g_build_filename (path, name, NULL);
            if (contains_old_destination (child))
                return TRUE;
        }
    }
    return FALSE;
}

static void
test_existing (gconstpointer data)
{
    int mode = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new ();
    make_small_dataset (fixture);
    write_new (fixture->destination, "previous archive data", 21);
    struct stat before, after;
    g_assert_cmpint (lstat (fixture->destination, &before), ==, 0);
    run_job (fixture, FALSE, mode != 0,
             mode == 2 ? FAIL_WRITE : mode == 3 ? FAIL_PUBLISH : FAULT_NONE);
    assert_all_originals (fixture);
    if (mode == 1) {
        assert_success (fixture, FALSE);
        /* Safe overwrite must retain the old bytes in owned recovery storage. */
        g_assert_true (contains_old_destination (fixture->output));
    } else {
        if (mode == 3)
            assert_no_retirement_failure (fixture);
        else
            assert_unpublished_failure (fixture);
        if (mode == 0)
            g_assert_error (fixture->error, G_IO_ERROR, G_IO_ERROR_EXISTS);
        else
            g_assert_cmpuint (operation.injected, ==, 1);
        g_assert_cmpint (lstat (fixture->destination, &after), ==, 0);
        g_assert_cmpuint (before.st_ino, ==, after.st_ino);
        g_assert_true (contains_old_destination (fixture->destination));
    }
    fixture_free (fixture);
}

static void
test_unsafe_target (gconstpointer data)
{
    int kind = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new ();
    make_small_dataset (fixture);
    g_autofree char *victim = g_build_filename (fixture->input, "victim.txt", NULL);
    if (kind == 0) {
        g_free (fixture->destination);
        fixture->destination = g_build_filename (fixture->input, "folder", "inside.7z", NULL);
    } else if (kind == 1) {
        g_autofree char *self = g_build_filename (fixture->input, "self.7z", NULL);
        write_new (self, "selected archive", 16);
        fixture->sources = g_list_append (fixture->sources, g_file_new_for_path (self));
        g_free (fixture->destination);
        fixture->destination = g_strdup (self);
    } else if (kind == 2) {
        g_assert_cmpint (link (victim, fixture->destination), ==, 0);
    } else if (kind == 3) {
        g_assert_cmpint (mkdir (fixture->destination, 0700), ==, 0);
    } else {
        g_assert_cmpint (symlink (victim, fixture->destination), ==, 0);
    }
    run_job (fixture, TRUE, TRUE, FAULT_NONE);
    assert_unpublished_failure (fixture);
    assert_all_originals (fixture);
    fixture_free (fixture);
}

static void
test_bad_source (gconstpointer data)
{
    gboolean unreadable = GPOINTER_TO_INT (data);
    if (unreadable && geteuid () == 0) {
        g_test_skip ("Root bypasses ordinary unreadable-file permissions");
        return;
    }
    Fixture *fixture = fixture_new ();
    make_small_dataset (fixture);
    g_autofree char *path = g_build_filename (fixture->input, "folder", "blocked", NULL);
    if (unreadable) {
        write_new (path, "must not be silently omitted", 27);
        g_assert_cmpint (chmod (path, 0000), ==, 0);
    } else {
        g_assert_cmpint (mkfifo (path, 0600), ==, 0);
    }
    snapshot_metadata (fixture);
    run_job (fixture, TRUE, FALSE, FAULT_NONE);
    assert_unpublished_failure (fixture);
    assert_all_originals (fixture);
    g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test (fixture->destination, G_FILE_TEST_EXISTS));
    fixture_free (fixture);
}

static void
test_mutation (gconstpointer data)
{
    Fault fault = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new ();
    make_small_dataset (fixture);
    run_job (fixture, TRUE, FALSE, fault);
    g_assert_cmpuint (operation.injected, ==, 1);
    g_assert_false (fixture->success);
    g_assert_nonnull (fixture->error);
    g_assert_false (fixture->result.sources_removed);
    if (fault <= CHILD_BEFORE_PUBLISH) {
        assert_unpublished_failure (fixture);
        g_assert_false (g_file_test (fixture->destination, G_FILE_TEST_EXISTS));
    } else {
        g_assert_true (fixture->result.published);
        g_assert_true (fixture->result.archive_ok);
        inspect_archive (fixture);
        NemoProgressResult progress;
        g_assert_true (nemo_progress_info_get_result (fixture->progress, &progress));
        g_assert_cmpint (progress.outcome, ==, NEMO_PROGRESS_OUTCOME_PARTIAL);
        g_autofree char *details = nemo_progress_info_get_completion_text (fixture->progress);
        g_assert_nonnull (strstr (details, "manifest.sqlite"));
    }
    g_autofree char *path = NULL;
    const char *expected;
    if (fault == CHILD_BEFORE_PUBLISH || fault == CHILD_AFTER_PUBLISH) {
        path = g_build_filename (fixture->input, "folder", "new-child.txt", NULL);
        expected = "not in archive";
    } else if (fault == REPLACE_BEFORE_PUBLISH || fault == REPLACE_AFTER_PUBLISH) {
        path = g_build_filename (fixture->input, "victim.txt", NULL);
        expected = "replacement survives";
        Expected *original = g_hash_table_lookup (fixture->members, "victim.txt");
        g_autofree char *saved = g_build_filename (fixture->input, "saved-original.txt", NULL);
        struct stat identity;
        g_assert_cmpint (lstat (saved, &identity), ==, 0);
        g_assert_cmpuint (identity.st_ino, ==, original->original.st_ino);
    } else {
        path = g_build_filename (fixture->input, "victim.txt", NULL);
        expected = "changedl victim bytes";
    }
    g_autofree char *bytes = NULL;
    g_assert_true (g_file_get_contents (path, &bytes, NULL, NULL));
    g_assert_cmpstr (bytes, ==, expected);
    fixture_free (fixture);
}

static void
test_partial_retirement (void)
{
    Fixture *fixture = fixture_new ();
    make_small_dataset (fixture);
    run_job (fixture, TRUE, FALSE, FAIL_RETIRE_SECOND);
    g_assert_cmpuint (operation.injected, ==, 1);
    g_assert_false (fixture->success);
    g_assert_nonnull (fixture->error);
    g_assert_true (fixture->result.published);
    g_assert_true (fixture->result.archive_ok);
    g_assert_false (fixture->result.sources_removed);
    g_assert_cmpuint (fixture->result.removed_items, ==, 1);
    inspect_archive (fixture);
    guint missing = 0;
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init (&iter, fixture->members);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        Expected *expected = value;
        g_autofree char *path = g_build_filename (fixture->input, expected->name, NULL);
        if (!g_file_test (path, G_FILE_TEST_EXISTS))
            missing++;
        else if (!S_ISDIR (expected->original.st_mode))
            assert_original (fixture, expected);
    }
    g_assert_cmpuint (missing, ==, fixture->result.removed_items);
    g_autofree char *details = nemo_progress_info_get_completion_text (fixture->progress);
    g_assert_nonnull (strstr (details, "manifest.sqlite"));
    fixture_free (fixture);
}

static void
test_capture_restore (void)
{
    Fixture *fixture = fixture_new ();
    make_small_dataset (fixture);
    run_job (fixture, TRUE, FALSE, FAIL_CAPTURE_UNLINK);
    g_assert_cmpuint (operation.injected, ==, 1);
    g_assert_false (fixture->success);
    g_assert_nonnull (fixture->error);
    g_assert_true (fixture->result.published);
    g_assert_true (fixture->result.archive_ok);
    g_assert_false (fixture->result.sources_removed);
    g_assert_cmpuint (fixture->result.removed_items, ==, 0);
    assert_all_originals (fixture);
    inspect_archive (fixture);
    g_autofree char *details = nemo_progress_info_get_completion_text (fixture->progress);
    g_assert_nonnull (strstr (details, "restored"));
    g_assert_nonnull (strstr (details, "manifest.sqlite"));
    fixture_free (fixture);
}

static void
test_worker_exits_immediately (void)
{
    Fixture *fixture = fixture_new ();
    /* Exceed socket capacity even on hosts with unusually large defaults.
     * No timeout callback cancels this job: it must notice the dead reader. */
    g_autofree char *bytes = g_malloc (2 * 1024 * 1024 + 1);
    memset (bytes, 'x', 2 * 1024 * 1024);
    bytes[2 * 1024 * 1024] = '\0';
    add_member (fixture, "larger-than-worker-socket.bin", bytes, NULL);
    snapshot_metadata (fixture);
    run_job (fixture, TRUE, FALSE, EXIT_WORKER);
    g_assert_cmpuint (operation.injected, ==, 1);
    assert_unpublished_failure (fixture);
    g_assert_false (g_error_matches (fixture->error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
    g_autoptr (GCancellable) cancel = nemo_progress_info_get_cancellable (fixture->progress);
    g_assert_false (g_cancellable_is_cancelled (cancel));
    g_assert_false (g_file_test (fixture->destination, G_FILE_TEST_EXISTS));
    assert_all_originals (fixture);
    fixture_free (fixture);
}

static void
write_virtual_backing (Fixture *fixture, const char *path, gboolean iso)
{
    struct archive *writer = archive_write_new ();
    g_assert_cmpint (archive_write_set_format_by_name (writer, iso ? "iso9660" : "zip"),
                     ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_open_filename (writer, path), ==, ARCHIVE_OK);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init (&iter, fixture->members);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        Expected *expected = value;
        struct archive_entry *entry = archive_entry_new ();
        archive_entry_set_pathname_utf8 (entry, expected->name);
        archive_entry_set_filetype (entry, expected->original.st_mode & S_IFMT);
        archive_entry_set_perm (entry, expected->original.st_mode & 07777);
        archive_entry_set_mtime (entry, expected->original.st_mtim.tv_sec,
                                expected->original.st_mtim.tv_nsec);
        gsize size = 0;
        gconstpointer bytes = expected->bytes ? g_bytes_get_data (expected->bytes, &size) : NULL;
        archive_entry_set_size (entry, size);
        if (expected->link)
            archive_entry_set_symlink_utf8 (entry, expected->link);
        g_assert_cmpint (archive_write_header (writer, entry), ==, ARCHIVE_OK);
        if (size)
            g_assert_cmpint (archive_write_data (writer, bytes, size), ==, size);
        g_assert_cmpint (archive_write_finish_entry (writer), ==, ARCHIVE_OK);
        archive_entry_free (entry);
    }
    g_assert_cmpint (archive_write_close (writer), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_free (writer), ==, ARCHIVE_OK);
}

static char *
file_digest (const char *path)
{
    g_autofree char *bytes = NULL;
    gsize size;
    g_assert_true (g_file_get_contents (path, &bytes, &size, NULL));
    return g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *) bytes, size);
}

typedef struct { gboolean iso; gboolean move; } VirtualCase;

static void
test_virtual_sources (gconstpointer data)
{
    const VirtualCase *test = data;
    Fixture *fixture = fixture_new ();
    make_full_dataset (fixture);
    g_autofree char *path = g_build_filename (fixture->root, test->iso ? "input.iso" : "input.zip", NULL);
    write_virtual_backing (fixture, path, test->iso);
    struct stat before, after;
    g_assert_cmpint (lstat (path, &before), ==, 0);
    g_autofree char *original_digest = file_digest (path);
    g_autoptr (GFile) backing = g_file_new_for_path (path);
    GFile *root = nemo_archive_file_new_for_archive (backing);
    g_assert_false (g_file_is_native (root));
    GList *virtual_sources = NULL;
    for (GList *l = fixture->sources; l; l = l->next) {
        g_autofree char *name = g_file_get_basename (l->data);
        GFile *member = g_file_get_child (root, name);
        g_assert_true (g_file_has_uri_scheme (member, "nemo-archive"));
        virtual_sources = g_list_append (virtual_sources, member);
    }
    g_list_free_full (fixture->sources, g_object_unref);
    fixture->sources = virtual_sources;

    /* ZIP/ISO have their own timestamp precision. Preserve what the real
     * provider exposes, not unavailable nanoseconds from the fixture inputs. */
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init (&iter, fixture->members);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        Expected *expected = value;
        g_autoptr (GFile) member = g_file_resolve_relative_path (root, expected->name);
        g_autoptr (GError) error = NULL;
        g_autoptr (GFileInfo) info = g_file_query_info (
            member, G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                    G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
        g_assert_no_error (error);
        g_assert_nonnull (info);
        expected->original.st_mtim.tv_sec =
            g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        expected->original.st_mtim.tv_nsec =
            1000L * g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
    }
    g_object_unref (root);
    run_job (fixture, test->move, FALSE, FAULT_NONE);
    if (test->move) {
        assert_unpublished_failure (fixture);
        g_assert_false (g_file_test (fixture->destination, G_FILE_TEST_EXISTS));
    } else {
        assert_success (fixture, FALSE);
        g_assert_cmpuint (operation.retirements, ==, 0);
    }
    g_autofree char *after_digest = file_digest (path);
    g_assert_cmpstr (after_digest, ==, original_digest);
    g_assert_cmpint (lstat (path, &after), ==, 0);
    g_assert_cmpuint (after.st_dev, ==, before.st_dev);
    g_assert_cmpuint (after.st_ino, ==, before.st_ino);
    g_assert_cmpint (after.st_mtim.tv_sec, ==, before.st_mtim.tv_sec);
    g_assert_cmpint (after.st_mtim.tv_nsec, ==, before.st_mtim.tv_nsec);
    fixture_free (fixture);
}

int
main (int argc, char **argv)
{
    if (argc == 2 && g_str_equal (argv[1], "--exit-worker"))
        return 73;
    test_program = g_file_read_link ("/proc/self/exe", NULL);
    g_assert_nonnull (test_program);
    setlocale (LC_ALL, "C.UTF-8");
    g_setenv ("GIO_USE_VFS", "local", TRUE);
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
    g_test_init (&argc, &argv, NULL);
    struct statfs filesystem;
    g_assert_cmpint (statfs (".", &filesystem), ==, 0);
    if (filesystem.f_type == TMPFS_MAGIC) {
        g_printerr ("Archive durability regressions require a disk-backed build workspace.\n");
        return 77;
    }
    g_autofree char *cwd = g_get_current_dir ();
    g_autofree char *cache = g_build_filename (cwd, ".nemo-archive-create-cache-XXXXXX", NULL);
    g_assert_nonnull (g_mkdtemp_full (cache, 0700));
    struct stat cache_identity;
    g_assert_cmpint (lstat (cache, &cache_identity), ==, 0);
    g_assert_true (g_setenv ("XDG_CACHE_HOME", cache, TRUE));
    g_assert_cmpstr (g_get_user_cache_dir (), ==, cache);
    nemo_archive_file_register ();
    g_test_add_data_func ("/archive-create/copy/roundtrip-metadata", GINT_TO_POINTER (FALSE), test_roundtrip);
    g_test_add_func ("/archive-create/copy/uppercase-extension", test_uppercase_extension);
    g_test_add_data_func ("/archive-create/move/roundtrip-metadata", GINT_TO_POINTER (TRUE), test_roundtrip);
    static const struct { const char *name; Fault fault; } faults[] = {
        { "cancel-before", CANCEL_BEFORE }, { "cancel-compress", CANCEL_COMPRESS },
        { "cancel-verify", CANCEL_VERIFY }, { "producer-write", FAIL_WRITE },
        { "verification-read", FAIL_VERIFY }, { "verification-corrupt", CORRUPT_VERIFY },
        { "staging-fsync", FAIL_SYNC }, { "producer-disk-full", FAIL_NOSPACE },
        { "publication-rename", FAIL_PUBLISH }
    };
    for (guint i = 0; i < G_N_ELEMENTS (faults); i++) {
        g_autofree char *name = g_strconcat ("/archive-create/fault/", faults[i].name, NULL);
        g_test_add_data_func (name, GINT_TO_POINTER (faults[i].fault), test_fault);
    }
    g_test_add_data_func ("/archive-create/existing/no-overwrite", GINT_TO_POINTER (0), test_existing);
    g_test_add_data_func ("/archive-create/existing/overwrite", GINT_TO_POINTER (1), test_existing);
    g_test_add_data_func ("/archive-create/existing/failed-overwrite", GINT_TO_POINTER (2), test_existing);
    g_test_add_data_func ("/archive-create/existing/publication-failure", GINT_TO_POINTER (3), test_existing);
    const char *targets[] = { "inside-input", "self", "hardlink-alias", "directory", "symlink" };
    for (guint i = 0; i < G_N_ELEMENTS (targets); i++) {
        g_autofree char *name = g_strconcat ("/archive-create/target/", targets[i], NULL);
        g_test_add_data_func (name, GINT_TO_POINTER (i), test_unsafe_target);
    }
    g_test_add_data_func ("/archive-create/source/unreadable", GINT_TO_POINTER (TRUE), test_bad_source);
    g_test_add_data_func ("/archive-create/source/fifo", GINT_TO_POINTER (FALSE), test_bad_source);
    static const struct { const char *name; Fault fault; } mutations[] = {
        { "changed-before", CHANGE_BEFORE_PUBLISH }, { "replaced-before", REPLACE_BEFORE_PUBLISH },
        { "new-child-before", CHILD_BEFORE_PUBLISH }, { "changed-after", CHANGE_AFTER_PUBLISH },
        { "replaced-after", REPLACE_AFTER_PUBLISH }, { "new-child-after", CHILD_AFTER_PUBLISH }
    };
    for (guint i = 0; i < G_N_ELEMENTS (mutations); i++) {
        g_autofree char *name = g_strconcat ("/archive-create/mutation/", mutations[i].name, NULL);
        g_test_add_data_func (name, GINT_TO_POINTER (mutations[i].fault), test_mutation);
    }
    g_test_add_func ("/archive-create/move/partial-retirement", test_partial_retirement);
    g_test_add_func ("/archive-create/move/capture-restore", test_capture_restore);
    g_test_add_func ("/archive-create/fault/worker-exits-immediately", test_worker_exits_immediately);
    static const VirtualCase virtual_cases[] = {
        { FALSE, FALSE }, { TRUE, FALSE }, { FALSE, TRUE }, { TRUE, TRUE }
    };
    g_test_add_data_func ("/archive-create/virtual/copy-zip", &virtual_cases[0], test_virtual_sources);
    g_test_add_data_func ("/archive-create/virtual/copy-iso", &virtual_cases[1], test_virtual_sources);
    g_test_add_data_func ("/archive-create/virtual/refuse-move-zip", &virtual_cases[2], test_virtual_sources);
    g_test_add_data_func ("/archive-create/virtual/refuse-move-iso", &virtual_cases[3], test_virtual_sources);
    int status = g_test_run ();
    struct stat identity;
    g_assert_cmpint (lstat (cache, &identity), ==, 0);
    g_assert_cmpuint (identity.st_dev, ==, cache_identity.st_dev);
    g_assert_cmpuint (identity.st_ino, ==, cache_identity.st_ino);
    remove_owned_entry (cache, identity.st_dev);
    g_free (test_program);
    return status;
}
#endif
