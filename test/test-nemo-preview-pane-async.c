/* Run in an isolated D-Bus/Xvfb session with private HOME/XDG directories.
 * Include the implementation to exercise completion ordering without exposing
 * worker and generation internals as public widget API. */
#include <glib/gstdio.h>
#include <unistd.h>
#include "../src/nemo-preview-pane.c"

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

#ifdef HAVE_EXIF
typedef struct {
	Completion completion;
	GpsData *gps;
} MetadataResult;

static void
metadata_fixture_ready_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	MetadataResult *metadata = user_data;
	GError *error = NULL;

	metadata->gps = g_task_propagate_pointer (G_TASK (result), &error);
	g_assert_no_error (error);
	metadata->completion.done = TRUE;
}

static void
add_gps_entry (ExifData *exif, ExifTag tag, ExifFormat format,
	       const guchar *bytes, guint size, guint components)
{
	ExifEntry *entry = exif_entry_new ();

	entry->tag = tag;
	entry->format = format;
	entry->components = components;
	entry->size = size;
	entry->data = g_malloc (size);
	memcpy (entry->data, bytes, size);
	exif_content_add_entry (exif->ifd[EXIF_IFD_GPS], entry);
	exif_entry_unref (entry);
}

static void
test_metadata_worker (void)
{
	NemoPreviewPane *pane = new_test_pane ();
	GFile *file = g_file_new_for_path ("pane-test-gps.jpg");
	MetadataResult metadata = { { NULL, FALSE }, NULL };
	GTask *task = g_task_new (NULL, pane->cancellable,
				 metadata_fixture_ready_cb, &metadata);
	ExifData *exif = exif_data_new ();
	guchar values[24], header[6] = { 0xff, 0xd8, 0xff, 0xe1, 0, 0 };
	const guchar end[2] = { 0xff, 0xd9 };
	ExifRational value = { 40, 1 };
	guchar *bytes = NULL;
	guint length = 0;
	GByteArray *jpeg = g_byte_array_new ();
	GError *error = NULL;

	exif_data_set_byte_order (exif, EXIF_BYTE_ORDER_INTEL);
	exif_set_rational (values, EXIF_BYTE_ORDER_INTEL, value);
	value.numerator = 30;
	exif_set_rational (values + 8, EXIF_BYTE_ORDER_INTEL, value);
	value.numerator = 0;
	exif_set_rational (values + 16, EXIF_BYTE_ORDER_INTEL, value);
	add_gps_entry (exif, 0x0002, EXIF_FORMAT_RATIONAL, values, sizeof values, 3);
	value.numerator = 70;
	exif_set_rational (values, EXIF_BYTE_ORDER_INTEL, value);
	value.numerator = 15;
	exif_set_rational (values + 8, EXIF_BYTE_ORDER_INTEL, value);
	add_gps_entry (exif, 0x0004, EXIF_FORMAT_RATIONAL, values, sizeof values, 3);
	add_gps_entry (exif, 0x0001, EXIF_FORMAT_ASCII, (const guchar *) "N", 2, 2);
	add_gps_entry (exif, 0x0003, EXIF_FORMAT_ASCII, (const guchar *) "W", 2, 2);
	exif_data_save_data (exif, &bytes, &length);
	g_assert_cmpuint (length, >, 0);
	g_assert_cmpuint (length, <, 65534);
	header[4] = (length + 2) >> 8;
	header[5] = (length + 2) & 0xff;
	g_byte_array_append (jpeg, header, sizeof header);
	g_byte_array_append (jpeg, bytes, length);
	g_byte_array_append (jpeg, end, sizeof end);
	g_assert_true (g_file_set_contents ("pane-test-gps.jpg",
					   (const char *) jpeg->data, jpeg->len, &error));
	g_assert_no_error (error);
	g_free (bytes);
	exif_data_unref (exif);
	g_byte_array_unref (jpeg);

	g_task_set_task_data (task, pane_load_data_new (pane, file),
			     (GDestroyNotify) pane_load_data_free);
	nemo_preview_run_task (task, gps_metadata_worker);
	wait_for_completion (&metadata.completion);
	g_assert_nonnull (metadata.gps);
	g_assert_cmpfloat (metadata.gps->latitude, ==, 40.5);
	g_assert_cmpfloat (metadata.gps->longitude, ==, -70.25);
	g_free (metadata.gps);
	g_assert_cmpint (g_remove ("pane-test-gps.jpg"), ==, 0);
	g_object_unref (task);
	g_object_unref (file);
	gtk_widget_destroy (GTK_WIDGET (pane));
	g_object_unref (pane);
}

static void
test_cached_map_worker (void)
{
	NemoPreviewPane *pane = new_test_pane ();
	GFile *file = g_file_new_for_path ("nonexistent-map-download.png");
	GdkPixbuf *pixbuf = new_test_pixbuf ();
	GError *error = NULL;
	Completion completion = { map_tile_ready_cb, FALSE };
	GTask *task = new_test_task (pane, file, &completion);
	PaneLoadData *data = g_task_get_task_data (task);

	data->cache_path = g_strdup ("pane-test-map.png");
	data->pixel_x = data->pixel_y = 128;
	g_assert_true (gdk_pixbuf_save (pixbuf, data->cache_path, "png", &error, NULL));
	g_assert_no_error (error);
	g_object_unref (pixbuf);
	nemo_preview_run_task (task, map_tile_worker);
	wait_for_completion (&completion);
	pixbuf = gtk_image_get_pixbuf (GTK_IMAGE (pane->detail_gps_map));
	g_assert_nonnull (pixbuf);
	g_assert_cmpint (gdk_pixbuf_get_width (pixbuf), ==, GPS_MAP_SIZE);
	g_assert_true (gtk_widget_get_visible (pane->gps_map_event_box));

	g_assert_cmpint (g_remove (data->cache_path), ==, 0);
	g_object_unref (task);
	g_object_unref (file);
	gtk_widget_destroy (GTK_WIDGET (pane));
	g_object_unref (pane);
}

static void
test_stale_metadata_and_map (void)
{
	NemoPreviewPane *pane = new_test_pane ();
	GFile *file = g_file_new_for_path ("unused-preview-gps");
	Completion metadata = { gps_metadata_ready_cb, FALSE };
	Completion map = { map_tile_ready_cb, FALSE };
	GTask *metadata_task = new_test_task (pane, file, &metadata);
	GTask *map_task = new_test_task (pane, file, &map);
	GpsData *gps = g_new0 (GpsData, 1);

	gps->latitude = 40;
	gps->longitude = -70;
	g_strlcpy (gps->label, "obsolete GPS", sizeof gps->label);
	g_task_return_pointer (metadata_task, gps, g_free);
	g_task_return_pointer (map_task, new_test_pixbuf (), g_object_unref);
	nemo_preview_pane_clear (pane);
	wait_for_completion (&metadata);
	wait_for_completion (&map);
	g_assert_false (gtk_widget_get_visible (pane->detail_gps));
	g_assert_false (gtk_widget_get_visible (pane->gps_map_event_box));
	g_assert_cmpint (gtk_image_get_storage_type (GTK_IMAGE (pane->detail_gps_map)),
			 ==, GTK_IMAGE_EMPTY);

	g_object_unref (metadata_task);
	g_object_unref (map_task);
	g_object_unref (file);
	gtk_widget_destroy (GTK_WIDGET (pane));
	g_object_unref (pane);
}

static void
test_polar_coordinates (void)
{
	int x, y;
	double px, py;

	gps_to_tile (90, 180, GPS_MAP_ZOOM, &x, &y, &px, &py);
	g_assert_cmpint (x, >=, 0);
	g_assert_cmpint (x, <, 1 << GPS_MAP_ZOOM);
	g_assert_cmpint (y, >=, 0);
	g_assert_cmpint (y, <, 1 << GPS_MAP_ZOOM);
	g_assert_true (isfinite (px));
	g_assert_true (isfinite (py));
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
#ifdef HAVE_EXIF
	g_test_add_func ("/preview-pane/metadata-worker", test_metadata_worker);
	g_test_add_func ("/preview-pane/cached-map-worker", test_cached_map_worker);
	g_test_add_func ("/preview-pane/stale-metadata-and-map", test_stale_metadata_and_map);
	g_test_add_func ("/preview-pane/polar-coordinates", test_polar_coordinates);
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
