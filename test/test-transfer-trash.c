/* Guard contracts only: no GVfs queries, g_file_trash(), or D-Bus. */
#include <config.h>
#include <glib/gstdio.h>
#include <libnemo-private/nemo-mount-operation.h>
#include <libnemo-private/nemo-transfer-safety.h>
#include <string.h>
#include <sys/stat.h>

/* Link the safety module directly; no mount-operation/UI initialization. */
gboolean
nemo_mount_operation_is_removing (void)
{
    return FALSE;
}

static void
assert_unchanged (const char *path, const struct stat *before, const char *expected)
{
    struct stat after;
    g_assert_cmpint (g_lstat (path, &after), ==, 0);
    g_assert_cmpuint (before->st_dev, ==, after.st_dev);
    g_assert_cmpuint (before->st_ino, ==, after.st_ino);
    g_assert_cmpuint (before->st_mode, ==, after.st_mode);
    g_assert_cmpint (before->st_size, ==, after.st_size);
    g_assert_cmpint (before->st_mtim.tv_sec, ==, after.st_mtim.tv_sec);
    g_assert_cmpint (before->st_mtim.tv_nsec, ==, after.st_mtim.tv_nsec);
    g_assert_cmpint (before->st_ctim.tv_sec, ==, after.st_ctim.tv_sec);
    g_assert_cmpint (before->st_ctim.tv_nsec, ==, after.st_ctim.tv_nsec);
    g_autofree char *contents = NULL;
    g_autoptr (GError) error = NULL;
    g_assert_true (g_file_get_contents (path, &contents, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (contents, ==, expected);
}

static void
guard_retains_fixture (gconstpointer user_data)
{
    gboolean move = GPOINTER_TO_INT (user_data);
    g_autofree char *cwd = g_get_current_dir ();
    g_autofree char *fixture = g_build_filename (cwd, ".nemo-trash-guard-XXXXXX", NULL);
    g_assert_nonnull (g_mkdtemp_full (fixture, 0700));
    g_autofree char *data_path = g_build_filename (fixture, "data", NULL);
    g_autofree char *trash_path = g_build_filename (data_path, "Trash", NULL);
    g_autofree char *files_path = g_build_filename (trash_path, "files", NULL);
    g_autofree char *info_path = g_build_filename (trash_path, "info", NULL);
    g_autofree char *source_path = g_build_filename (files_path, "item", NULL);
    g_autofree char *metadata_path = g_build_filename (info_path, "item.trashinfo", NULL);
    g_autofree char *destination_path = g_build_filename (fixture, "destination", NULL);
    g_assert_cmpint (g_mkdir_with_parents (files_path, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (info_path, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (destination_path, 0700), ==, 0);

    const char *payload = "Disposable original Trash contents.\n";
    const char *metadata = "[Trash Info]\nPath=/not/the/backing/source\n"
                           "DeletionDate=2026-09-20T12:00:00\n";
    g_autoptr (GError) error = NULL;
    g_assert_true (g_file_set_contents (source_path, payload, -1, &error));
    g_assert_no_error (error);
    g_assert_true (g_file_set_contents (metadata_path, metadata, -1, &error));
    g_assert_no_error (error);
    struct stat source_before, metadata_before;
    g_assert_cmpint (g_lstat (source_path, &source_before), ==, 0);
    g_assert_cmpint (g_lstat (metadata_path, &metadata_before), ==, 0);

    g_autoptr (GFile) virtual = g_file_new_for_uri ("trash:///item");
    g_autoptr (GFile) source = g_file_new_for_path (source_path);
    g_autoptr (GFile) destination = g_file_new_for_path (destination_path);
    g_autofree char *target_uri = g_file_get_uri (source);
    g_autoptr (GFileInfo) info = g_file_info_new ();
    g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, target_uri);
    g_file_info_set_attribute_byte_string (info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH,
                                         "/not/the/backing/source");
    g_file_info_set_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_DEVICE, source_before.st_dev);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_UNIX_INODE, source_before.st_ino);
    /* Fixture metadata is deliberately not queried through a real backend.
     * The guard must keep the virtual source, not authorize a native move. */
    g_object_set_data_full (G_OBJECT (virtual), "mock-trash-info",
                           g_object_ref (info), g_object_unref);

    GList sources = { .data = virtual };
    NemoTransferGuard *guard = nemo_transfer_guard_new (&sources, destination, move);
    if (move) {
        for (guint attempt = 0; attempt < 2; attempt++) {
            g_assert_false (nemo_transfer_guard_check (guard, &error));
            g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
            g_assert_cmpstr (error->message, ==,
                            "Safe Trash restore is unavailable. The original Trash item "
                            "was retained. Use Copy to a supported local destination instead, "
                            "and review the copied files before emptying Trash.");
            g_clear_error (&error);
        }
    } else {
        g_assert_true (nemo_transfer_guard_check (guard, &error));
        g_assert_no_error (error);
        g_autoptr (GFile) lease = nemo_transfer_guard_file (guard, virtual, FALSE, &error);
        g_assert_no_error (error);
        g_assert_true (lease == virtual);
        g_assert_false (g_file_is_native (lease));
        g_autofree char *path = g_file_get_path (lease);
        g_assert_null (path);
        g_assert_true (g_object_get_data (G_OBJECT (lease), "mock-trash-info") == info);
    }
    g_assert_true (nemo_transfer_guard_release (guard, &error));
    g_assert_no_error (error);
    nemo_transfer_guard_unref (guard);

    assert_unchanged (source_path, &source_before, payload);
    assert_unchanged (metadata_path, &metadata_before, metadata);

    /* Remove only exact entries created by this test, never enumerate Trash. */
    g_assert_cmpint (g_unlink (source_path), ==, 0);
    g_assert_cmpint (g_unlink (metadata_path), ==, 0);
    g_assert_cmpint (g_rmdir (files_path), ==, 0);
    g_assert_cmpint (g_rmdir (info_path), ==, 0);
    g_assert_cmpint (g_rmdir (trash_path), ==, 0);
    g_assert_cmpint (g_rmdir (data_path), ==, 0);
    g_assert_cmpint (g_rmdir (destination_path), ==, 0);
    g_assert_cmpint (g_rmdir (fixture), ==, 0);
}

static void
guard_other_remote_refusal (void)
{
    g_autoptr (GFile) source = g_file_new_for_uri ("sftp://example.invalid/item");
    GList sources = { .data = source };
    NemoTransferGuard *guard = nemo_transfer_guard_new (&sources, NULL, TRUE);
    g_autoptr (GError) error = NULL;
    g_assert_false (nemo_transfer_guard_check (guard, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_assert_null (strstr (error->message, "Trash"));
    g_assert_nonnull (strstr (error->message, "required atomic publication"));
    nemo_transfer_guard_unref (guard);
}

int
main (int argc, char **argv)
{
    /* Set before initializing GIO: even the URI objects must use no GVfs. */
    g_setenv ("GIO_USE_VFS", "local", TRUE);
    g_test_init (&argc, &argv, NULL);
    g_test_add_data_func ("/transfer-trash/guard-refusal-retains-fixture",
                         GINT_TO_POINTER (TRUE), guard_retains_fixture);
    g_test_add_data_func ("/transfer-trash/guard-copy-retains-fixture",
                         GINT_TO_POINTER (FALSE), guard_retains_fixture);
    g_test_add_func ("/transfer-trash/guard-other-remote-refusal", guard_other_remote_refusal);
    return g_test_run ();
}
