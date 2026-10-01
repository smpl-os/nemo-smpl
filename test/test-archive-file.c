/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../libnemo-private/nemo-archive-file.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <string.h>
#include <utime.h>

static char *fixture_dir;
static char *test_program;
static const char payload[] = "archive member bytes\nwith another line\n";
static gint header_reads, info_allocations, cancel_at_header;
static gpointer header_cancel;

int __real_archive_read_next_header (struct archive *, struct archive_entry **);
GFileInfo *__real_g_file_info_new (void);

int
__wrap_archive_read_next_header (struct archive *reader, struct archive_entry **entry)
{
    int result = __real_archive_read_next_header (reader, entry);
    gint count = g_atomic_int_add (&header_reads, 1) + 1;
    GCancellable *cancel = g_atomic_pointer_get (&header_cancel);
    if (cancel != NULL && count == g_atomic_int_get (&cancel_at_header))
        g_cancellable_cancel (cancel);
    return result;
}

GFileInfo *
__wrap_g_file_info_new (void)
{
    g_atomic_int_inc (&info_allocations);
    return __real_g_file_info_new ();
}

static void
add_entry (struct archive *writer, const char *name, const char *bytes, int type)
{
    struct archive_entry *entry = archive_entry_new ();
    archive_entry_set_pathname (entry, name);
    archive_entry_set_filetype (entry, type);
    archive_entry_set_perm (entry, 0644);
    archive_entry_set_mtime (entry, 1700000000, 123456000);
    archive_entry_set_size (entry, bytes ? strlen (bytes) : 0);
    if (type == AE_IFLNK)
        archive_entry_set_symlink (entry, "../../outside");
    g_assert_cmpint (archive_write_header (writer, entry), ==, ARCHIVE_OK);
    if (bytes)
        g_assert_cmpint (archive_write_data (writer, bytes, strlen (bytes)), ==, strlen (bytes));
    archive_entry_free (entry);
}

static char *
make_archive (const char *name, const char *format, gboolean unsafe, gboolean duplicate)
{
    char *path = g_build_filename (fixture_dir, name, NULL);
    struct archive *writer = archive_write_new ();
    g_assert_cmpint (archive_write_set_format_by_name (writer, format), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_open_filename (writer, path), ==, ARCHIVE_OK);
    add_entry (writer, "top/sub/hello %.txt", payload, AE_IFREG);
    add_entry (writer, "second.txt", "second\n", AE_IFREG);
    if (strcmp (format, "pax") == 0)
        add_entry (writer, "link", NULL, AE_IFLNK);
    if (unsafe)
        add_entry (writer, "../outside", "unsafe", AE_IFREG);
    if (duplicate)
        add_entry (writer, "second.txt", "different", AE_IFREG);
    g_assert_cmpint (archive_write_close (writer), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_free (writer), ==, ARCHIVE_OK);
    return path;
}

static void
check_member_seek (GFile *member, const guint8 *expected, gsize length)
{
    GError *error = NULL;
    GFileInputStream *stream = g_file_read (member, NULL, &error);
    GCancellable *cancel = g_cancellable_new ();
    guint8 bytes[32];
    gsize read;
    goffset middle = length / 2;
    g_assert_no_error (error);
    g_assert_nonnull (stream);
    g_assert_true (g_seekable_can_seek (G_SEEKABLE (stream)));
    g_assert_true (g_input_stream_read_all (G_INPUT_STREAM (stream), bytes, 4, &read, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpmem (bytes, read, expected, 4);
    g_assert_cmpint (g_seekable_tell (G_SEEKABLE (stream)), ==, 4);
    g_assert_true (g_seekable_seek (G_SEEKABLE (stream), middle, G_SEEK_SET, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_input_stream_read_all (G_INPUT_STREAM (stream), bytes, 12, &read, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpmem (bytes, read, expected + middle, 12);
    g_assert_true (g_seekable_seek (G_SEEKABLE (stream), -8, G_SEEK_CUR, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_seekable_tell (G_SEEKABLE (stream)), ==, middle + 4);
    g_assert_true (g_input_stream_read_all (G_INPUT_STREAM (stream), bytes, 8, &read, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpmem (bytes, read, expected + middle + 4, 8);
    g_assert_true (g_seekable_seek (G_SEEKABLE (stream), -9, G_SEEK_END, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_input_stream_read_all (G_INPUT_STREAM (stream), bytes, sizeof bytes, &read, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpmem (bytes, read, expected + length - 9, 9);
    g_assert_cmpint (g_seekable_tell (G_SEEKABLE (stream)), ==, length);
    g_assert_true (g_seekable_seek (G_SEEKABLE (stream), 0, G_SEEK_SET, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_input_stream_read_all (G_INPUT_STREAM (stream), bytes, 8, &read, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpmem (bytes, read, expected, 8);
    g_assert_true (g_seekable_seek (G_SEEKABLE (stream), 0, G_SEEK_END, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_input_stream_read (G_INPUT_STREAM (stream), bytes, sizeof bytes, NULL, &error), ==, 0);
    g_assert_no_error (error);
    g_assert_false (g_seekable_seek (G_SEEKABLE (stream), -1, G_SEEK_SET, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error (&error);
    g_assert_false (g_seekable_seek (G_SEEKABLE (stream), 1, G_SEEK_END, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error (&error);
    g_assert_cmpint (g_seekable_tell (G_SEEKABLE (stream)), ==, length);
    g_cancellable_cancel (cancel);
    g_assert_false (g_seekable_seek (G_SEEKABLE (stream), 0, G_SEEK_SET, cancel, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&error);
    g_assert_cmpint (g_seekable_tell (G_SEEKABLE (stream)), ==, length);
    g_assert_true (g_seekable_seek (G_SEEKABLE (stream), 0, G_SEEK_SET, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_input_stream_close (G_INPUT_STREAM (stream), NULL, &error));
    g_assert_no_error (error);
    g_object_unref (stream);
    g_object_unref (cancel);
}

static void
check_archive (const char *path)
{
    GError *error = NULL;
    GFile *backing = g_file_new_for_path (path);
    GFile *root = nemo_archive_file_new_for_archive (backing);
    GFile *member = g_file_resolve_relative_path (root, "top/sub/hello %.txt");
    GFile *decoded, *parent, *archive;
    GFileEnumerator *enumerator;
    GFileInfo *info, *stream_info;
    GFileInputStream *stream;
    char *uri, *relative, *copy_path;
    guint count = 0;
    char bytes[128] = { 0 };
    gsize size;
    GFile *destination;
    char *copied;

    g_assert_false (g_file_is_native (root));
    g_assert_null (g_file_get_path (root));
    g_assert_true (g_file_has_uri_scheme (root, "nemo-archive"));
    uri = g_file_get_uri (member);
    decoded = g_file_new_for_uri (uri);
    g_assert_true (g_file_equal (member, decoded));
    g_assert_cmpuint (g_file_hash (member), ==, g_file_hash (decoded));
    archive = nemo_archive_file_get_archive (decoded);
    g_assert_true (g_file_equal (backing, archive));
    g_object_unref (archive);
    g_object_unref (decoded);
    decoded = g_file_parse_name (uri);
    g_assert_true (g_file_equal (member, decoded));
    g_object_unref (decoded);
    g_free (uri);
    uri = g_file_get_path (backing);
    decoded = g_file_parse_name (uri);
    g_assert_true (g_file_equal (backing, decoded));
    g_object_unref (decoded);
    g_free (uri);
    g_assert_true (g_file_has_prefix (member, root));
    relative = g_file_get_relative_path (root, member);
    g_assert_cmpstr (relative, ==, "top/sub/hello %.txt");
    g_free (relative);
    parent = g_file_get_parent (member);
    info = g_file_query_info (parent, "*", 0, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpint (g_file_info_get_file_type (info), ==, G_FILE_TYPE_DIRECTORY);
    g_object_unref (info);
    enumerator = g_file_enumerate_children (parent, "*", 0, NULL, &error);
    g_assert_no_error (error);
    while ((info = g_file_enumerator_next_file (enumerator, NULL, &error))) {
        count++;
        g_assert_cmpstr (g_file_info_get_name (info), ==, "hello %.txt");
        g_assert_cmpint (g_file_info_get_size (info), ==, strlen (payload));
        g_assert_false (g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE));
        g_object_unref (info);
    }
    g_assert_no_error (error);
    g_assert_cmpuint (count, ==, 1);
    g_assert_true (g_file_enumerator_close (enumerator, NULL, &error));
    g_assert_no_error (error);
    g_object_unref (enumerator);
    g_object_unref (parent);
    info = g_file_query_info (member, "*", 0, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (g_file_info_get_etag (info));
    stream = g_file_read (member, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (stream);
    g_assert_true (g_seekable_can_seek (G_SEEKABLE (stream)));
    stream_info = g_file_input_stream_query_info (stream, "*", NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (g_file_info_get_etag (info), ==, g_file_info_get_etag (stream_info));
    g_object_unref (info);
    g_object_unref (stream_info);
    g_assert_true (g_input_stream_read_all (G_INPUT_STREAM (stream), bytes, sizeof bytes, &size, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (size, ==, strlen (payload));
    g_assert_cmpstr (bytes, ==, payload);
    g_assert_true (g_input_stream_close (G_INPUT_STREAM (stream), NULL, &error));
    g_assert_no_error (error);
    g_object_unref (stream);

    copy_path = g_build_filename (fixture_dir, "copy.txt", NULL);
    destination = g_file_new_for_path (copy_path);
    g_assert_true (g_file_copy (member, destination, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_file_get_contents (copy_path, &copied, &size, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (copied, ==, payload);
    g_free (copied);
    g_assert_cmpint (g_unlink (copy_path), ==, 0);
    g_assert_false (g_file_move (member, destination, 0, NULL, NULL, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error (&error);
    g_assert_false (g_file_test (copy_path, G_FILE_TEST_EXISTS));
    g_free (copy_path);
    g_object_unref (destination);

    g_assert_false (g_file_delete (member, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error (&error);
    g_assert_null (g_file_replace (member, NULL, FALSE, 0, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error (&error);
    decoded = g_file_resolve_relative_path (root, "../outside");
    g_assert_null (g_file_read (decoded, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error (&error);
    g_object_unref (decoded);
    check_member_seek (member, (const guint8 *) payload, strlen (payload));
    g_object_unref (member);
    g_object_unref (root);
    g_object_unref (backing);
}

static void
test_formats (void)
{
    const char *formats[] = { "7zip", "zip", "pax", "iso9660" };
    guint i;
    for (i = 0; i < G_N_ELEMENTS (formats); i++) {
        char *path = make_archive (formats[i], formats[i], FALSE, FALSE);
        g_test_message ("Reading and copying %s", formats[i]);
        check_archive (path);
        g_assert_cmpint (g_unlink (path), ==, 0);
        g_free (path);
    }
}

static void
test_bsdtar (void)
{
    GError *error = NULL;
    char *tool = g_find_program_in_path ("bsdtar");
    char *source, *archive, *top, *sub, *large_path, *output = NULL, *errors = NULL;
    guint8 *large = g_malloc (1024 * 1024);
    gint status;
    const char *argv[9];
    if (!tool) {
        g_test_skip ("bsdtar unavailable");
        g_free (large);
        return;
    }
    top = g_build_filename (fixture_dir, "top", NULL);
    sub = g_build_filename (top, "sub", NULL);
    g_assert_cmpint (g_mkdir (top, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (sub, 0700), ==, 0);
    source = g_build_filename (sub, "hello %.txt", NULL);
    large_path = g_build_filename (top, "large.bin", NULL);
    for (guint i = 0; i < 1024 * 1024; i++)
        large[i] = (guint8) g_test_rand_int ();
    g_assert_true (g_file_set_contents (large_path, (char *) large, 1024 * 1024, &error));
    g_assert_no_error (error);
    archive = g_build_filename (fixture_dir, "bsdtar.7z", NULL);
    g_assert_true (g_file_set_contents (source, payload, -1, &error));
    g_assert_no_error (error);
    argv[0] = tool; argv[1] = "--format=7zip"; argv[2] = "-cf"; argv[3] = archive;
    argv[4] = "-C"; argv[5] = fixture_dir; argv[6] = "top"; argv[7] = NULL;
    g_assert_true (g_spawn_sync (NULL, (char **) argv, NULL, 0, NULL, NULL, &output, &errors, &status, &error));
    g_assert_no_error (error);
    g_assert_true (g_spawn_check_wait_status (status, &error));
    g_assert_no_error (error);
    check_archive (archive);
    {
        GFile *backing = g_file_new_for_path (archive);
        GFile *root = nemo_archive_file_new_for_archive (backing);
        GFile *member = g_file_resolve_relative_path (root, "top/large.bin");
        char *actual;
        gsize length;
        g_assert_true (g_file_load_contents (member, NULL, &actual, &length, NULL, &error));
        g_assert_no_error (error);
        g_assert_cmpuint (length, ==, 1024 * 1024);
        g_assert_cmpmem (actual, length, large, 1024 * 1024);
        check_member_seek (member, large, 1024 * 1024);
        g_free (actual);
        g_object_unref (member);
        g_object_unref (root);
        g_object_unref (backing);
    }
    g_assert_cmpint (g_unlink (archive), ==, 0);
    g_assert_cmpint (g_unlink (source), ==, 0);
    g_assert_cmpint (g_unlink (large_path), ==, 0);
    g_assert_cmpint (g_rmdir (sub), ==, 0);
    g_assert_cmpint (g_rmdir (top), ==, 0);
    g_free (output); g_free (errors); g_free (source); g_free (archive);
    g_free (sub); g_free (top); g_free (tool);
    g_free (large_path); g_free (large);
}

static void
test_invalid (void)
{
    guint i;
    for (i = 0; i < 3; i++) {
        GError *error = NULL;
        char *path = make_archive ("invalid.tar", "pax", i == 0, i == 1);
        GFile *backing = g_file_new_for_path (path);
        GFile *root = nemo_archive_file_new_for_archive (backing);
        GFile *link;
        if (i < 2) {
            g_assert_null (g_file_enumerate_children (root, "*", 0, NULL, &error));
            g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        } else {
            link = g_file_get_child (root, "link");
            g_assert_null (g_file_read (link, NULL, &error));
            g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
            g_object_unref (link);
        }
        g_clear_error (&error);
        g_object_unref (root);
        g_object_unref (backing);
        g_assert_cmpint (g_unlink (path), ==, 0);
        g_free (path);
    }
}

static void
test_cancel_and_change (void)
{
    GError *error = NULL;
    char *path = make_archive ("change.zip", "zip", FALSE, FALSE);
    GFile *backing = g_file_new_for_path (path);
    GFile *root = nemo_archive_file_new_for_archive (backing);
    GFile *member = g_file_get_child (root, "second.txt");
    GCancellable *cancel = g_cancellable_new ();
    GFileInputStream *stream;
    char bytes[128];

    g_cancellable_cancel (cancel);
    g_assert_null (g_file_read (member, cancel, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&error);
    stream = g_file_read (member, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpint (g_input_stream_read (G_INPUT_STREAM (stream), bytes, sizeof bytes, cancel, &error), ==, -1);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&error);
    g_assert_true (g_file_set_contents (path, "replacement", -1, &error));
    g_assert_no_error (error);
    g_assert_null (g_file_input_stream_query_info (stream, "*", NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG);
    g_clear_error (&error);
    g_assert_false (g_seekable_seek (G_SEEKABLE (stream), 0, G_SEEK_SET, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_WRONG_ETAG);
    g_clear_error (&error);
    g_object_unref (stream);
    g_object_unref (cancel);
    g_object_unref (member);
    g_object_unref (root);
    g_object_unref (backing);
    g_assert_cmpint (g_unlink (path), ==, 0);
    g_free (path);
}

static void
test_nested_seek (void)
{
    GError *error = NULL;
    char *inner = make_archive ("inner.7z", "7zip", FALSE, FALSE);
    char *outer = g_build_filename (fixture_dir, "outer.zip", NULL);
    char *contents, *uri;
    gsize length;
    struct archive *writer = archive_write_new ();
    struct archive_entry *entry = archive_entry_new ();
    GFile *backing, *root, *nested, *nested_root, *member, *roundtrip;
    GFileInputStream *stream;
    char bytes[128] = { 0 };
    gsize read;

    g_assert_true (g_file_get_contents (inner, &contents, &length, &error));
    g_assert_no_error (error);
    g_assert_cmpint (archive_write_set_format_zip (writer), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_open_filename (writer, outer), ==, ARCHIVE_OK);
    archive_entry_set_pathname (entry, "inner.7z");
    archive_entry_set_filetype (entry, AE_IFREG);
    archive_entry_set_perm (entry, 0644);
    archive_entry_set_size (entry, length);
    g_assert_cmpint (archive_write_header (writer, entry), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_data (writer, contents, length), ==, length);
    g_assert_cmpint (archive_write_close (writer), ==, ARCHIVE_OK);
    archive_write_free (writer);
    archive_entry_free (entry);
    g_free (contents);
    backing = g_file_new_for_path (outer);
    root = nemo_archive_file_new_for_archive (backing);
    nested = g_file_get_child (root, "inner.7z");
    nested_root = nemo_archive_file_new_for_archive (nested);
    member = g_file_resolve_relative_path (nested_root, "top/sub/hello %.txt");
    uri = g_file_get_uri (member);
    roundtrip = g_file_new_for_uri (uri);
    g_assert_true (g_file_equal (roundtrip, member));
    stream = g_file_read (roundtrip, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (stream);
    g_assert_cmpint (g_input_stream_skip (G_INPUT_STREAM (stream), 8, NULL, &error), ==, 8);
    g_assert_no_error (error);
    g_assert_true (g_input_stream_read_all (G_INPUT_STREAM (stream), bytes, sizeof bytes, &read, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (bytes, ==, payload + 8);
    g_assert_cmpuint (read, ==, strlen (payload) - 8);
    g_assert_true (g_input_stream_close (G_INPUT_STREAM (stream), NULL, &error));
    g_assert_no_error (error);
    g_object_unref (stream);
    check_member_seek (roundtrip, (const guint8 *) payload, strlen (payload));
    g_object_unref (roundtrip);
    g_free (uri);
    g_object_unref (member);
    g_object_unref (nested_root);
    g_object_unref (nested);
    g_object_unref (root);
    g_object_unref (backing);
    g_assert_cmpint (g_unlink (inner), ==, 0);
    g_assert_cmpint (g_unlink (outer), ==, 0);
    g_free (inner);
    g_free (outer);
}

typedef struct {
    GMainLoop *loop;
    GFile *member;
} AsyncTest;

static void
async_read_done (GObject *object, GAsyncResult *result, gpointer data)
{
    AsyncTest *test = data;
    GError *error = NULL;
    GFileInputStream *stream = g_file_read_finish (G_FILE (object), result, &error);
    char bytes[128] = { 0 };
    gsize read;
    g_assert_no_error (error);
    g_assert_nonnull (stream);
    g_assert_true (g_input_stream_read_all (G_INPUT_STREAM (stream), bytes, sizeof bytes, &read, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (bytes, ==, payload);
    g_assert_true (g_input_stream_close (G_INPUT_STREAM (stream), NULL, &error));
    g_assert_no_error (error);
    g_object_unref (stream);
    g_main_loop_quit (test->loop);
}

static void
async_query_done (GObject *object, GAsyncResult *result, gpointer data)
{
    AsyncTest *test = data;
    GError *error = NULL;
    GFileInfo *info = g_file_query_info_finish (G_FILE (object), result, &error);
    g_assert_no_error (error);
    g_assert_nonnull (info);
    g_assert_cmpint (g_file_info_get_size (info), ==, strlen (payload));
    g_object_unref (info);
    g_file_read_async (test->member, G_PRIORITY_DEFAULT, NULL, async_read_done, test);
}

static void
test_async (void)
{
    char *path = make_archive ("async.7z", "7zip", FALSE, FALSE);
    GFile *backing = g_file_new_for_path (path);
    GFile *root = nemo_archive_file_new_for_archive (backing);
    AsyncTest test = { g_main_loop_new (NULL, FALSE),
                       g_file_resolve_relative_path (root, "top/sub/hello %.txt") };
    g_file_query_info_async (test.member, "*", 0, G_PRIORITY_DEFAULT, NULL, async_query_done, &test);
    g_main_loop_run (test.loop);
    g_main_loop_unref (test.loop);
    g_object_unref (test.member);
    g_object_unref (root);
    g_object_unref (backing);
    g_assert_cmpint (g_unlink (path), ==, 0);
    g_free (path);
}

static void
test_encrypted_and_corrupt (void)
{
    GError *error = NULL;
    char *path = g_build_filename (fixture_dir, "encrypted.zip", NULL);
    struct archive *writer = archive_write_new ();
    GFile *backing, *root;
    g_assert_cmpint (archive_write_set_format_zip (writer), ==, ARCHIVE_OK);
    if (archive_write_set_options (writer, "zip:encryption=aes256") == ARCHIVE_OK) {
        g_assert_cmpint (archive_write_set_passphrase (writer, "test-password"), ==, ARCHIVE_OK);
        g_assert_cmpint (archive_write_open_filename (writer, path), ==, ARCHIVE_OK);
        add_entry (writer, "secret.txt", "secret", AE_IFREG);
        g_assert_cmpint (archive_write_close (writer), ==, ARCHIVE_OK);
        backing = g_file_new_for_path (path);
        root = nemo_archive_file_new_for_archive (backing);
        g_assert_null (g_file_enumerate_children (root, "*", 0, NULL, &error));
        g_assert_nonnull (error);
        g_clear_error (&error);
        g_object_unref (root);
        g_object_unref (backing);
        g_assert_cmpint (g_unlink (path), ==, 0);
    } else {
        g_test_message ("libarchive writer does not support ZIP encryption");
    }
    archive_write_free (writer);
    g_assert_true (g_file_set_contents (path, "7z\274\257\047\034broken", 12, &error));
    g_assert_no_error (error);
    backing = g_file_new_for_path (path);
    root = nemo_archive_file_new_for_archive (backing);
    g_assert_null (g_file_enumerate_children (root, "*", 0, NULL, &error));
    g_assert_nonnull (error);
    g_clear_error (&error);
    g_object_unref (root);
    g_object_unref (backing);
    g_assert_cmpint (g_unlink (path), ==, 0);
    writer = archive_write_new ();
    g_assert_cmpint (archive_write_set_format_zip (writer), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_set_options (writer, "zip:compression=store"), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_open_filename (writer, path), ==, ARCHIVE_OK);
    add_entry (writer, "damaged.txt", payload, AE_IFREG);
    g_assert_cmpint (archive_write_close (writer), ==, ARCHIVE_OK);
    archive_write_free (writer);
    {
        char *contents, *loaded = NULL;
        gsize length, i;
        GFile *member;
        GFileInputStream *stream;
        gboolean found = FALSE;
        g_assert_true (g_file_get_contents (path, &contents, &length, &error));
        g_assert_no_error (error);
        for (i = 0; i + strlen (payload) <= length; i++) {
            if (memcmp (contents + i, payload, strlen (payload)) == 0) {
                contents[i] ^= 1;
                found = TRUE;
                break;
            }
        }
        g_assert_true (found);
        g_assert_true (g_file_set_contents (path, contents, length, &error));
        g_assert_no_error (error);
        g_free (contents);
        backing = g_file_new_for_path (path);
        root = nemo_archive_file_new_for_archive (backing);
        member = g_file_get_child (root, "damaged.txt");
        g_assert_false (g_file_load_contents (member, NULL, &loaded, NULL, NULL, &error));
        g_assert_nonnull (error);
        g_clear_error (&error);
        g_free (loaded);
        stream = g_file_read (member, NULL, &error);
        g_assert_no_error (error);
        g_assert_nonnull (stream);
        g_assert_false (g_seekable_seek (G_SEEKABLE (stream), 0, G_SEEK_END, NULL, &error));
        g_assert_nonnull (error);
        g_assert_cmpint (g_seekable_tell (G_SEEKABLE (stream)), ==, 0);
        g_clear_error (&error);
        g_object_unref (stream);
        g_object_unref (member);
        g_object_unref (root);
        g_object_unref (backing);
    }
    g_assert_cmpint (g_unlink (path), ==, 0);
    g_free (path);
}

static void
test_unicode_c_locale (void)
{
    GError *error = NULL;
    char *path = g_build_filename (fixture_dir, "unicode # % café.zip", NULL);
    char *previous = g_strdup (setlocale (LC_CTYPE, NULL));
    struct archive *writer = archive_write_new ();
    GFile *backing, *root, *member;
    GFileEnumerator *enumerator;
    GFileInfo *info;
    char *bytes;
    gsize length;

    g_assert_nonnull (setlocale (LC_CTYPE, "C.UTF-8"));
    g_assert_cmpint (archive_write_set_format_zip (writer), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_open_filename (writer, path), ==, ARCHIVE_OK);
    add_entry (writer, "uri # % café.txt", payload, AE_IFREG);
    g_assert_cmpint (archive_write_close (writer), ==, ARCHIVE_OK);
    archive_write_free (writer);
    g_assert_nonnull (setlocale (LC_CTYPE, "C"));
    backing = g_file_new_for_path (path);
    root = nemo_archive_file_new_for_archive (backing);
    enumerator = g_file_enumerate_children (root, "*", 0, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (enumerator);
    info = g_file_enumerator_next_file (enumerator, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (info);
    g_assert_cmpstr (g_file_info_get_name (info), ==, "uri # % café.txt");
    g_object_unref (info);
    g_object_unref (enumerator);
    member = g_file_get_child (root, "uri # % café.txt");
    g_assert_true (g_file_load_contents (member, NULL, &bytes, &length, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (bytes, ==, payload);
    check_member_seek (member, (const guint8 *) payload, strlen (payload));
    g_assert_true (uselocale ((locale_t) 0) == LC_GLOBAL_LOCALE);
    g_assert_cmpstr (setlocale (LC_CTYPE, NULL), ==, "C");
    g_free (bytes);
    g_object_unref (member);
    g_object_unref (root);
    g_object_unref (backing);
    g_assert_nonnull (setlocale (LC_CTYPE, previous));
    g_free (previous);
    g_assert_cmpint (g_unlink (path), ==, 0);
    g_free (path);
}

typedef struct {
    guint32 block, size;
    gboolean move;
} IsoFixtureDirectory;

static guint32
iso_fixture_u32 (const guint8 *bytes, gboolean big_endian)
{
    guint32 value;
    memcpy (&value, bytes, sizeof value);
    return big_endian ? GUINT32_FROM_BE (value) : GUINT32_FROM_LE (value);
}

static void
iso_fixture_put_u32 (guint8 *bytes, guint32 value, gboolean big_endian)
{
    value = big_endian ? GUINT32_TO_BE (value) : GUINT32_TO_LE (value);
    memcpy (bytes, &value, sizeof value);
}

static void
iso_fixture_put_both (guint8 *bytes, guint32 value)
{
    iso_fixture_put_u32 (bytes, value, FALSE);
    iso_fixture_put_u32 (bytes + 4, value, TRUE);
}

static void
iso_fixture_walk (const guint8 *bytes, gsize length, guint32 block, guint32 size,
                  gboolean move, GArray *directories, GArray *records)
{
    IsoFixtureDirectory directory = { block, size, move };
    gsize position = (gsize) block * 2048;
    gsize end = position + size;
    g_assert_cmpuint (end, <=, length);
    g_assert_cmpuint (size % 2048, ==, 0);
    for (guint i = 0; i < directories->len; i++)
        g_assert_cmpuint (g_array_index (directories, IsoFixtureDirectory, i).block, !=, block);
    g_array_append_val (directories, directory);
    while (position < end) {
        guint record_size = bytes[position];
        if (record_size == 0) {
            position = (position / 2048 + 1) * 2048;
            continue;
        }
        g_assert_cmpuint (record_size, >=, 34);
        g_assert_cmpuint (position + record_size, <=, end);
        guint name_length = bytes[position + 32];
        const guint8 *name = bytes + position + 33;
        g_assert_cmpuint (33 + name_length, <=, record_size);
        g_array_append_val (records, position);
        if ((bytes[position + 25] & 2) &&
            !(name_length == 1 && (name[0] == 0 || name[0] == 1))) {
            gboolean child_move = move || (name_length == 3 && memcmp (name, "USR", 3) == 0);
            iso_fixture_walk (bytes, length, iso_fixture_u32 (bytes + position + 2, FALSE),
                              iso_fixture_u32 (bytes + position + 10, FALSE),
                              child_move, directories, records);
        }
        position += record_size;
    }
}

static int
iso_fixture_directory_order (gconstpointer a, gconstpointer b)
{
    const IsoFixtureDirectory *left = a, *right = b;
    return (left->block > right->block) - (left->block < right->block);
}

static void
iso_fixture_fix_susp (guint8 *bytes, gsize length, gsize position, gsize end,
                      GHashTable *moved, guint depth)
{
    g_assert_cmpuint (depth, <, 8);
    g_assert_cmpuint (end, <=, length);
    while (position + 4 <= end && bytes[position + 2] >= 4) {
        guint record_size = bytes[position + 2];
        g_assert_cmpuint (position + record_size, <=, end);
        if (memcmp (bytes + position, "CL", 2) == 0 ||
            memcmp (bytes + position, "PL", 2) == 0) {
            g_assert_cmpuint (record_size, >=, 12);
            guint32 block = iso_fixture_u32 (bytes + position + 4, FALSE);
            guint32 replacement = GPOINTER_TO_UINT (g_hash_table_lookup (moved, GUINT_TO_POINTER (block)));
            if (replacement)
                iso_fixture_put_both (bytes + position + 4, replacement);
        } else if (memcmp (bytes + position, "CE", 2) == 0) {
            g_assert_cmpuint (record_size, >=, 28);
            gsize continuation = (gsize) iso_fixture_u32 (bytes + position + 4, FALSE) * 2048 +
                                iso_fixture_u32 (bytes + position + 12, FALSE);
            iso_fixture_fix_susp (bytes, length, continuation,
                                  continuation + iso_fixture_u32 (bytes + position + 20, FALSE),
                                  moved, depth + 1);
        }
        position += record_size;
    }
}

static void
iso_fixture_move_parents_after_files (const char *path)
{
    char *contents = NULL;
    gsize length;
    g_assert_true (g_file_get_contents (path, &contents, &length, NULL));
    g_autofree guint8 *bytes = (guint8 *) contents;
    const gsize pvd = 16 * 2048;
    g_assert_cmpuint (length, >, pvd + 2048);
    g_assert_cmpuint (length, <, 1024 * 1024);
    g_assert_cmpuint (length % 2048, ==, 0);
    g_assert_cmpmem (bytes + pvd, 7, "\1CD001\1", 7);
    g_autoptr (GArray) directories = g_array_new (FALSE, FALSE, sizeof (IsoFixtureDirectory));
    g_autoptr (GArray) records = g_array_new (FALSE, FALSE, sizeof (gsize));
    g_autoptr (GHashTable) moved = g_hash_table_new (g_direct_hash, g_direct_equal);
    iso_fixture_walk (bytes, length, iso_fixture_u32 (bytes + pvd + 158, FALSE),
                      iso_fixture_u32 (bytes + pvd + 166, FALSE), FALSE, directories, records);
    g_array_sort (directories, iso_fixture_directory_order);
    guint32 next_block = length / 2048;
    for (guint i = 0; i < directories->len; i++) {
        IsoFixtureDirectory directory = g_array_index (directories, IsoFixtureDirectory, i);
        if (directory.move) {
            g_hash_table_insert (moved, GUINT_TO_POINTER (directory.block), GUINT_TO_POINTER (next_block));
            next_block += directory.size / 2048;
        }
    }
    g_assert_cmpuint (g_hash_table_size (moved), ==, 8);
    for (guint i = 0; i < records->len; i++) {
        gsize position = g_array_index (records, gsize, i);
        guint32 block = iso_fixture_u32 (bytes + position + 2, FALSE);
        guint32 replacement = GPOINTER_TO_UINT (g_hash_table_lookup (moved, GUINT_TO_POINTER (block)));
        if (replacement)
            iso_fixture_put_both (bytes + position + 2, replacement);
        guint name_length = bytes[position + 32];
        iso_fixture_fix_susp (bytes, length, position + 33 + name_length + (name_length + 1) % 2,
                              position + bytes[position], moved, 0);
    }
    guint32 table_size = iso_fixture_u32 (bytes + pvd + 132, FALSE);
    for (guint offset = 140; offset <= 152; offset += 4) {
        gboolean big_endian = offset >= 148;
        guint32 block = iso_fixture_u32 (bytes + pvd + offset, big_endian);
        if (!block)
            continue;
        gsize position = (gsize) block * 2048, end = position + table_size;
        g_assert_cmpuint (end, <=, length);
        while (position < end) {
            g_assert_cmpuint (position + 8, <=, end);
            block = iso_fixture_u32 (bytes + position + 2, big_endian);
            guint32 replacement = GPOINTER_TO_UINT (g_hash_table_lookup (moved, GUINT_TO_POINTER (block)));
            if (replacement)
                iso_fixture_put_u32 (bytes + position + 2, replacement, big_endian);
            position += 8 + bytes[position] + bytes[position] % 2;
        }
        g_assert_cmpuint (position, ==, end);
    }
    guint32 volume_blocks = next_block + 16;
    bytes = g_realloc (bytes, (gsize) volume_blocks * 2048);
    for (guint i = 0; i < directories->len; i++) {
        IsoFixtureDirectory directory = g_array_index (directories, IsoFixtureDirectory, i);
        guint32 replacement = GPOINTER_TO_UINT (g_hash_table_lookup (moved, GUINT_TO_POINTER (directory.block)));
        if (replacement)
            memcpy (bytes + (gsize) replacement * 2048, bytes + (gsize) directory.block * 2048, directory.size);
    }
    /* Retain trailing padding after the reordered tree for ISO readers which
     * read ahead one sector past the final directory block. */
    memset (bytes + (gsize) next_block * 2048, 0, 16 * 2048);
    iso_fixture_put_both (bytes + pvd + 80, volume_blocks);
    g_assert_true (g_file_set_contents (path, (char *) bytes, (gsize) volume_blocks * 2048, NULL));
}

static void
test_iso_rock_ridge_relocation (void)
{
    const char *paths[] = {
        "usr/local/opnsense/www/themes/opnsense-dark/assets/fonts/bootstrap/glyphicons-halflings-regular.eot",
        "usr/local/opnsense/www/themes/opnsense-dark/build/fonts/bootstrap/glyphicons-halflings-regular.eot",
    };
    g_autofree char *path = g_build_filename (fixture_dir, "rock-ridge-only.iso", NULL);
    g_autofree char *source = g_build_filename (fixture_dir, "rock-ridge-source", NULL);
    g_autofree char *tool = g_find_program_in_path ("xorriso");
    if (tool == NULL)
        g_error ("Rock Ridge relocation regression requires xorriso");
    /* Equal size and metadata, but distinct extents and bytes. Treating these
     * relocated files as duplicate aliases would silently return wrong data. */
    for (guint i = 0; i < G_N_ELEMENTS (paths); i++) {
        g_autofree char *filename = g_build_filename (source, paths[i], NULL);
        g_autofree char *parent = g_path_get_dirname (filename);
        g_autofree char *bytes = g_strnfill (20127, 'a' + i);
        g_assert_cmpint (g_mkdir_with_parents (parent, 0700), ==, 0);
        g_assert_true (g_file_set_contents (filename, bytes, -1, NULL));
        struct utimbuf timestamp = { 1700000000, 1700000000 };
        g_assert_cmpint (g_utime (filename, &timestamp), ==, 0);
    }
    /* libarchive's ISO writer itself fails with "Unable to insert rr_moved
     * entry" for these siblings. Use an independent ISO author, without Joliet. */
    const char *argv[] = { tool, "-as", "mkisofs", "-quiet", "-R",
                          "-rr_reloc_dir", ".rr_moved", "-o", path, source, NULL };
    GError *error = NULL;
    g_autofree char *output = NULL, *diagnostic = NULL;
    int status;
    g_assert_true (g_spawn_sync (NULL, (char **) argv, NULL, 0, NULL, NULL,
                                 &output, &diagnostic, &status, &error));
    g_assert_no_error (error);
    if (!g_spawn_check_wait_status (status, &error))
        g_error ("Could not generate Rock Ridge ISO: %s: %s", error->message, diagnostic);
    /* xorriso normally puts all directories before file data. FreeBSD images
     * can put logical CL ancestors later: libarchive then emits both files
     * prematurely under the same .rr_moved path. Preserve every ISO reference,
     * including path tables and Rock Ridge parent links, while changing order. */
    iso_fixture_move_parents_after_files (path);
    for (guint i = 0; i < G_N_ELEMENTS (paths); i++) {
        g_autofree char *filename = g_build_filename (source, paths[i], NULL);
        char *directory = g_path_get_dirname (filename);
        g_assert_cmpint (g_unlink (filename), ==, 0);
        while (!g_str_equal (directory, fixture_dir)) {
            int removed = g_rmdir (directory);
            if (removed != 0) {
                g_assert_cmpint (errno, ==, ENOTEMPTY);
                break;
            }
            char *parent = g_path_get_dirname (directory);
            g_free (directory);
            directory = parent;
        }
        g_free (directory);
    }
    for (guint i = 0; i < G_N_ELEMENTS (paths); i++) {
        g_autofree char *reference = g_strdup_printf ("%s/reference-%u", fixture_dir, i);
        g_autofree char *canonical = g_strconcat ("/", paths[i], NULL);
        const char *verify[] = { tool, "-osirrox", "on", "-indev", path,
                                "-extract", canonical, reference, NULL };
        g_clear_pointer (&output, g_free);
        g_clear_pointer (&diagnostic, g_free);
        g_assert_true (g_spawn_sync (NULL, (char **) verify, NULL, 0, NULL, NULL,
                                     &output, &diagnostic, &status, &error));
        g_assert_no_error (error);
        if (!g_spawn_check_wait_status (status, &error))
            g_error ("Independent ISO validation failed: %s: %s", error->message, diagnostic);
        g_autofree char *expected = g_strnfill (20127, 'a' + i);
        g_autofree char *actual = NULL;
        gsize size;
        g_assert_true (g_file_get_contents (reference, &actual, &size, NULL));
        g_assert_cmpmem (actual, size, expected, 20127);
        g_assert_cmpint (g_unlink (reference), ==, 0);
    }
    GFile *backing = g_file_new_for_path (path);
    GFile *root = nemo_archive_file_new_for_archive (backing);
    for (guint i = 0; i < G_N_ELEMENTS (paths); i++) {
        GFile *member = g_file_resolve_relative_path (root, paths[i]);
        char *contents = NULL;
        gsize size = 0;
        gboolean loaded = g_file_load_contents (member, NULL, &contents, &size, NULL, &error);
        g_assert_no_error (error);
        g_assert_true (loaded);
        g_assert_cmpuint (size, ==, 20127);
        for (gsize j = 0; j < size; j++)
            g_assert_cmpint (contents[j], ==, 'a' + i);
        check_member_seek (member, (const guint8 *) contents, size);
        g_free (contents);
        g_object_unref (member);
    }
    GFileEnumerator *children = g_file_enumerate_children (root, "standard::*", 0, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (children);
    GFileInfo *info = g_file_enumerator_next_file (children, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (info);
    g_assert_cmpstr (g_file_info_get_name (info), ==, "usr");
    g_assert_cmpint (g_file_info_get_file_type (info), ==, G_FILE_TYPE_DIRECTORY);
    g_object_unref (info);
    g_assert_null (g_file_enumerator_next_file (children, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_file_enumerator_close (children, NULL, &error));
    g_assert_no_error (error);
    g_object_unref (children);
    g_object_unref (root);
    g_object_unref (backing);
    g_assert_cmpint (g_unlink (path), ==, 0);
}

#define LARGE_ENTRIES 120000
#define LARGE_BUCKETS 120
#define READER_RSS_LIMIT_KIB (96 * 1024)

static char *
scale_member_name (guint ordinal, const char *layout)
{
    char padding[193];
    memset (padding, 'x', sizeof padding - 1);
    padding[sizeof padding - 1] = '\0';
    if (g_str_equal (layout, "spread"))
        return g_strdup_printf ("bucket-%03u/%s/%s/member-%06u-%s.bin",
                                ordinal % LARGE_BUCKETS, padding, padding, ordinal, padding);
    if (g_str_equal (layout, "short"))
        return g_strdup_printf ("flat/member-%06u.bin", ordinal);
    return g_strdup_printf ("flat/member-%06u-%s.bin", ordinal, padding);
}

static void
write_scale_fixture (const char *path, const char *layout, guint count, const char *tail)
{
    struct archive *writer = archive_write_new ();
    guint64 pathname_bytes = 0;
    g_assert_cmpint (archive_write_set_format_zip (writer), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_set_options (writer, "zip:compression=store"), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_open_filename (writer, path), ==, ARCHIVE_OK);
    for (guint i = 0; i < count; i++) {
        g_autofree char *name = scale_member_name (i, layout);
        pathname_bytes += strlen (name);
        add_entry (writer, name, i == count - 1 ? payload : NULL, AE_IFREG);
    }
    if (g_str_equal (tail, "unsafe")) {
        add_entry (writer, "../outside", "unsafe", AE_IFREG);
    } else if (g_str_equal (tail, "duplicate")) {
        g_autofree char *name = scale_member_name (count - 1, layout);
        add_entry (writer, name, "duplicate", AE_IFREG);
    }
    g_assert_cmpint (archive_write_close (writer), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_free (writer), ==, ARCHIVE_OK);
    if (g_str_equal (layout, "spread") && count == LARGE_ENTRIES)
        g_assert_cmpuint (pathname_bytes, >, 64 * 1024 * 1024);
}

static char *
scale_fixture (const char *name, const char *layout, guint count, const char *tail)
{
    char *path = g_build_filename (fixture_dir, name, NULL);
    g_autofree char *number = g_strdup_printf ("%u", count);
    const char *argv[] = { test_program, "--write-scale-fixture", path, layout, number, tail, NULL };
    GError *error = NULL;
    int status;
    /* libarchive's ZIP writer retains its central directory. Generate fixtures
     * outside the measured reader process so this cannot hide reader growth. */
    g_assert_true (g_spawn_sync (NULL, (char **) argv, NULL, 0, NULL, NULL,
                                 NULL, NULL, &status, &error));
    g_assert_no_error (error);
    g_assert_true (g_spawn_check_wait_status (status, &error));
    g_assert_no_error (error);
    return path;
}

static glong
peak_rss_kib (void)
{
    g_autofree char *status = NULL;
    g_assert_true (g_file_get_contents ("/proc/self/status", &status, NULL, NULL));
    /* ru_maxrss can retain a much larger launcher's peak across exec, hiding
     * this reader's allocations. VmHWM belongs to the current process image. */
    const char *field = strstr (status, "\nVmHWM:");
    g_assert_nonnull (field);
    char *end = NULL;
    gint64 peak = g_ascii_strtoll (field + strlen ("\nVmHWM:"), &end, 10);
    g_assert_cmpint (peak, >, 0);
    g_assert_cmpint (peak, <=, G_MAXLONG);
    g_assert_true (g_str_has_prefix (end, " kB"));
    return peak;
}

static GFile *
scale_root (const char *path)
{
    GFile *backing = g_file_new_for_path (path);
    GFile *root = nemo_archive_file_new_for_archive (backing);
    g_object_unref (backing);
    return root;
}

static guint
count_children (GFile *directory, guint expected)
{
    GError *error = NULL;
    GFileEnumerator *enumerator = g_file_enumerate_children (
        directory, "*", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (enumerator);
    GFileInfo *info;
    guint count = 0;
    while ((info = g_file_enumerator_next_file (enumerator, NULL, &error)) != NULL) {
        g_assert_nonnull (g_file_info_get_name (info));
        g_assert_false (g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE));
        count++;
        g_object_unref (info);
    }
    g_assert_no_error (error);
    g_assert_cmpuint (count, ==, expected);
    g_assert_true (g_file_enumerator_close (enumerator, NULL, &error));
    g_assert_no_error (error);
    g_object_unref (enumerator);
    return count;
}

static void
assert_late_member (GFile *root, const char *layout, guint count)
{
    g_autofree char *name = scale_member_name (count - 1, layout);
    GFile *member = g_file_resolve_relative_path (root, name);
    GError *error = NULL;
    char *contents = NULL;
    gsize length = 0;
    g_assert_true (g_file_load_contents (member, NULL, &contents, &length, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpmem (contents, length, payload, strlen (payload));
    g_free (contents);
    g_object_unref (member);
}

static void
test_large_spread (void)
{
    g_autofree char *path = scale_fixture ("spread.zip", "spread", LARGE_ENTRIES, "none");
    GFile *root = scale_root (path);
    glong before = peak_rss_kib ();
    gint64 start = g_get_monotonic_time ();
    g_atomic_int_set (&header_reads, 0);
    count_children (root, LARGE_BUCKETS);
    gint scanned = g_atomic_int_get (&header_reads);
    g_assert_cmpint (scanned, >=, LARGE_ENTRIES);
    g_assert_cmpint (scanned, <=, LARGE_ENTRIES + 2);

    g_autofree char *last = scale_member_name (LARGE_ENTRIES - 1, "spread");
    GFile *member = g_file_resolve_relative_path (root, last);
    GFile *directory = g_file_get_parent (member);
    count_children (directory, LARGE_ENTRIES / LARGE_BUCKETS);
    g_autofree char *uri = g_file_get_uri (root);
    for (guint i = 0; i < 20; i++) {
        GFile *fresh_root = g_file_new_for_uri (uri);
        GFile *fresh = g_file_resolve_relative_path (fresh_root, last);
        GError *error = NULL;
        GFileInfo *info = g_file_query_info (fresh, "*", 0, NULL, &error);
        g_assert_no_error (error);
        g_assert_nonnull (info);
        g_assert_cmpint (g_file_info_get_size (info), ==, strlen (payload));
        g_object_unref (info);
        g_object_unref (fresh);
        g_object_unref (fresh_root);
    }
    g_assert_cmpint (g_atomic_int_get (&header_reads), ==, scanned);
    gint64 index_elapsed = g_get_monotonic_time () - start;
    glong peak = peak_rss_kib ();
    glong growth = MAX (0, peak - before);
    g_assert_cmpint (peak, <, READER_RSS_LIMIT_KIB);
    assert_late_member (root, "spread", LARGE_ENTRIES);
    g_autofree char *extracted = g_build_filename (fixture_dir, "bucket-000", NULL);
    g_assert_false (g_file_test (extracted, G_FILE_TEST_EXISTS));
    g_printerr ("archive-scale spread: entries=%u index-and-queries-ms=%" G_GINT64_FORMAT
                " peak-rss-kib=%ld peak-rss-growth-kib=%ld metadata-header-reads=%d\n",
                LARGE_ENTRIES, index_elapsed / 1000, peak, growth, scanned);
    g_object_unref (directory);
    g_object_unref (member);
    g_object_unref (root);
    g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
test_large_flat (void)
{
    g_autofree char *path = scale_fixture ("flat.zip", "flat", LARGE_ENTRIES, "none");
    GFile *root = scale_root (path);
    GFile *flat = g_file_get_child (root, "flat");
    GError *error = NULL;
    glong before = peak_rss_kib ();
    gint64 start = g_get_monotonic_time ();
    g_atomic_int_set (&info_allocations, 0);
    g_atomic_int_set (&header_reads, 0);
    GFileEnumerator *enumerator = g_file_enumerate_children (flat, "*", 0, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (enumerator);
    /* Constructing a cursor must not construct one rich GFileInfo per member. */
    g_assert_cmpint (g_atomic_int_get (&info_allocations), <=, 64);
    gint scanned = g_atomic_int_get (&header_reads);
    GCancellable *cancel = g_cancellable_new ();
    for (guint i = 0; i < 10; i++) {
        GFileInfo *info = g_file_enumerator_next_file (enumerator, cancel, &error);
        g_assert_no_error (error);
        g_assert_nonnull (info);
        g_object_unref (info);
    }
    g_assert_cmpint (g_atomic_int_get (&info_allocations), <=, 74);
    g_cancellable_cancel (cancel);
    g_assert_null (g_file_enumerator_next_file (enumerator, cancel, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&error);
    g_assert_true (g_file_enumerator_close (enumerator, NULL, &error));
    g_assert_no_error (error);
    g_object_unref (enumerator);
    g_object_unref (cancel);
    count_children (flat, LARGE_ENTRIES);
    g_assert_cmpint (g_atomic_int_get (&header_reads), ==, scanned);
    glong peak = peak_rss_kib ();
    glong growth = MAX (0, peak - before);
    g_assert_cmpint (peak, <, READER_RSS_LIMIT_KIB);
    g_printerr ("archive-scale flat: entries=%u streamed-ms=%" G_GINT64_FORMAT
                " peak-rss-kib=%ld peak-rss-growth-kib=%ld metadata-header-reads=%d\n",
                LARGE_ENTRIES, (g_get_monotonic_time () - start) / 1000, peak, growth, scanned);
    assert_late_member (root, "flat", LARGE_ENTRIES);
    g_autofree char *extracted = g_build_filename (fixture_dir, "flat", NULL);
    g_assert_false (g_file_test (extracted, G_FILE_TEST_EXISTS));
    g_object_unref (flat);
    g_object_unref (root);
    g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
test_index_cancellation (void)
{
    g_autofree char *path = scale_fixture ("cancel.zip", "short", 6000, "none");
    GFile *root = scale_root (path);
    GCancellable *cancel = g_cancellable_new ();
    GError *error = NULL;
    g_atomic_int_set (&header_reads, 0);
    g_atomic_int_set (&cancel_at_header, 1000);
    g_atomic_pointer_set (&header_cancel, cancel);
    g_assert_null (g_file_enumerate_children (root, "*", 0, cancel, &error));
    g_atomic_pointer_set (&header_cancel, NULL);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_true (g_cancellable_is_cancelled (cancel));
    g_assert_cmpint (g_atomic_int_get (&header_reads), >=, 1000);
    g_assert_cmpint (g_atomic_int_get (&header_reads), <, 6000);
    g_clear_error (&error);
    GFile *flat = g_file_get_child (root, "flat");
    count_children (flat, 6000);
    assert_late_member (root, "short", 6000);
    g_object_unref (flat);
    g_object_unref (cancel);
    g_object_unref (root);
    g_assert_cmpint (g_unlink (path), ==, 0);
}

typedef struct {
    GMutex mutex;
    GCond cond;
    guint ready;
    gboolean start;
    char *uri;
    char *member;
} QueryGroup;

static gpointer
query_together (gpointer data)
{
    QueryGroup *group = data;
    GFile *root = g_file_new_for_uri (group->uri);
    GFile *member = g_file_resolve_relative_path (root, group->member);
    g_mutex_lock (&group->mutex);
    group->ready++;
    g_cond_broadcast (&group->cond);
    while (!group->start)
        g_cond_wait (&group->cond, &group->mutex);
    g_mutex_unlock (&group->mutex);
    GError *error = NULL;
    GFileInfo *info = g_file_query_info (member, "*", 0, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (info);
    g_assert_cmpint (g_file_info_get_size (info), ==, strlen (payload));
    g_object_unref (info);
    g_object_unref (member);
    g_object_unref (root);
    return NULL;
}

static void
test_concurrent_index (void)
{
    g_autofree char *path = scale_fixture ("concurrent.zip", "short", 10000, "none");
    GFile *root = scale_root (path);
    QueryGroup group = { 0 };
    group.uri = g_file_get_uri (root);
    group.member = scale_member_name (9999, "short");
    g_mutex_init (&group.mutex);
    g_cond_init (&group.cond);
    GThread *threads[8];
    g_atomic_int_set (&header_reads, 0);
    for (guint i = 0; i < G_N_ELEMENTS (threads); i++)
        threads[i] = g_thread_new ("archive-query", query_together, &group);
    g_mutex_lock (&group.mutex);
    while (group.ready != G_N_ELEMENTS (threads))
        g_cond_wait (&group.cond, &group.mutex);
    group.start = TRUE;
    g_cond_broadcast (&group.cond);
    g_mutex_unlock (&group.mutex);
    for (guint i = 0; i < G_N_ELEMENTS (threads); i++)
        g_thread_join (threads[i]);
    g_assert_cmpint (g_atomic_int_get (&header_reads), >=, 10000);
    g_assert_cmpint (g_atomic_int_get (&header_reads), <=, 10002);
    g_printerr ("archive-scale concurrent: readers=8 entries=10000 metadata-header-reads=%d\n",
                g_atomic_int_get (&header_reads));
    g_cond_clear (&group.cond);
    g_mutex_clear (&group.mutex);
    g_free (group.member);
    g_free (group.uri);
    g_object_unref (root);
    g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
test_cached_source_change (void)
{
    g_autofree char *path = scale_fixture ("old.zip", "short", 20, "none");
    GFile *root = scale_root (path);
    count_children (root, 1);
    GFile *old_member = g_file_resolve_relative_path (root, "flat/member-000019.bin");
    GError *error = NULL;
    GFileInfo *before = g_file_query_info (old_member, "*", 0, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (before);
    g_autofree char *replacement = make_archive ("replacement.zip", "zip", FALSE, FALSE);
    g_assert_cmpint (g_rename (replacement, path), ==, 0);
    gint scanned = g_atomic_int_get (&header_reads);
    GFile *new_member = g_file_get_child (root, "second.txt");
    GFileInfo *after = g_file_query_info (new_member, "*", 0, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (after);
    g_assert_cmpint (g_file_info_get_size (after), ==, strlen ("second\n"));
    g_assert_cmpstr (g_file_info_get_etag (before), !=, g_file_info_get_etag (after));
    g_assert_cmpint (g_atomic_int_get (&header_reads), >, scanned);
    g_assert_null (g_file_query_info (old_member, "*", 0, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error (&error);
    scanned = g_atomic_int_get (&header_reads);
    count_children (root, 2);
    g_assert_cmpint (g_atomic_int_get (&header_reads), ==, scanned);
    g_object_unref (after);
    g_object_unref (before);
    g_object_unref (new_member);
    g_object_unref (old_member);
    g_object_unref (root);
    g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
test_late_invalid (void)
{
    const char *tails[] = { "unsafe", "duplicate" };
    for (guint i = 0; i < G_N_ELEMENTS (tails); i++) {
        g_autofree char *path = scale_fixture ("late-invalid.zip", "short", 100001, tails[i]);
        GFile *root = scale_root (path);
        GFile *first = g_file_resolve_relative_path (root, "flat/member-000000.bin");
        GError *error = NULL;
        g_atomic_int_set (&header_reads, 0);
        /* Even a first-member query must validate the archive's final entry. */
        g_assert_null (g_file_query_info (first, "*", 0, NULL, &error));
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_assert_cmpint (g_atomic_int_get (&header_reads), >, 100000);
        g_clear_error (&error);
        g_assert_null (g_file_enumerate_children (root, "*", 0, NULL, &error));
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error (&error);
        g_object_unref (first);
        g_object_unref (root);
        g_assert_cmpint (g_unlink (path), ==, 0);
    }
}

typedef struct {
    const char *name;
    GTestFunc run;
} ScaleCase;

static void
run_scale_case (gconstpointer data)
{
    const ScaleCase *test = data;
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 120 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_INHERIT_STDERR);
        g_test_trap_assert_passed ();
        return;
    }
    test->run ();
}

int
main (int argc, char **argv)
{
    int result;
    if (argc == 6 && g_str_equal (argv[1], "--write-scale-fixture")) {
        write_scale_fixture (argv[2], argv[3], g_ascii_strtoull (argv[4], NULL, 10), argv[5]);
        return 0;
    }
    test_program = g_canonicalize_filename (argv[0], NULL);
    g_test_init (&argc, &argv, NULL);
    fixture_dir = g_strdup_printf (".archive-provider-test-%08x-%08x", g_random_int (), g_random_int ());
    g_assert_cmpint (g_mkdir (fixture_dir, 0700), ==, 0);
    g_autofree char *cache_home = g_canonicalize_filename (fixture_dir, NULL);
    g_assert_true (g_setenv ("XDG_CACHE_HOME", cache_home, TRUE));
    g_assert_cmpstr (g_get_user_cache_dir (), ==, cache_home);
    nemo_archive_file_register ();
    g_test_add_func ("/archive-file/formats", test_formats);
    g_test_add_func ("/archive-file/bsdtar-7z", test_bsdtar);
    g_test_add_func ("/archive-file/invalid-and-links", test_invalid);
    g_test_add_func ("/archive-file/cancellation-and-change", test_cancel_and_change);
    g_test_add_func ("/archive-file/nested-seek", test_nested_seek);
    g_test_add_func ("/archive-file/default-async", test_async);
    g_test_add_func ("/archive-file/encrypted-and-corrupt", test_encrypted_and_corrupt);
    g_test_add_func ("/archive-file/unicode-c-locale", test_unicode_c_locale);
    g_test_add_func ("/archive-file/iso-rock-ridge-relocation", test_iso_rock_ridge_relocation);
    static const ScaleCase scale_cases[] = {
        { "/archive-file/large/spread", test_large_spread },
        { "/archive-file/large/flat-streaming", test_large_flat },
        { "/archive-file/large/index-cancellation", test_index_cancellation },
        { "/archive-file/large/concurrent-cache", test_concurrent_index },
        { "/archive-file/large/cache-source-change", test_cached_source_change },
        { "/archive-file/large/late-invalid", test_late_invalid },
    };
    for (guint i = 0; i < G_N_ELEMENTS (scale_cases); i++)
        g_test_add_data_func (scale_cases[i].name, &scale_cases[i], run_scale_case);
    result = g_test_run ();
    g_autofree char *cache_parent = g_build_filename (fixture_dir, "nemo", NULL);
    g_autofree char *cache_directory = g_build_filename (cache_parent, "archive-metadata", NULL);
    if (g_file_test (cache_directory, G_FILE_TEST_EXISTS))
        g_assert_cmpint (g_rmdir (cache_directory), ==, 0);
    if (g_file_test (cache_parent, G_FILE_TEST_EXISTS))
        g_assert_cmpint (g_rmdir (cache_parent), ==, 0);
    g_assert_cmpint (g_rmdir (fixture_dir), ==, 0);
    g_free (fixture_dir);
    g_free (test_program);
    return result;
}
