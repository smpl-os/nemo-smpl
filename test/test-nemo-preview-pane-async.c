/* Run in an isolated D-Bus/Xvfb session with private HOME/XDG directories.
 * Include the implementation to exercise completion ordering without exposing
 * worker and generation internals as public widget API. */
#include <glib/gstdio.h>
#include <unistd.h>
#include "../src/nemo-preview-pane.c"
#ifdef HAVE_DOCUMENT_PREVIEW
#include "document-preview-fixture.h"
#endif

typedef struct {
	GAsyncReadyCallback callback;
	gboolean done;
} Completion;

static NemoPreviewPane *
new_test_pane (void)
{
	return NEMO_PREVIEW_PANE (g_object_ref_sink (nemo_preview_pane_new ()));
}

static void
completed_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	Completion *completion = user_data;

	completion->callback (source, result, NULL);
	completion->done = TRUE;
}

static GTask *
new_test_task (NemoPreviewPane *pane, GFile *file, Completion *completion)
{
	GTask *task = g_task_new (NULL, pane->cancellable,
				 completed_cb, completion);

	g_task_set_task_data (task, pane_load_data_new (pane, file),
			     (GDestroyNotify) pane_load_data_free);
	return task;
}

static void
wait_for_completion (Completion *completion)
{
	gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

	while (!completion->done && g_get_monotonic_time () < deadline) {
		g_main_context_iteration (NULL, FALSE);
		g_usleep (1000);
	}
	g_assert_true (completion->done);
}

static GdkPixbuf *
new_test_pixbuf (void)
{
	GdkPixbuf *pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 256, 256);
	gdk_pixbuf_fill (pixbuf, 0xff0000ff);
	return pixbuf;
}

static void
test_stale_thumbnail (void)
{
	NemoPreviewPane *pane = new_test_pane ();
	GFile *file = g_file_new_for_path ("unused-preview-thumbnail");
	Completion completion = { thumbnail_ready_cb, FALSE };
	GTask *task = new_test_task (pane, file, &completion);
	GCancellable *cancel = g_object_ref (pane->cancellable);

	g_task_return_pointer (task, new_test_pixbuf (), g_object_unref);
	nemo_preview_pane_clear (pane);
	g_assert_true (g_cancellable_is_cancelled (cancel));
	wait_for_completion (&completion);
	g_assert_cmpint (gtk_image_get_storage_type (GTK_IMAGE (pane->info_icon)),
			 ==, GTK_IMAGE_EMPTY);
	g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (pane->stack)),
			 ==, "empty");

	g_object_unref (task);
	g_object_unref (cancel);
	g_object_unref (file);
	gtk_widget_destroy (GTK_WIDGET (pane));
	g_object_unref (pane);
}

static void
test_generation_guard (void)
{
	NemoPreviewPane *pane = new_test_pane ();
	GFile *file = g_file_new_for_path ("unused-preview-generation");
	Completion completion = { thumbnail_ready_cb, FALSE };
	GTask *task = new_test_task (pane, file, &completion);
	NemoPreviewPane *current = pane_load_get_current (task);

	g_assert_true (current == pane);
	g_object_unref (current);
	pane->generation++;
	g_assert_false (g_cancellable_is_cancelled (pane->cancellable));
	g_assert_null (pane_load_get_current (task));
	g_task_return_pointer (task, new_test_pixbuf (), g_object_unref);
	wait_for_completion (&completion);
	g_assert_cmpint (gtk_image_get_storage_type (GTK_IMAGE (pane->info_icon)),
			 ==, GTK_IMAGE_EMPTY);

	g_object_unref (task);
	g_object_unref (file);
	gtk_widget_destroy (GTK_WIDGET (pane));
	g_object_unref (pane);
}

static void
test_destroy_pending (void)
{
	NemoPreviewPane *pane = new_test_pane ();
	NemoPreviewPane *weak_pane = pane;
	GFile *file = g_file_new_for_path ("unused-preview-destroy");
	Completion completion = { thumbnail_ready_cb, FALSE };
	GTask *task = new_test_task (pane, file, &completion);
	GCancellable *cancel = g_object_ref (pane->cancellable);

	g_object_add_weak_pointer (G_OBJECT (pane), (gpointer *) &weak_pane);
	gtk_widget_destroy (GTK_WIDGET (pane));
	g_assert_true (pane->destroyed);
	g_assert_true (g_cancellable_is_cancelled (cancel));
	g_assert_null (pane_load_get_current (task));
	nemo_preview_pane_clear (pane);
	nemo_preview_pane_set_file (pane, NULL);
	nemo_preview_pane_toggle_details (pane);
	nemo_preview_pane_toggle_mute (pane);
	nemo_preview_pane_toggle_play (pane);
	g_object_run_dispose (G_OBJECT (pane));
	g_object_run_dispose (G_OBJECT (pane));
	g_object_unref (pane);
	g_assert_null (weak_pane);

	g_task_return_pointer (task, new_test_pixbuf (), g_object_unref);
	wait_for_completion (&completion);
	g_object_unref (task);
	g_object_unref (cancel);
	g_object_unref (file);
}

static void
test_thumbnail_worker (void)
{
	NemoPreviewPane *pane = new_test_pane ();
	GFile *file = g_file_new_for_path ("pane-test-thumbnail.png");
	GdkPixbuf *pixbuf = new_test_pixbuf ();
	GError *error = NULL;
	Completion completion = { thumbnail_ready_cb, FALSE };
	GTask *task = new_test_task (pane, file, &completion);

	g_assert_true (gdk_pixbuf_save (pixbuf, "pane-test-thumbnail.png", "png",
				       &error, NULL));
	g_assert_no_error (error);
	g_object_unref (pixbuf);
	nemo_preview_run_task (task, thumbnail_worker);
	wait_for_completion (&completion);
	pixbuf = gtk_image_get_pixbuf (GTK_IMAGE (pane->info_icon));
	g_assert_nonnull (pixbuf);
	g_assert_cmpint (gdk_pixbuf_get_width (pixbuf), ==, 128);
	g_assert_cmpint (gdk_pixbuf_get_height (pixbuf), ==, 128);

	g_assert_cmpint (g_remove ("pane-test-thumbnail.png"), ==, 0);
	g_object_unref (task);
	g_object_unref (file);
	gtk_widget_destroy (GTK_WIDGET (pane));
	g_object_unref (pane);
}

/* Metadata/GPS ordering and cache regressions live in test-nemo-preview-details.
 * The pane must own exactly one shared widget and tear it down with itself. */
static void
test_details_ownership (void)
{
	NemoPreviewPane *pane = new_test_pane ();
	NemoPreviewDetails *weak_details = pane->details;
	GFile *file = g_file_new_for_path ("unused-preview-details");

	g_assert_true (NEMO_IS_PREVIEW_DETAILS (pane->details));
	g_object_add_weak_pointer (G_OBJECT (pane->details), (gpointer *) &weak_details);
	nemo_preview_details_set_file (pane->details, file);
	nemo_preview_pane_clear (pane);
	g_assert_false (gtk_widget_get_visible (pane->details_scroll));
	gtk_widget_destroy (GTK_WIDGET (pane));
	g_object_unref (pane);
	g_assert_null (weak_details);
	g_object_unref (file);
}

#ifdef HAVE_DOCUMENT_PREVIEW
static void
document_file_ready (NemoFile *file, gpointer data)
{
	*(gboolean *) data = TRUE;
}

static void
test_rich_document_pane (void)
{
	gboolean sandbox = document_sandbox_available ();
	char *directory = g_dir_make_tmp ("nemo-pane-document-XXXXXX", NULL);
	char *path = g_build_filename (directory, "book.md", NULL);
	GString *contents = g_string_new ("# Native right-pane preview\n\n");
	for (guint i = 0; i < 60; i++)
		g_string_append_printf (contents, "## Section %u\n\n"
			"This document is rendered in the right preview pane without taking focus.\n\n", i);
	g_assert_true (g_file_set_contents (path, contents->str, contents->len, NULL));
	g_string_free (contents, TRUE);
	GFile *location = g_file_new_for_path (path);
	NemoFile *file = nemo_file_get (location);
	gboolean ready = FALSE;
	nemo_file_call_when_ready (file, NEMO_FILE_ATTRIBUTE_INFO, document_file_ready, &ready);
	gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
	while (!ready && g_get_monotonic_time () < deadline) {
		g_main_context_iteration (NULL, FALSE);
		g_usleep (1000);
	}
	g_assert_true (ready);
	NemoPreviewPane *pane = new_test_pane ();
	GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	GtkWidget *entry = gtk_entry_new ();
	gtk_container_add (GTK_CONTAINER (window), box);
	gtk_box_pack_start (GTK_BOX (box), entry, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (box), GTK_WIDGET (pane), TRUE, TRUE, 0);
	gtk_window_set_default_size (GTK_WINDOW (window), 800, 600);
	gtk_widget_show_all (window);
	gtk_widget_grab_focus (entry);
	DocumentResult result = { 0 };
	g_signal_connect (pane->document_viewer, "load-finished", G_CALLBACK (document_finished_cb), &result);
	nemo_preview_pane_set_file (pane, file);
	g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (pane->stack)), ==, "document");
	deadline = g_get_monotonic_time () + 20 * G_TIME_SPAN_SECOND;
	while (!result.finished && g_get_monotonic_time () < deadline) {
		g_main_context_iteration (NULL, FALSE);
		g_usleep (1000);
	}
	g_assert_true (result.finished);
	g_assert_true (gtk_window_get_focus (GTK_WINDOW (window)) == entry);
	if (sandbox) {
		g_assert_no_error (result.error);
		g_assert_cmpuint (nemo_document_viewer_get_page_count (pane->document_viewer), >, 1);
		GdkEventKey page = { .type = GDK_KEY_PRESS, .keyval = GDK_KEY_Page_Down };
		g_assert_true (nemo_preview_pane_handle_key_event (pane, &page));
		g_assert_true (pane->current_file == file);
	} else {
		g_assert_nonnull (result.error);
		g_assert_cmpuint (nemo_document_viewer_get_page_count (pane->document_viewer), ==, 0);
	}
	g_signal_handlers_disconnect_by_data (pane->document_viewer, &result);
	g_clear_error (&result.error);
	nemo_preview_pane_clear (pane);
	g_assert_cmpuint (nemo_document_viewer_get_page_count (pane->document_viewer), ==, 0);
	g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (pane->stack)), ==, "empty");
	gtk_widget_destroy (window);
	g_object_unref (pane);
	nemo_file_unref (file);
	g_object_unref (location);
	g_assert_cmpint (g_remove (path), ==, 0);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (path);
	g_free (directory);
}
#endif

#ifdef HAVE_GSTREAMER
static GThread *test_main_thread;

static void
test_video_sample_ready_cb (GObject *widget, GstSample *sample)
{
	g_assert_true (g_thread_self () == test_main_thread);
	video_sample_ready_cb (widget, sample);
}

static void
test_media_frame_lifetime (void)
{
	NemoPreviewPane *pane;
	NemoPreviewPane *weak_pane;
	GstElement *pipeline, *source, *sink;
	GstCaps *caps;
	GstBuffer *buffer;
	GstFlowReturn flow;
	GstState state;
	gint64 deadline;

	source = gst_element_factory_make ("appsrc", NULL);
	sink = gst_element_factory_make ("appsink", NULL);
	if (source == NULL || sink == NULL) {
		g_clear_object (&source);
		g_clear_object (&sink);
		g_test_skip ("GStreamer appsrc/appsink plugins are unavailable");
		return;
	}

	pane = new_test_pane ();
	weak_pane = pane;
	g_object_add_weak_pointer (G_OBJECT (pane), (gpointer *) &weak_pane);
	pipeline = gst_pipeline_new ("pane-frame-test");
	gst_bin_add_many (GST_BIN (pipeline), source, sink, NULL);
	g_assert_true (gst_element_link (source, sink));
	g_object_set (sink, "sync", FALSE, "async", FALSE, NULL);
	caps = gst_caps_new_simple ("video/x-raw",
				   "format", G_TYPE_STRING, "BGRx",
				   "width", G_TYPE_INT, 2,
				   "height", G_TYPE_INT, 2,
				   "framerate", GST_TYPE_FRACTION, 1, 1,
				   NULL);
	g_object_set (source, "caps", caps, "format", GST_FORMAT_TIME, NULL);
	gst_caps_unref (caps);
	pane->pipeline = gst_object_ref (pipeline);
	test_main_thread = g_thread_self ();
	pane->video_frames = nemo_preview_media_connect_sink (
		sink, G_OBJECT (pane), test_video_sample_ready_cb);
	g_assert_true (request_video_state (pane, GST_STATE_PLAYING));
	buffer = gst_buffer_new_allocate (NULL, 16, NULL);
	gst_buffer_memset (buffer, 0, 0x44, 16);
	GST_BUFFER_PTS (buffer) = 0;
	GST_BUFFER_DURATION (buffer) = GST_SECOND;
	g_signal_emit_by_name (source, "push-buffer", buffer, &flow);
	gst_buffer_unref (buffer);
	g_assert_cmpint (flow, ==, GST_FLOW_OK);

	deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
	while (pane->frame_surface == NULL && g_get_monotonic_time () < deadline) {
		g_main_context_iteration (NULL, FALSE);
		g_usleep (1000);
	}
	g_assert_nonnull (pane->frame_surface);
	g_assert_cmpint (pane->video_width, ==, 2);
	g_assert_cmpint (pane->video_height, ==, 2);

	gtk_widget_destroy (GTK_WIDGET (pane));
	g_assert_null (pane->video_frames);
	g_assert_null (pane->frame_surface);
	g_object_run_dispose (G_OBJECT (pane));
	g_object_unref (pane);
	g_assert_null (weak_pane);

	do {
		g_main_context_iteration (NULL, FALSE);
		gst_element_get_state (pipeline, &state, NULL, 0);
		if (state == GST_STATE_NULL)
			break;
		g_usleep (1000);
	} while (g_get_monotonic_time () < deadline);
	g_assert_cmpint (state, ==, GST_STATE_NULL);
	gst_object_unref (pipeline);
}
#endif

int
main (int argc, char **argv)
{
	char *scratch;
	char *cwd;
	int result;

	if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
		g_printerr ("Run this test through test/run-isolated-regression.py\n");
		return 77;
	}
	g_setenv ("GDK_BACKEND", "x11", TRUE);
	g_unsetenv ("WAYLAND_DISPLAY");
	g_unsetenv ("AT_SPI_BUS_ADDRESS");
	g_unsetenv ("DBUS_STARTER_ADDRESS");
	g_setenv ("GIO_USE_VFS", "local", TRUE);
	g_setenv ("NO_AT_BRIDGE", "1", TRUE);
	g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
	gtk_test_init (&argc, &argv, NULL);
	g_assert_cmpstr (gdk_display_get_name (gdk_display_get_default ()), ==, g_getenv ("DISPLAY"));
	g_test_message ("GTK display: %s", gdk_display_get_name (gdk_display_get_default ()));
#ifdef HAVE_GSTREAMER
	gst_init (&argc, &argv);
#endif
	cwd = g_get_current_dir ();
	scratch = g_strdup_printf ("pane-test-data-%u", (guint) getpid ());
	g_assert_cmpint (g_mkdir (scratch, 0700), ==, 0);
	g_assert_cmpint (g_chdir (scratch), ==, 0);
	g_test_add_func ("/preview-pane/stale-thumbnail", test_stale_thumbnail);
	g_test_add_func ("/preview-pane/generation", test_generation_guard);
	g_test_add_func ("/preview-pane/destroy-pending", test_destroy_pending);
	g_test_add_func ("/preview-pane/thumbnail-worker", test_thumbnail_worker);
	g_test_add_func ("/preview-pane/details-ownership", test_details_ownership);
#ifdef HAVE_DOCUMENT_PREVIEW
	g_test_add_func ("/preview-pane/rich-document", test_rich_document_pane);
#endif
#ifdef HAVE_GSTREAMER
	g_test_add_func ("/preview-pane/media-frame-lifetime", test_media_frame_lifetime);
#endif
	result = g_test_run ();
	g_assert_cmpint (g_chdir (cwd), ==, 0);
	g_assert_cmpint (g_rmdir (scratch), ==, 0);
	g_free (scratch);
	g_free (cwd);
	return result;
}
