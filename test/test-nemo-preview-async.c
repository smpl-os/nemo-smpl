/* Isolated GTK regressions: no Nemo application, desktop, or device access.
 * Include the widgets so delayed backend completions can be injected without
 * adding fault switches or test-only APIs to production code. */
#include <config.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#endif
#ifdef HAVE_LIBRAW
#include <libraw/libraw.h>
#endif

static GThread *main_thread;
static GMutex delay_lock;
static GCond delay_cond;
static gboolean release_io;
static gint delayed_calls;
static gint finished_calls;
static gint delay_open;
static gint delay_info;
static gint delay_directory;
static gint delay_state;
static gint fail_state;
static gint fail_preview_thread;
static gint fail_media_push;
static guint heartbeats;

static GThread *
test_thread_try_new (const char *name, GThreadFunc function, gpointer data, GError **error)
{
	if (g_atomic_int_compare_and_exchange (&fail_preview_thread, 1, 0)) {
		g_set_error_literal (error, G_THREAD_ERROR, G_THREAD_ERROR_AGAIN, "Test thread unavailable");
		return NULL;
	}
	return g_thread_try_new (name, function, data, error);
}

static gboolean
test_pool_push (GThreadPool *pool, gpointer data, GError **error)
{
	gboolean queued = g_thread_pool_push (pool, data, error);
	if (queued && g_atomic_int_compare_and_exchange (&fail_media_push, 1, 0)) {
		/* GLib queues the item even when a thread-creation error is returned. */
		g_set_error_literal (error, G_THREAD_ERROR, G_THREAD_ERROR_AGAIN, "Test worker unavailable");
		return FALSE;
	}
	return queued;
}

static void
hold_backend (void)
{
	g_assert_true (g_thread_self () != main_thread);
	g_mutex_lock (&delay_lock);
	g_atomic_int_inc (&delayed_calls);
	while (!release_io)
		g_cond_wait (&delay_cond, &delay_lock);
	g_atomic_int_inc (&finished_calls);
	g_mutex_unlock (&delay_lock);
}

static GFileInputStream *
test_read (GFile *file, GCancellable *cancel, GError **error)
{
	if (g_atomic_int_get (&delay_open)) {
		char *name = g_file_get_basename (file);
		if (g_str_has_prefix (name, "slow"))
			hold_backend ();
		g_free (name);
	}
	/* Deliberately model a backend which completes despite cancellation. */
	return g_file_read (file, NULL, error);
}

static GFileInfo *
test_query_info (GFile *file, const char *attributes, GFileQueryInfoFlags flags,
		 GCancellable *cancel, GError **error)
{
	char *name = g_file_get_basename (file);
	if (g_atomic_int_get (&delay_info) && g_str_has_prefix (name, "slow"))
		hold_backend ();
	g_free (name);
	return g_file_query_info (file, attributes, flags, NULL, error);
}

static GFileEnumerator *
test_enumerate (GFile *file, const char *attributes, GFileQueryInfoFlags flags,
		GCancellable *cancel, GError **error)
{
	char *name = g_file_get_basename (file);
	if (g_atomic_int_get (&delay_directory) && g_str_has_prefix (name, "slow"))
		hold_backend ();
	g_free (name);
	return g_file_enumerate_children (file, attributes, flags, NULL, error);
}

#ifdef HAVE_GSTREAMER
static GstStateChangeReturn
test_set_state (GstElement *pipeline, GstState state)
{
	g_assert_true (g_thread_self () != main_thread);
	if (g_atomic_int_get (&delay_state) && state == GST_STATE_PAUSED)
		hold_backend ();
	if (g_atomic_int_get (&fail_state) && state == GST_STATE_PAUSED)
		return GST_STATE_CHANGE_FAILURE;
	return gst_element_set_state (pipeline, state);
}

static gboolean
test_query_position (GstElement *pipeline, GstFormat format, gint64 *position)
{
	g_assert_false (g_atomic_int_get (&delay_state));
	return gst_element_query_position (pipeline, format, position);
}
#endif

#ifdef HAVE_LIBRAW
static int
test_raw_open (libraw_data_t *raw, const char *path)
{
	g_assert_true (g_thread_self () != main_thread);
	return libraw_open_file (raw, path);
}
#endif

#define g_file_read test_read
#ifdef HAVE_LIBRAW
#define libraw_open_file test_raw_open
#endif
#include "../src/nemo-image-viewer.c"
#undef libraw_open_file
#undef g_file_read

#define g_file_query_info test_query_info
#define g_file_enumerate_children test_enumerate
#define gst_element_query_position test_query_position
#include "../src/nemo-quick-preview.c"
#undef gst_element_query_position
#undef g_file_enumerate_children
#undef g_file_query_info

#define gst_element_set_state test_set_state
#define g_thread_try_new test_thread_try_new
#define g_thread_pool_push test_pool_push
#include "../src/nemo-preview-utils.c"
#undef g_thread_pool_push
#undef g_thread_try_new
#undef gst_element_set_state

/* The directory analyzer's desktop navigation callbacks are not exercised. */
#include "../src/nemo-window-slot.h"
GSettings *nemo_keybinding_settings;
GType nemo_window_get_type (void) { return GTK_TYPE_WINDOW; }
NemoWindowSlot *nemo_window_get_active_slot (NemoWindow *window)
{
	g_assert_not_reached ();
}
void nemo_window_slot_open_location_full (NemoWindowSlot *slot, GFile *location,
	NemoWindowOpenFlags flags, GList *selection, NemoWindowGoToCallback callback,
	gpointer data)
{
	g_assert_not_reached ();
}

static gboolean
heartbeat (gpointer data)
{
	heartbeats++;
	return G_SOURCE_CONTINUE;
}

static void
iterate_for (guint milliseconds)
{
	gint64 end = g_get_monotonic_time () + milliseconds * 1000;
	while (g_get_monotonic_time () < end) {
		while (g_main_context_iteration (NULL, FALSE));
		g_usleep (1000);
	}
}

#define WAIT_UNTIL(condition) G_STMT_START { \
	gint64 deadline = g_get_monotonic_time () + 5000000; \
	while (!(condition) && g_get_monotonic_time () < deadline) \
		iterate_for (5); \
	g_assert_true (condition); \
} G_STMT_END

static void
reset_delay (void)
{
	g_mutex_lock (&delay_lock);
	release_io = FALSE;
	delayed_calls = finished_calls = 0;
	g_mutex_unlock (&delay_lock);
}

static void
release_backend (void)
{
	g_mutex_lock (&delay_lock);
	release_io = TRUE;
	g_cond_broadcast (&delay_cond);
	g_mutex_unlock (&delay_lock);
}

static void
assert_responsive (void)
{
	guint before = heartbeats;
	iterate_for (80);
	g_assert_cmpuint (heartbeats, >=, before + 4);
}

static GFile *
write_image (const char *directory, const char *name, guint32 colour)
{
	char *path = g_build_filename (directory, name, NULL);
	GdkPixbuf *pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 4, 4);
	GError *error = NULL;
	GFile *file = g_file_new_for_path (path);
	gdk_pixbuf_fill (pixbuf, colour);
	g_assert_true (gdk_pixbuf_save (pixbuf, path, "png", &error, NULL));
	g_assert_no_error (error);
	g_object_unref (pixbuf);
	g_free (path);
	return file;
}

static void
remove_fixture (GFile *file)
{
	GError *error = NULL;
	g_assert_true (g_file_delete (file, NULL, &error));
	g_assert_no_error (error);
	g_object_unref (file);
}

static void
test_image_switch_and_destroy (void)
{
	char *directory = g_dir_make_tmp ("nemo-image-async-XXXXXX", NULL);
	GFile *slow = write_image (directory, "slow.png", 0xff0000ff);
	GFile *fast = write_image (directory, "fast.png", 0x0000ffff);
	NemoImageViewer *viewer = nemo_image_viewer_new ();
	gpointer weak;
	g_object_ref_sink (viewer);
	reset_delay ();
	g_atomic_int_set (&delay_open, TRUE);
	nemo_image_viewer_load_location (viewer, slow);
	WAIT_UNTIL (g_atomic_int_get (&delayed_calls) == 1);
	assert_responsive ();
	nemo_image_viewer_load_location (viewer, fast);
	WAIT_UNTIL (viewer->original_pixbuf != NULL);
	g_assert_cmpuint (gdk_pixbuf_get_pixels (viewer->original_pixbuf)[2], ==, 255);
	release_backend ();
	WAIT_UNTIL (g_atomic_int_get (&finished_calls) == 1);
	iterate_for (100);
	g_assert_cmpuint (gdk_pixbuf_get_pixels (viewer->original_pixbuf)[2], ==, 255);

	reset_delay ();
	nemo_image_viewer_load_location (viewer, slow);
	WAIT_UNTIL (g_atomic_int_get (&delayed_calls) == 1);
	weak = viewer;
	g_object_add_weak_pointer (G_OBJECT (viewer), &weak);
	gtk_widget_destroy (GTK_WIDGET (viewer));
	g_object_unref (viewer);
	g_assert_null (weak);
	assert_responsive ();
	release_backend ();
	WAIT_UNTIL (g_atomic_int_get (&finished_calls) == 1);
	iterate_for (100);
	g_atomic_int_set (&delay_open, FALSE);
	remove_fixture (slow);
	remove_fixture (fast);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (directory);
}

static gboolean
preview_workers_idle (void)
{
	gboolean idle;
	g_mutex_lock (&preview_work_lock);
	idle = preview_active_workers == 0 && g_queue_is_empty (&preview_pending_work);
	g_mutex_unlock (&preview_work_lock);
	return idle;
}

static void
test_rapid_selection_worker_bound (void)
{
	char *directory = g_dir_make_tmp ("nemo-bounded-async-XXXXXX", NULL);
	GFile *slow = write_image (directory, "slow.png", 0xff0000ff);
	GFile *fast = write_image (directory, "fast.png", 0x0000ffff);
	NemoImageViewer *old = nemo_image_viewer_new ();
	NemoImageViewer *latest = nemo_image_viewer_new ();
	gpointer weak = old;

	g_object_ref_sink (old);
	g_object_ref_sink (latest);
	g_object_add_weak_pointer (G_OBJECT (old), &weak);
	WAIT_UNTIL (preview_workers_idle ());
	reset_delay ();
	g_atomic_int_set (&delay_open, TRUE);
	for (guint i = 0; i < 4; i++) {
		nemo_image_viewer_load_location (old, slow);
		WAIT_UNTIL (g_atomic_int_get (&delayed_calls) == (gint) i + 1);
	}
	for (guint i = 0; i < 100; i++)
		nemo_image_viewer_load_location (old, slow);
	g_mutex_lock (&preview_work_lock);
	g_assert_cmpuint (preview_active_workers, ==, 4);
	g_assert_cmpuint (g_queue_get_length (&preview_pending_work), ==, 1);
	g_mutex_unlock (&preview_work_lock);

	gtk_widget_destroy (GTK_WIDGET (old));
	g_object_unref (old);
	g_assert_null (weak);
	nemo_image_viewer_load_location (latest, fast);
	g_mutex_lock (&preview_work_lock);
	g_assert_cmpuint (g_queue_get_length (&preview_pending_work), ==, 1);
	g_mutex_unlock (&preview_work_lock);
	assert_responsive ();
	g_assert_cmpint (g_atomic_int_get (&delayed_calls), ==, 4);
	release_backend ();
	WAIT_UNTIL (latest->original_pixbuf != NULL && preview_workers_idle ());
	g_assert_cmpuint (gdk_pixbuf_get_pixels (latest->original_pixbuf)[2], ==, 255);
	g_assert_cmpint (g_atomic_int_get (&finished_calls), ==, 4);
	g_atomic_int_set (&delay_open, FALSE);
	gtk_widget_destroy (GTK_WIDGET (latest));
	g_object_unref (latest);
	remove_fixture (slow);
	remove_fixture (fast);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (directory);
}

static void
test_worker_creation_failure (void)
{
	char *directory = g_dir_make_tmp ("nemo-worker-failure-XXXXXX", NULL);
	GFile *file = write_image (directory, "fast.png", 0x0000ffff);
	NemoImageViewer *viewer = nemo_image_viewer_new ();
	g_object_ref_sink (viewer);
	WAIT_UNTIL (preview_workers_idle ());
	g_atomic_int_set (&fail_preview_thread, 1);
	nemo_image_viewer_load_location (viewer, file);
	WAIT_UNTIL (g_strcmp0 (gtk_label_get_text (GTK_LABEL (viewer->status_label)),
			      "Test thread unavailable") == 0);
	g_assert_true (preview_workers_idle ());
	nemo_image_viewer_load_location (viewer, file);
	WAIT_UNTIL (viewer->original_pixbuf != NULL);
	gtk_widget_destroy (GTK_WIDGET (viewer));
	g_object_unref (viewer);
	remove_fixture (file);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (directory);
}

static void
test_quick_navigation_races (void)
{
	char *directory = g_dir_make_tmp ("nemo-quick-async-XXXXXX", NULL);
	char *slow_directory = g_build_filename (directory, "slow-dir", NULL);
	GFile *slow, *fast, *other;
	NemoQuickPreview *preview;
	gpointer weak;

	g_assert_cmpint (g_mkdir (slow_directory, 0700), ==, 0);
	slow = write_image (slow_directory, "slow.png", 0xff0000ff);
	fast = write_image (directory, "fast.png", 0x0000ffff);
	other = write_image (directory, "other.png", 0x00ff00ff);
	preview = g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL);
	g_object_ref_sink (preview);
	reset_delay ();
	g_atomic_int_set (&delay_info, TRUE);
	g_atomic_int_set (&delay_directory, TRUE);
	nemo_quick_preview_show_file (preview, slow, NULL);
	g_assert_true (gtk_widget_get_visible (GTK_WIDGET (preview)));
	WAIT_UNTIL (g_atomic_int_get (&delayed_calls) == 2);
	assert_responsive ();
	nemo_quick_preview_show_file (preview, fast, NULL);
	WAIT_UNTIL (preview->mode == PREVIEW_IMAGE && preview->dir_files != NULL &&
		    preview->image_viewer->original_pixbuf != NULL);
	g_assert_cmpstr (gtk_header_bar_get_title (GTK_HEADER_BAR (preview->header_bar)), ==, "fast.png");
	release_backend ();
	WAIT_UNTIL (g_atomic_int_get (&finished_calls) == 2);
	iterate_for (100);
	g_assert_true (g_file_equal (preview->current_file, fast));
	g_assert_cmpstr (gtk_header_bar_get_title (GTK_HEADER_BAR (preview->header_bar)), ==, "fast.png");
	navigate_to_offset (preview, 1);
	WAIT_UNTIL (g_strcmp0 (gtk_header_bar_get_title (GTK_HEADER_BAR (preview->header_bar)), "other.png") == 0);
	navigate_to_offset (preview, -1);
	WAIT_UNTIL (g_strcmp0 (gtk_header_bar_get_title (GTK_HEADER_BAR (preview->header_bar)), "fast.png") == 0);

	reset_delay ();
	nemo_quick_preview_show_file (preview, slow, NULL);
	WAIT_UNTIL (g_atomic_int_get (&delayed_calls) == 2);
	nemo_quick_preview_dismiss (preview);
	g_assert_false (gtk_widget_get_visible (GTK_WIDGET (preview)));
	weak = preview;
	g_object_add_weak_pointer (G_OBJECT (preview), &weak);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	g_assert_null (weak);
	assert_responsive ();
	release_backend ();
	WAIT_UNTIL (g_atomic_int_get (&finished_calls) == 2);
	iterate_for (100);
	g_atomic_int_set (&delay_info, FALSE);
	g_atomic_int_set (&delay_directory, FALSE);
	remove_fixture (slow);
	remove_fixture (fast);
	remove_fixture (other);
	g_assert_cmpint (g_rmdir (slow_directory), ==, 0);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (slow_directory);
	g_free (directory);
}

static void
test_image_error_and_raw_worker (void)
{
	NemoImageViewer *viewer = nemo_image_viewer_new ();
	char *directory = g_dir_make_tmp ("nemo-raw-async-XXXXXX", NULL);
	char *path = g_build_filename (directory, "broken.dng", NULL);
	GFile *file = g_file_new_for_path (path);
	g_object_ref_sink (viewer);
	g_assert_true (g_file_set_contents (path, "not a raw image", -1, NULL));
	nemo_image_viewer_load_location (viewer, file);
	WAIT_UNTIL (g_strcmp0 (gtk_label_get_text (GTK_LABEL (viewer->status_label)), "Loading...") != 0);
	g_assert_true (gtk_widget_get_visible (viewer->status_label));
	g_assert_null (viewer->original_pixbuf);
	gtk_widget_destroy (GTK_WIDGET (viewer));
	g_object_unref (viewer);
	remove_fixture (file);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (path);
	g_free (directory);
}

static void
test_quick_text_search_and_modes (void)
{
	char *directory = g_dir_make_tmp ("nemo-text-async-XXXXXX", NULL);
	char *path = g_build_filename (directory, "example.txt", NULL);
	GFile *file = g_file_new_for_path (path);
	GFile *folder = g_file_new_for_path (directory);
	NemoQuickPreview *preview = g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL);

	g_object_ref_sink (preview);
	g_assert_true (g_file_set_contents (path, "first line\nneedle here\nlast line\n", -1, NULL));
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->mode == PREVIEW_TEXT);
	gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (preview->search_bar), TRUE);
	gtk_entry_set_text (GTK_ENTRY (preview->search_entry), "needle");
	on_search_changed (GTK_SEARCH_ENTRY (preview->search_entry), preview);
	WAIT_UNTIL (!nemo_paged_viewer_search_is_pending (preview->paged_viewer) &&
		    nemo_paged_viewer_search_has_match (preview->paged_viewer));
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (preview->search_match_label)), ==, "");
	gtk_entry_set_text (GTK_ENTRY (preview->search_entry), "absent");
	on_search_changed (GTK_SEARCH_ENTRY (preview->search_entry), preview);
	WAIT_UNTIL (!nemo_paged_viewer_search_is_pending (preview->paged_viewer));
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (preview->search_match_label)), ==, "No matches");
	preview_show_paged (preview, file, NEMO_VIEWER_MODE_HEX);
	g_assert_cmpint (preview->mode, ==, PREVIEW_HEX);
	g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (preview->stack)), ==, "paged");

	nemo_quick_preview_show_file (preview, folder, NULL);
	WAIT_UNTIL (preview->mode == PREVIEW_DIR);
	WAIT_UNTIL (nemo_dir_analyzer_get_entries (preview->dir_analyzer) != NULL);
	nemo_quick_preview_dismiss (preview);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	iterate_for (100);
	remove_fixture (file);
	remove_fixture (folder);
	g_free (path);
	g_free (directory);
}

#ifdef HAVE_GSTREAMER
static void
test_bounded_media_stop (void)
{
	GstElement *pipelines[5];
	reset_delay ();
	g_atomic_int_set (&delay_state, TRUE);
	for (guint i = 0; i < 5; i++) {
		pipelines[i] = gst_pipeline_new (NULL);
		if (i < 4)
			g_assert_true (nemo_preview_media_set_state_async (pipelines[i], GST_STATE_PAUSED));
		else
			g_assert_false (nemo_preview_media_set_state_async (pipelines[i], GST_STATE_PAUSED));
	}
	WAIT_UNTIL (g_atomic_int_get (&delayed_calls) == 2);
	assert_responsive ();
	for (guint i = 0; i < 5; i++) {
		g_assert_true (nemo_preview_media_set_state_async (pipelines[i], GST_STATE_NULL));
		gst_object_unref (pipelines[i]);
	}
	assert_responsive ();
	g_assert_cmpint (g_atomic_int_get (&delayed_calls), ==, 2);
	release_backend ();
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	g_assert_cmpint (g_atomic_int_get (&delayed_calls), ==, 2);
	g_atomic_int_set (&delay_state, FALSE);
}

static guint frames_seen;

static void
test_quick_close_while_media_loading (void)
{
	char *directory = g_dir_make_tmp ("nemo-media-async-XXXXXX", NULL);
	char *path = g_build_filename (directory, "missing-video.avi", NULL);
	GFile *file = g_file_new_for_path (path);
	NemoQuickPreview *preview = g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL);
	gpointer weak = preview;

	g_object_ref_sink (preview);
	g_object_add_weak_pointer (G_OBJECT (preview), &weak);
	reset_delay ();
	g_atomic_int_set (&delay_state, TRUE);
	preview_show_media (preview, file);
	WAIT_UNTIL (g_atomic_int_get (&delayed_calls) == 1);
	GdkEventKey key = { .type = GDK_KEY_PRESS, .keyval = GDK_KEY_Right };
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	key.keyval = GDK_KEY_period;
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	seek_position_update_cb (preview);
	seek_release_cb (preview->seek_scale, NULL, preview);
	preview_show_media (preview, file);
	WAIT_UNTIL (g_atomic_int_get (&delayed_calls) == 2);
	for (guint i = 0; i < 20; i++)
		preview_show_media (preview, file);
	g_assert_cmpint (g_atomic_int_get (&media_pipeline_count), <=, 4);
	nemo_quick_preview_dismiss (preview);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	g_assert_null (weak);
	assert_responsive ();
	release_backend ();
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	g_atomic_int_set (&delay_state, FALSE);
	g_object_unref (file);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (path);
	g_free (directory);
}

static void
test_media_start_error (void)
{
	NemoQuickPreview *preview = g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL);
	GFile *file = g_file_new_for_uri ("test-preview:///video.avi");
	g_object_ref_sink (preview);
	g_atomic_int_set (&fail_state, TRUE);
	preview_show_media (preview, file);
	WAIT_UNTIL (g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (preview->stack)), "status") == 0);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (preview->status_label)),
			==, "Unable to start media preview.");
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	g_atomic_int_set (&fail_state, FALSE);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	g_object_unref (file);
}

static void
test_media_push_failure (void)
{
	GstElement *pipeline = gst_pipeline_new (NULL);
	g_atomic_int_set (&fail_media_push, 1);
	g_assert_false (nemo_preview_media_set_state_async (pipeline, GST_STATE_PAUSED));
	g_assert_true (nemo_preview_media_set_state_async (pipeline, GST_STATE_NULL));
	gst_object_unref (pipeline);
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	iterate_for (50);
	g_assert_cmpint (g_atomic_int_get (&media_pipeline_count), ==, 0);
}

static void
consume_test_frame (GObject *widget, GstSample *sample)
{
	g_assert_true (g_thread_self () == main_thread);
	frames_seen++;
	process_video_sample (NEMO_QUICK_PREVIEW (widget), sample);
}

static void
test_media_frame_lifetime (void)
{
	NemoQuickPreview *preview = g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL);
	GError *error = NULL;
	GstElement *pipeline = gst_parse_launch (
		"videotestsrc num-buffers=10 ! video/x-raw,format=BGRx,width=32,height=32 ! appsink name=output",
		&error);
	GstElement *sink;
	NemoPreviewMediaFrames *frames;
	g_object_ref_sink (preview);
	g_assert_no_error (error);
	g_assert_nonnull (pipeline);
	sink = gst_bin_get_by_name (GST_BIN (pipeline), "output");
	frames = nemo_preview_media_connect_sink (sink, G_OBJECT (preview), consume_test_frame);
	gst_object_unref (sink);
	frames_seen = 0;
	g_assert_true (nemo_preview_media_set_state_async (pipeline, GST_STATE_PAUSED));
	WAIT_UNTIL (frames_seen > 0);
	g_assert_nonnull (preview->frame_surface);
	nemo_preview_media_frames_stop (frames);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	g_assert_true (nemo_preview_media_set_state_async (pipeline, GST_STATE_NULL));
	gst_object_unref (pipeline);
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	iterate_for (100);
}
#endif

int
main (int argc, char **argv)
{
	if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
		g_printerr ("Run this test through test/run-isolated-regression.py\n");
		return 77;
	}
	g_setenv ("GDK_BACKEND", "x11", TRUE);
	g_unsetenv ("WAYLAND_DISPLAY");
	g_unsetenv ("AT_SPI_BUS_ADDRESS");
	g_unsetenv ("DBUS_STARTER_ADDRESS");
	g_setenv ("GIO_USE_VFS", "local", TRUE);
	g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
	g_setenv ("NO_AT_BRIDGE", "1", TRUE);
	gtk_test_init (&argc, &argv, NULL);
	g_assert_cmpstr (gdk_display_get_name (gdk_display_get_default ()), ==, g_getenv ("DISPLAY"));
	g_test_message ("GTK display: %s", gdk_display_get_name (gdk_display_get_default ()));
#ifdef HAVE_GSTREAMER
	gst_init (&argc, &argv);
#endif
	main_thread = g_thread_self ();
	guint tick = g_timeout_add (10, heartbeat, NULL);
	g_test_add_func ("/preview/image/switch-destroy-delayed-open", test_image_switch_and_destroy);
	g_test_add_func ("/preview/image/error-raw-worker", test_image_error_and_raw_worker);
	g_test_add_func ("/preview/image/rapid-selection-worker-bound", test_rapid_selection_worker_bound);
	g_test_add_func ("/preview/image/worker-creation-failure", test_worker_creation_failure);
	g_test_add_func ("/preview/quick/metadata-navigation-races", test_quick_navigation_races);
	g_test_add_func ("/preview/quick/text-search-hex-folder", test_quick_text_search_and_modes);
#ifdef HAVE_GSTREAMER
	g_test_add_func ("/preview/media/bounded-delayed-stop", test_bounded_media_stop);
	g_test_add_func ("/preview/media/quick-close-loading", test_quick_close_while_media_loading);
	g_test_add_func ("/preview/media/start-error", test_media_start_error);
	g_test_add_func ("/preview/media/push-failure", test_media_push_failure);
	g_test_add_func ("/preview/media/frame-lifetime", test_media_frame_lifetime);
#endif
	int result = g_test_run ();
	g_source_remove (tick);
	return result;
}
