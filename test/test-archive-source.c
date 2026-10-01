/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../src/nemo-archive-source.h"
#include "../libnemo-private/nemo-archive-file.h"

#include <archive.h>
#include <archive_entry.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <string.h>

static char *fixture_dir;
static char *program_path;

static void
write_u32 (guint8 *bytes, guint32 value)
{
    value = GUINT32_TO_LE (value);
    memcpy (bytes, &value, sizeof value);
}

static void
write_u16 (guint8 *bytes, guint16 value)
{
    value = GUINT16_TO_LE (value);
    memcpy (bytes, &value, sizeof value);
}

static char *
make_wav_archive (const char *format)
{
    guint8 *wav = g_malloc0 (32044);
    char *path = g_build_filename (fixture_dir, format, NULL);
    struct archive *writer = archive_write_new ();
    struct archive_entry *entry = archive_entry_new ();
    guint i;
    memcpy (wav, "RIFF", 4);
    write_u32 (wav + 4, 32036);
    memcpy (wav + 8, "WAVEfmt ", 8);
    write_u32 (wav + 16, 16);
    write_u16 (wav + 20, 1);
    write_u16 (wav + 22, 1);
    write_u32 (wav + 24, 8000);
    write_u32 (wav + 28, 16000);
    write_u16 (wav + 32, 2);
    write_u16 (wav + 34, 16);
    memcpy (wav + 36, "data", 4);
    write_u32 (wav + 40, 32000);
    for (i = 0; i < 16000; i++)
        write_u16 (wav + 44 + 2 * i, i % 256);
    g_assert_cmpint (archive_write_set_format_by_name (writer, format), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_open_filename (writer, path), ==, ARCHIVE_OK);
    archive_entry_set_pathname (entry, "sound # %.wav");
    archive_entry_set_filetype (entry, AE_IFREG);
    archive_entry_set_perm (entry, 0644);
    archive_entry_set_size (entry, 32044);
    g_assert_cmpint (archive_write_header (writer, entry), ==, ARCHIVE_OK);
    g_assert_cmpint (archive_write_data (writer, wav, 32044), ==, 32044);
    g_assert_cmpint (archive_write_close (writer), ==, ARCHIVE_OK);
    archive_write_free (writer);
    archive_entry_free (entry);
    g_free (wav);
    return path;
}

static void
audio_handoff (GstElement *sink, GstBuffer *buffer, GstPad *pad, gpointer data)
{
    gint *buffers = data;
    GstCaps *caps = gst_pad_get_current_caps (pad);
    g_assert_nonnull (caps);
    g_assert_cmpstr (gst_structure_get_name (gst_caps_get_structure (caps, 0)), ==, "audio/x-raw");
    g_assert_cmpuint (gst_buffer_get_size (buffer), >, 0);
    gst_caps_unref (caps);
    g_atomic_int_inc (buffers);
}

static void
assert_state (GstElement *pipeline, GstState state)
{
    GstStateChangeReturn result = gst_element_set_state (pipeline, state);
    GstBus *bus = gst_element_get_bus (pipeline);
    GstMessage *message;
    if (result != GST_STATE_CHANGE_FAILURE)
        result = gst_element_get_state (pipeline, NULL, NULL, 15 * GST_SECOND);
    message = result == GST_STATE_CHANGE_SUCCESS ? NULL : gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
    if (message) {
        GError *error = NULL;
        char *debug = NULL;
        gst_message_parse_error (message, &error, &debug);
        g_error ("Pipeline failed: %s (%s)", error->message, debug ? debug : "");
    }
    gst_object_unref (bus);
    g_assert_cmpint (result, ==, GST_STATE_CHANGE_SUCCESS);
}

static void
test_decode_seek (gconstpointer data)
{
    const char *format = data;
    char *path = make_wav_archive (format);
    GFile *backing = g_file_new_for_path (path);
    GFile *root = nemo_archive_file_new_for_archive (backing);
    GFile *member = g_file_get_child (root, "sound # %.wav");
    char *uri = g_file_get_uri (member);
    GError *error = NULL;
    GstElement *source, *player, *audio, *video;
    GstBus *bus;
    GstMessage *message;
    GstQuery *query;
    gboolean seekable = FALSE;
    gint64 duration = 0;
    gint buffers = 0;

    source = gst_element_make_from_uri (GST_URI_SRC, uri, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (source);
    g_assert_cmpstr (gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (gst_element_get_factory (source))),
                    ==, "nemoarchivesrc");
    g_assert_false (gst_uri_handler_set_uri (GST_URI_HANDLER (source), "file:///etc/passwd", &error));
    g_assert_error (error, GST_URI_ERROR, GST_URI_ERROR_UNSUPPORTED_PROTOCOL);
    g_clear_error (&error);
    g_assert_false (gst_uri_handler_set_uri (GST_URI_HANDLER (source), "nemo-archive://invalid/../escape", &error));
    g_assert_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI);
    g_clear_error (&error);
    gst_object_unref (source);

    player = gst_element_factory_make ("playbin", NULL);
    audio = gst_element_factory_make ("fakesink", NULL);
    video = gst_element_factory_make ("fakesink", NULL);
    g_assert_nonnull (player);
    g_assert_nonnull (audio);
    g_assert_nonnull (video);
    g_object_set (audio, "sync", FALSE, "signal-handoffs", TRUE, NULL);
    g_signal_connect (audio, "handoff", G_CALLBACK (audio_handoff), &buffers);
    g_object_set (video, "sync", FALSE, NULL);
    g_object_set (player, "audio-sink", audio, "video-sink", video, "uri", uri, NULL);
    assert_state (player, GST_STATE_PAUSED);
    g_assert_true (gst_element_query_duration (player, GST_FORMAT_TIME, &duration));
    g_assert_cmpint (duration, ==, 2 * GST_SECOND);
    query = gst_query_new_seeking (GST_FORMAT_TIME);
    g_assert_true (gst_element_query (player, query));
    gst_query_parse_seeking (query, NULL, &seekable, NULL, NULL);
    g_assert_true (seekable);
    gst_query_unref (query);
    g_object_get (player, "source", &source, NULL);
    g_assert_nonnull (source);
    g_assert_false (gst_uri_handler_set_uri (GST_URI_HANDLER (source), uri, &error));
    g_assert_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_STATE);
    g_clear_error (&error);
    gst_object_unref (source);
    g_assert_true (gst_element_seek_simple (player, GST_FORMAT_TIME,
                                           GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, GST_SECOND));
    assert_state (player, GST_STATE_PAUSED);
    g_assert_true (gst_element_seek_simple (player, GST_FORMAT_TIME,
                                           GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, GST_SECOND / 2));
    assert_state (player, GST_STATE_PAUSED);
    assert_state (player, GST_STATE_PLAYING);
    bus = gst_element_get_bus (player);
    message = gst_bus_timed_pop_filtered (bus, 15 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    g_test_message ("Decoded buffers before completion: %d", g_atomic_int_get (&buffers));
    g_assert_nonnull (message);
    if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
        char *debug = NULL;
        gst_message_parse_error (message, &error, &debug);
        g_error ("Decode failed: %s (%s)", error->message, debug ? debug : "");
    }
    g_assert_cmpint (GST_MESSAGE_TYPE (message), ==, GST_MESSAGE_EOS);
    g_assert_cmpint (g_atomic_int_get (&buffers), >, 0);
    gst_message_unref (message);
    gst_object_unref (bus);
    assert_state (player, GST_STATE_NULL);
    gst_object_unref (player);
    g_free (uri);
    g_object_unref (member);
    g_object_unref (root);
    g_object_unref (backing);
    g_assert_cmpint (g_unlink (path), ==, 0);
    g_free (path);
}

static int
missing_gio_child (void)
{
    GError *error = NULL;
    GstElement *source;
    GFile *backing, *root;
    char *uri;
    g_assert_true (gst_init_check (NULL, NULL, &error));
    g_assert_no_error (error);
    g_assert_null (gst_element_factory_find ("giosrc"));
    g_assert_true (nemo_archive_source_register (&error));
    g_assert_no_error (error);
    source = gst_element_factory_make ("nemoarchivesrc", NULL);
    g_assert_nonnull (source);
    backing = g_file_new_for_path ("nonexistent-archive.zip");
    root = nemo_archive_file_new_for_archive (backing);
    uri = g_file_get_uri (root);
    g_assert_false (gst_uri_handler_set_uri (GST_URI_HANDLER (source), uri, &error));
    g_assert_error (error, GST_URI_ERROR, GST_URI_ERROR_BAD_REFERENCE);
    g_clear_error (&error);
    g_free (uri);
    g_object_unref (root);
    g_object_unref (backing);
    gst_object_unref (source);
    gst_deinit ();
    return 0;
}

static void
test_missing_gio (void)
{
    GError *error = NULL;
    char *registry = g_build_filename (fixture_dir, "missing-registry.bin", NULL);
    char **environment = g_get_environ ();
    char *arguments[] = { program_path, "--missing-gio", NULL };
    gint status;
    environment = g_environ_setenv (environment, "GST_PLUGIN_PATH_1_0", "", TRUE);
    environment = g_environ_setenv (environment, "GST_PLUGIN_SYSTEM_PATH_1_0", "", TRUE);
    environment = g_environ_setenv (environment, "GST_REGISTRY_1_0", registry, TRUE);
    environment = g_environ_setenv (environment, "GST_REGISTRY_UPDATE", "no", TRUE);
    g_assert_true (g_spawn_sync (NULL, arguments, environment, 0, NULL, NULL, NULL, NULL,
                                &status, &error));
    g_assert_no_error (error);
    g_assert_true (g_spawn_check_wait_status (status, &error));
    g_assert_no_error (error);
    g_assert_false (g_file_test (registry, G_FILE_TEST_EXISTS));
    g_strfreev (environment);
    g_free (registry);
}

int
main (int argc, char **argv)
{
    GError *error = NULL;
    GFile *program;
    int result;
    if (argc == 2 && strcmp (argv[1], "--missing-gio") == 0)
        return missing_gio_child ();
    program = g_file_new_for_path (argv[0]);
    program_path = g_file_get_path (program);
    g_object_unref (program);
    g_test_init (&argc, &argv, NULL);
    g_assert_false (nemo_archive_source_register (&error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);
    g_clear_error (&error);
    g_setenv ("GST_REGISTRY_UPDATE", "no", FALSE);
    g_assert_true (gst_init_check (&argc, &argv, &error));
    g_assert_no_error (error);
    g_assert_true (nemo_archive_source_register (&error));
    g_assert_no_error (error);
    g_assert_true (nemo_archive_source_register (&error));
    g_assert_no_error (error);
    fixture_dir = g_strdup_printf (".archive-source-test-%08x-%08x", g_random_int (), g_random_int ());
    g_assert_cmpint (g_mkdir (fixture_dir, 0700), ==, 0);
    g_test_add_data_func ("/archive-source/zip-decode-seek", "zip", test_decode_seek);
    g_test_add_data_func ("/archive-source/7z-decode-seek", "7zip", test_decode_seek);
    g_test_add_func ("/archive-source/missing-gio-deferred", test_missing_gio);
    result = g_test_run ();
    g_assert_cmpint (g_rmdir (fixture_dir), ==, 0);
    g_free (fixture_dir);
    g_free (program_path);
    gst_deinit ();
    return result;
}
