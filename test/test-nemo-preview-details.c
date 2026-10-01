/* Synthetic metadata and cached tiles only; never contact a map service.
 * Including the implementation makes completion ordering deterministic. */
#include <glib/gstdio.h>
#include <unistd.h>
#include "../src/nemo-preview-details.c"
#include <libnemo-private/nemo-file-private.h>

typedef struct {
	GAsyncReadyCallback callback;
	gboolean done;
	DetailsResult *metadata;
} Completion;

static NemoPreviewDetails *
new_details (void)
{
	NemoPreviewDetails *self = g_object_ref_sink (nemo_preview_details_new ());
	self->cancellable = g_cancellable_new ();
	return self;
}

static void
complete_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	Completion *completion = user_data;
	if (completion->callback != NULL)
		completion->callback (source, result, NULL);
	else {
		GError *error = NULL;
		completion->metadata = g_task_propagate_pointer (G_TASK (result), &error);
		g_assert_no_error (error);
	}
	completion->done = TRUE;
}

static GTask *
new_task (NemoPreviewDetails *self, const char *path, Completion *completion)
{
	GFile *file = g_file_new_for_path (path);
	GTask *task = g_task_new (NULL, self->cancellable, complete_cb, completion);
	g_task_set_task_data (task, details_load_data_new (self, file),
			     (GDestroyNotify) details_load_data_free);
	g_object_unref (file);
	return task;
}

static void
wait_done (Completion *completion)
{
	gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
	while (!completion->done && g_get_monotonic_time () < deadline) {
		g_main_context_iteration (NULL, FALSE);
		g_usleep (1000);
	}
	g_assert_true (completion->done);
}

static void
wait_metadata (NemoPreviewDetails *self)
{
	gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
	while (g_strcmp0 (gtk_label_get_text (GTK_LABEL (self->status)),
			 _("Loading information…")) == 0 &&
	       gtk_widget_get_visible (self->status) &&
	       g_get_monotonic_time () < deadline) {
		g_main_context_iteration (NULL, FALSE);
		g_usleep (1000);
	}
	g_assert_false (gtk_widget_get_visible (self->status));
}

static void
destroy_details (NemoPreviewDetails *self)
{
	gtk_widget_destroy (GTK_WIDGET (self));
	g_object_unref (self);
}

static GdkPixbuf *
new_tile (void)
{
	GdkPixbuf *pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 256, 256);
	gdk_pixbuf_fill (pixbuf, 0x428342ff);
	return pixbuf;
}

static void
test_parent_entry (void)
{
	NemoPreviewDetails *self = new_details ();
	NemoFile *parent = nemo_file_new_parent_entry ();
	GFile *location = nemo_file_get_location (parent);
	nemo_preview_details_set_file (self, location);
	g_assert_null (self->file);
	g_assert_null (self->cancellable);
	g_assert_null (self->size_file);
	g_assert_cmpuint (self->size_timeout_id, ==, 0);
	g_object_unref (location);
	nemo_file_unref (parent);
	destroy_details (self);
}

static void
test_no_gps (void)
{
	NemoPreviewDetails *self = new_details ();
	GFile *file = g_file_new_for_path ("plain.txt");
	GError *error = NULL;

	g_assert_true (g_file_set_contents ("plain.txt", "no camera metadata", -1, &error));
	g_assert_no_error (error);
	nemo_preview_details_set_file (self, file);
	wait_metadata (self);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->values[DETAIL_NAME])), ==, "plain.txt");
	g_assert_true (gtk_widget_get_visible (self->values[DETAIL_SIZE]));
	g_assert_true (gtk_widget_get_visible (self->values[DETAIL_TYPE]));
	g_assert_true (gtk_widget_get_visible (self->values[DETAIL_MODIFIED]));
	g_assert_true (gtk_widget_get_visible (self->values[DETAIL_LOCATION]));
	g_assert_false (gtk_widget_get_visible (self->values[DETAIL_GPS]));
	g_assert_false (gtk_widget_get_visible (self->values[DETAIL_CAMERA]));
	g_assert_false (gtk_widget_get_visible (self->map_box));
	g_assert_false (gtk_widget_get_visible (self->map_status));
	nemo_preview_details_clear (self);
	g_assert_false (gtk_widget_get_visible (self->values[DETAIL_NAME]));
	g_assert_null (self->file);
	g_assert_cmpint (g_remove ("plain.txt"), ==, 0);
	g_object_unref (file);
	destroy_details (self);
}

static void
wait_directory_size (NemoPreviewDetails *self)
{
	gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;

	while (g_strcmp0 (gtk_label_get_text (GTK_LABEL (self->values[DETAIL_SIZE])),
			 _("Calculating…")) == 0 &&
	       g_get_monotonic_time () < deadline) {
		g_main_context_iteration (NULL, FALSE);
		g_usleep (1000);
	}
	g_assert_nonnull (self->size_file);
	g_assert_cmpuint (self->size_timeout_id, ==, 0);
	g_assert_false (gtk_widget_get_visible (self->status));
}

static void
test_directory_size (void)
{
	NemoPreviewDetails *self = new_details ();
	GFile *file = g_file_new_for_path ("folder");
	GStatBuf statbuf;
	goffset expected, size;
	char *formatted;
	char contents[65536] = { 0 };

	g_assert_cmpint (g_mkdir ("folder", 0700), ==, 0);
	nemo_preview_details_set_file (self, file);
	wait_metadata (self);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->values[DETAIL_SIZE])),
			 ==, _("Calculating…"));
	g_assert_cmpuint (self->size_timeout_id, !=, 0);
	g_assert_null (self->size_file);
	wait_directory_size (self);
	formatted = g_format_size (0);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->values[DETAIL_SIZE])),
			 ==, formatted);
	g_free (formatted);
	nemo_preview_details_clear (self);

	g_assert_cmpint (g_mkdir ("folder/nested", 0700), ==, 0);
	g_assert_true (g_file_set_contents ("folder/nested/data", contents, sizeof contents, NULL));
	g_assert_true (g_file_set_contents ("folder/.hidden", contents, 12345, NULL));
	g_assert_cmpint (link ("folder/nested/data", "folder/hardlink"), ==, 0);
	g_assert_cmpint (symlink ("..", "folder/nested/loop"), ==, 0);
	g_assert_cmpint (g_lstat ("folder/nested", &statbuf), ==, 0);
	expected = sizeof contents + 12345 + statbuf.st_size;
	g_assert_cmpint (g_lstat ("folder/nested/loop", &statbuf), ==, 0);
	expected += statbuf.st_size;
	nemo_preview_details_set_file (self, file);
	wait_metadata (self);
	/* An info refresh before the debounce expires must still replace the
	 * cached empty-folder count when the scan starts. */
	nemo_preview_details_set_file (self, file);
	wait_metadata (self);
	wait_directory_size (self);
	g_assert_cmpint (nemo_file_get_deep_counts (self->size_file, NULL, NULL,
			 NULL, NULL, &size, TRUE), ==, NEMO_REQUEST_DONE);
	g_assert_cmpint (size, ==, expected);
	formatted = g_format_size (expected);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->values[DETAIL_SIZE])),
			 ==, formatted);
	g_free (formatted);
	destroy_details (self);
	g_object_unref (file);
	g_assert_cmpint (g_remove ("folder/nested/loop"), ==, 0);
	g_assert_cmpint (g_remove ("folder/hardlink"), ==, 0);
	g_assert_cmpint (g_remove ("folder/.hidden"), ==, 0);
	g_assert_cmpint (g_remove ("folder/nested/data"), ==, 0);
	g_assert_cmpint (g_rmdir ("folder/nested"), ==, 0);
	g_assert_cmpint (g_rmdir ("folder"), ==, 0);
}

static void
test_directory_size_cancel (gconstpointer data)
{
	guint mode = GPOINTER_TO_UINT (data);
	NemoPreviewDetails *self = new_details ();
	GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	GFile *file = g_file_new_for_path ("cancel-folder");
	GFile *replacement = g_file_new_for_path ("replacement.txt");

	g_assert_cmpint (g_mkdir ("cancel-folder", 0700), ==, 0);
	g_assert_true (g_file_set_contents ("replacement.txt", "replacement", -1, NULL));
	gtk_container_add (GTK_CONTAINER (window), GTK_WIDGET (self));
	gtk_widget_show_all (window);
	nemo_preview_details_set_file (self, file);
	wait_metadata (self);
	g_assert_cmpuint (self->size_timeout_id, !=, 0);
	if (mode != 0) {
		g_source_remove (self->size_timeout_id);
		start_directory_size (self);
		g_assert_nonnull (self->size_file);
	}
	if (mode == 2)
		gtk_widget_hide (window);
	else if (mode == 3)
		gtk_widget_destroy (GTK_WIDGET (self));
	else if (mode == 4)
		gtk_widget_hide (GTK_WIDGET (self));
	else
		nemo_preview_details_set_file (self, replacement);
	g_assert_cmpuint (self->size_timeout_id, ==, 0);
	g_assert_null (self->size_file);
	if (mode < 2) {
		wait_metadata (self);
		g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->values[DETAIL_NAME])),
				 ==, "replacement.txt");
	}
	gint64 deadline = g_get_monotonic_time () + 2 * DIRECTORY_SIZE_DELAY_MS * 1000;
	while (g_get_monotonic_time () < deadline) {
		g_main_context_iteration (NULL, FALSE);
		g_usleep (1000);
	}
	g_assert_cmpuint (self->size_timeout_id, ==, 0);
	g_assert_null (self->size_file);
	if (mode < 2) {
		char *formatted = g_format_size (strlen ("replacement"));
		g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->values[DETAIL_SIZE])),
				 ==, formatted);
		g_free (formatted);
	}
	if (mode == 2 || mode == 4) {
		gtk_widget_show_all (window);
		wait_metadata (self);
		wait_directory_size (self);
	}
	gtk_widget_destroy (window);
	destroy_details (self);
	g_object_unref (file);
	g_object_unref (replacement);
	g_assert_cmpint (g_remove ("replacement.txt"), ==, 0);
	g_assert_cmpint (g_rmdir ("cancel-folder"), ==, 0);
}

static void
test_directory_size_incomplete (void)
{
	NemoPreviewDetails *self = new_details ();
	NemoFile *file = nemo_file_get_by_uri ("file:///unused-preview-incomplete");
	char *formatted = g_format_size (8192);
	char *expected = g_strdup_printf (_("At least %s"), formatted);

	file->details->type = G_FILE_TYPE_DIRECTORY;
	file->details->deep_counts_status = NEMO_REQUEST_DONE;
	file->details->deep_unreadable_count = 1;
	file->details->deep_size = 8192;
	directory_size_ready_cb (file, self);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->values[DETAIL_SIZE])),
			 ==, expected);
	g_assert_true (gtk_widget_get_visible (self->status));
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->status)),
			 ==, _("Some folder contents could not be read."));
	nemo_file_unref (file);
	g_free (formatted);
	g_free (expected);
	destroy_details (self);
}

static void
test_stale_metadata_and_map (void)
{
	NemoPreviewDetails *self = new_details ();
	Completion metadata = { metadata_ready_cb, FALSE, NULL };
	Completion map = { map_tile_ready_cb, FALSE, NULL };
	GTask *meta_task = new_task (self, "obsolete", &metadata);
	GTask *map_task = new_task (self, "obsolete", &map);
	GCancellable *cancel = g_object_ref (self->cancellable);
	DetailsResult *result = g_new0 (DetailsResult, 1);

	result->has_gps = TRUE;
	result->latitude = 40.5;
	result->longitude = -70.25;
	result->values[DETAIL_NAME] = g_strdup ("obsolete name");
	result->values[DETAIL_GPS] = g_strdup ("obsolete GPS");
	g_task_return_pointer (meta_task, result, (GDestroyNotify) details_result_free);
	g_task_return_pointer (map_task, new_tile (), g_object_unref);
	nemo_preview_details_clear (self);
	g_assert_true (g_cancellable_is_cancelled (cancel));
	wait_done (&metadata);
	wait_done (&map);
	g_assert_false (gtk_widget_get_visible (self->values[DETAIL_NAME]));
	g_assert_false (gtk_widget_get_visible (self->values[DETAIL_GPS]));
	g_assert_false (gtk_widget_get_visible (self->map_box));
	g_assert_cmpint (gtk_image_get_storage_type (GTK_IMAGE (self->map_image)), ==, GTK_IMAGE_EMPTY);
	g_object_unref (meta_task);
	g_object_unref (map_task);
	g_object_unref (cancel);
	destroy_details (self);
}

static void
test_image_without_metadata (void)
{
	NemoPreviewDetails *self = new_details ();
	GdkPixbuf *pixbuf = new_tile ();
	GFile *file = g_file_new_for_path ("plain.png");
	GError *error = NULL;

	g_assert_true (gdk_pixbuf_save (pixbuf, "plain.png", "png", &error, NULL));
	g_assert_no_error (error);
	g_object_unref (pixbuf);
	nemo_preview_details_set_file (self, file);
	wait_metadata (self);
	g_assert_false (gtk_widget_get_visible (self->values[DETAIL_CAMERA]));
	g_assert_false (gtk_widget_get_visible (self->values[DETAIL_GPS]));
	g_assert_false (gtk_widget_get_visible (self->map_box));
	g_assert_false (gtk_widget_get_visible (self->map_status));
	g_assert_cmpint (g_remove ("plain.png"), ==, 0);
	g_object_unref (file);
	destroy_details (self);
}

static void
test_generation_guard (void)
{
	NemoPreviewDetails *self = new_details ();
	Completion completion = { metadata_ready_cb, FALSE, NULL };
	GTask *task = new_task (self, "obsolete", &completion);
	NemoPreviewDetails *current = details_load_get_current (task);
	DetailsResult *result = g_new0 (DetailsResult, 1);

	g_assert_true (current == self);
	g_object_unref (current);
	self->generation++;
	g_assert_false (g_cancellable_is_cancelled (self->cancellable));
	g_assert_null (details_load_get_current (task));
	result->values[DETAIL_NAME] = g_strdup ("obsolete");
	g_task_return_pointer (task, result, (GDestroyNotify) details_result_free);
	wait_done (&completion);
	g_assert_false (gtk_widget_get_visible (self->values[DETAIL_NAME]));
	g_object_unref (task);
	destroy_details (self);
}

static void
test_destroy_pending (void)
{
	NemoPreviewDetails *self = new_details ();
	NemoPreviewDetails *weak_self = self;
	Completion completion = { map_tile_ready_cb, FALSE, NULL };
	GTask *task = new_task (self, "obsolete", &completion);
	GCancellable *cancel = g_object_ref (self->cancellable);

	g_object_add_weak_pointer (G_OBJECT (self), (gpointer *) &weak_self);
	gtk_widget_destroy (GTK_WIDGET (self));
	g_assert_true (g_cancellable_is_cancelled (cancel));
	g_assert_null (details_load_get_current (task));
	nemo_preview_details_clear (self);
	nemo_preview_details_set_file (self, NULL);
	g_object_run_dispose (G_OBJECT (self));
	g_object_run_dispose (G_OBJECT (self));
	g_object_unref (self);
	g_assert_null (weak_self);
	g_task_return_pointer (task, new_tile (), g_object_unref);
	wait_done (&completion);
	g_object_unref (task);
	g_object_unref (cancel);
}

static void
test_hide_reload (void)
{
	NemoPreviewDetails *self = new_details ();
	GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	GFile *file = g_file_new_for_path ("hide.txt");
	GCancellable *cancel;
	GError *error = NULL;

	g_assert_true (g_file_set_contents ("hide.txt", "hidden", -1, &error));
	g_assert_no_error (error);
	gtk_container_add (GTK_CONTAINER (window), GTK_WIDGET (self));
	gtk_widget_show_all (window);
	nemo_preview_details_set_file (self, file);
	cancel = g_object_ref (self->cancellable);
	gtk_widget_hide (window);
	g_assert_true (g_cancellable_is_cancelled (cancel));
	g_assert_null (self->cancellable);
	g_assert_false (gtk_widget_get_visible (self->values[DETAIL_NAME]));
	gtk_widget_show (window);
	wait_metadata (self);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->values[DETAIL_NAME])), ==, "hide.txt");
	gtk_widget_hide (GTK_WIDGET (self));
	g_assert_null (self->cancellable);
	gtk_widget_show (GTK_WIDGET (self));
	wait_metadata (self);
	gtk_widget_destroy (window);
	g_object_unref (self);
	g_object_unref (cancel);
	g_object_unref (file);
	g_assert_cmpint (g_remove ("hide.txt"), ==, 0);
}

static void
test_map_cache (void)
{
	NemoPreviewDetails *self = new_details ();
	Completion completion = { map_tile_ready_cb, FALSE, NULL };
	GTask *task = new_task (self, "no-network-tile.png", &completion);
	DetailsLoadData *data = g_task_get_task_data (task);
	GdkPixbuf *pixbuf = new_tile ();
	GError *error = NULL;

	data->cache_path = g_strdup ("cached-map.png");
	data->pixel_x = data->pixel_y = 128;
	g_assert_true (gdk_pixbuf_save (pixbuf, data->cache_path, "png", &error, NULL));
	g_assert_no_error (error);
	g_object_unref (pixbuf);
	details_run_task (task, map_tile_worker);
	wait_done (&completion);
	pixbuf = gtk_image_get_pixbuf (GTK_IMAGE (self->map_image));
	g_assert_nonnull (pixbuf);
	g_assert_cmpint (gdk_pixbuf_get_width (pixbuf), ==, GPS_MAP_SIZE);
	g_assert_true (gtk_widget_get_visible (self->map_box));
	g_assert_false (gtk_widget_get_visible (self->map_status));
	g_assert_cmpint (g_remove (data->cache_path), ==, 0);
	g_object_unref (task);
	destroy_details (self);
}

static void
test_errors (void)
{
	NemoPreviewDetails *self = new_details ();
	Completion metadata = { metadata_ready_cb, FALSE, NULL };
	Completion map = { map_tile_ready_cb, FALSE, NULL };
	GTask *meta_task = new_task (self, "no-such-file", &metadata);
	GTask *map_task = new_task (self, "no-such-map.png", &map);
	DetailsLoadData *data = g_task_get_task_data (map_task);

	data->cache_path = g_strdup ("missing-cache.png");
	details_run_task (meta_task, metadata_worker);
	details_run_task (map_task, map_tile_worker);
	wait_done (&metadata);
	wait_done (&map);
	g_assert_true (gtk_widget_get_visible (self->status));
	g_assert_true (gtk_widget_get_visible (self->map_status));
	g_assert_false (gtk_widget_get_visible (self->map_box));
	g_object_unref (meta_task);
	g_object_unref (map_task);
	destroy_details (self);
}

static void
test_map_cache_write_failure (void)
{
	NemoPreviewDetails *self = new_details ();
	Completion completion = { map_tile_ready_cb, FALSE, NULL };
	GTask *task = new_task (self, "fetched-map.png", &completion);
	DetailsLoadData *data = g_task_get_task_data (task);
	GdkPixbuf *pixbuf = new_tile ();
	GError *error = NULL;

	g_assert_true (gdk_pixbuf_save (pixbuf, "fetched-map.png", "png", &error, NULL));
	g_assert_no_error (error);
	g_object_unref (pixbuf);
	g_assert_cmpint (g_mkdir ("blocked-map-cache", 0700), ==, 0);
	data->cache_path = g_strdup ("blocked-map-cache");
	data->pixel_x = data->pixel_y = 128;
	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*cannot cache GPS map*");
	details_run_task (task, map_tile_worker);
	wait_done (&completion);
	g_test_assert_expected_messages ();
	g_assert_nonnull (gtk_image_get_pixbuf (GTK_IMAGE (self->map_image)));
	g_assert_true (gtk_widget_get_visible (self->map_box));
	g_assert_false (gtk_widget_get_visible (self->map_status));
	g_assert_cmpint (g_rmdir ("blocked-map-cache"), ==, 0);
	g_assert_cmpint (g_remove ("fetched-map.png"), ==, 0);
	g_object_unref (task);
	destroy_details (self);
}

static void
test_coordinates (void)
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
	g_assert_cmpfloat (px, <, GPS_MAP_TILE_SIZE);
	g_assert_cmpfloat (py, <, GPS_MAP_TILE_SIZE);
#if defined (HAVE_EXIF) || defined (HAVE_LIBRAW)
	double lat[3] = { 40, 30, 0 }, lon[3] = { 70, 15, 0 };
	DetailsResult result = { 0 };

	set_gps (&result, lat, lon, '\0', 'W');
	g_assert_false (result.has_gps);
	lat[1] = 60;
	set_gps (&result, lat, lon, 'N', 'W');
	g_assert_false (result.has_gps);
	lat[1] = 30;
	lat[0] = NAN;
	set_gps (&result, lat, lon, 'N', 'W');
	g_assert_false (result.has_gps);
	lat[0] = 90;
	set_gps (&result, lat, lon, 'N', 'W');
	g_assert_false (result.has_gps);
	lat[0] = 40;
	set_gps (&result, lat, lon, 'N', 'W');
	g_assert_true (result.has_gps);
	g_assert_cmpfloat (result.latitude, ==, 40.5);
	g_assert_cmpfloat (result.longitude, ==, -70.25);
	g_free (result.values[DETAIL_GPS]);
#endif
}

static void
test_bounded_read (void)
{
	GFile *file = g_file_new_for_path ("bounded.bin");
	GError *error = NULL;
	GByteArray *bytes;
	GCancellable *cancel = g_cancellable_new ();

	g_assert_true (g_file_set_contents ("bounded.bin", "0123456789", -1, &error));
	g_assert_no_error (error);
	bytes = read_prefix (file, 4, cancel, &error);
	g_assert_no_error (error);
	g_assert_cmpuint (bytes->len, ==, 4);
	g_byte_array_unref (bytes);
	g_cancellable_cancel (cancel);
	bytes = read_prefix (file, 4, cancel, &error);
	g_assert_null (bytes);
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error (&error);
	g_object_unref (cancel);
	g_object_unref (file);
	g_assert_cmpint (g_remove ("bounded.bin"), ==, 0);
}

#ifdef HAVE_EXIF
static void
add_exif_entry (ExifData *exif, ExifIfd ifd, ExifTag tag, ExifFormat format,
		const guchar *bytes, guint size, guint components)
{
	ExifEntry *entry = exif_entry_new ();
	entry->tag = tag;
	entry->format = format;
	entry->components = components;
	entry->size = size;
	entry->data = g_malloc (size);
	memcpy (entry->data, bytes, size);
	exif_content_add_entry (exif->ifd[ifd], entry);
	exif_entry_unref (entry);
}

static ExifData *
new_exif_fixture (void)
{
	ExifData *exif = exif_data_new ();
	guchar values[24];
	ExifRational value = { 40, 1 };
	const char camera[] = "Synthetic camera";
	const char lens[] = "Fixture lens";

	exif_data_set_byte_order (exif, EXIF_BYTE_ORDER_INTEL);
	exif_set_rational (values, EXIF_BYTE_ORDER_INTEL, value);
	value.numerator = 30;
	exif_set_rational (values + 8, EXIF_BYTE_ORDER_INTEL, value);
	value.numerator = 0;
	exif_set_rational (values + 16, EXIF_BYTE_ORDER_INTEL, value);
	add_exif_entry (exif, EXIF_IFD_GPS, 2, EXIF_FORMAT_RATIONAL, values, sizeof values, 3);
	value.numerator = 70;
	exif_set_rational (values, EXIF_BYTE_ORDER_INTEL, value);
	value.numerator = 15;
	exif_set_rational (values + 8, EXIF_BYTE_ORDER_INTEL, value);
	add_exif_entry (exif, EXIF_IFD_GPS, 4, EXIF_FORMAT_RATIONAL, values, sizeof values, 3);
	add_exif_entry (exif, EXIF_IFD_GPS, 1, EXIF_FORMAT_ASCII, (const guchar *) "N", 2, 2);
	add_exif_entry (exif, EXIF_IFD_GPS, 3, EXIF_FORMAT_ASCII, (const guchar *) "W", 2, 2);
	add_exif_entry (exif, EXIF_IFD_0, EXIF_TAG_MODEL, EXIF_FORMAT_ASCII,
			(const guchar *) camera, sizeof camera, sizeof camera);
	add_exif_entry (exif, EXIF_IFD_EXIF, EXIF_TAG_LENS_MODEL, EXIF_FORMAT_ASCII,
			(const guchar *) lens, sizeof lens, sizeof lens);
	return exif;
}

static void
write_jpeg_fixture (const char *path)
{
	ExifData *exif = new_exif_fixture ();
	guchar header[6] = { 0xff, 0xd8, 0xff, 0xe1, 0, 0 };
	const guchar end[2] = { 0xff, 0xd9 };
	guchar *bytes = NULL;
	guint length = 0;
	GByteArray *jpeg = g_byte_array_new ();
	GError *error = NULL;

	exif_data_save_data (exif, &bytes, &length);
	g_assert_cmpuint (length, >, 0);
	g_assert_cmpuint (length, <, 65534);
	header[4] = (length + 2) >> 8;
	header[5] = (length + 2) & 0xff;
	g_byte_array_append (jpeg, header, sizeof header);
	g_byte_array_append (jpeg, bytes, length);
	g_byte_array_append (jpeg, end, sizeof end);
	g_assert_true (g_file_set_contents (path, (const char *) jpeg->data, jpeg->len, &error));
	g_assert_no_error (error);
	g_free (bytes);
	exif_data_unref (exif);
	g_byte_array_unref (jpeg);
}

static void
test_exif_validation (void)
{
	ExifData *exif = new_exif_fixture ();
	ExifEntry *entry = exif_content_get_entry (exif->ifd[EXIF_IFD_GPS], 2);
	double values[3];
	DetailsResult *result = g_new0 (DetailsResult, 1);
	ExifRational invalid = { 40, 0 };

	g_assert_true (read_gps_coordinate (entry, EXIF_BYTE_ORDER_INTEL, values));
	exif_set_rational (entry->data, EXIF_BYTE_ORDER_INTEL, invalid);
	g_assert_false (read_gps_coordinate (entry, EXIF_BYTE_ORDER_INTEL, values));
	extract_exif (result, exif);
	g_assert_false (result->has_gps);
	g_assert_cmpstr (result->values[DETAIL_CAMERA], ==, "Synthetic camera");
	details_result_free (result);
	entry->format = EXIF_FORMAT_LONG;
	g_assert_false (read_gps_coordinate (entry, EXIF_BYTE_ORDER_INTEL, values));
	exif_data_unref (exif);
}

static void
test_exif_worker (void)
{
	NemoPreviewDetails *self = new_details ();
	Completion completion = { NULL, FALSE, NULL };
	GTask *task = new_task (self, "fixture.jpg", &completion);

	write_jpeg_fixture ("fixture.jpg");
	details_run_task (task, metadata_worker);
	wait_done (&completion);
	g_assert_nonnull (completion.metadata);
	g_assert_true (completion.metadata->has_gps);
	g_assert_cmpfloat (completion.metadata->latitude, ==, 40.5);
	g_assert_cmpfloat (completion.metadata->longitude, ==, -70.25);
	g_assert_cmpstr (completion.metadata->values[DETAIL_CAMERA], ==, "Synthetic camera");
	g_assert_cmpstr (completion.metadata->values[DETAIL_LENS], ==, "Fixture lens");
	g_assert_null (completion.metadata->warning);
	details_result_free (completion.metadata);
	g_assert_cmpint (g_remove ("fixture.jpg"), ==, 0);
	g_object_unref (task);
	destroy_details (self);
}

static void
test_switch_from_gps (void)
{
	NemoPreviewDetails *self = new_details ();
	GFile *gps = g_file_new_for_path ("switch.jpg");
	GFile *plain = g_file_new_for_path ("switch.txt");
	GdkPixbuf *tile = new_tile ();
	GError *error = NULL;
	int x, y;
	double px, py;
	char *cache, *dir;
	gint64 deadline;

	write_jpeg_fixture ("switch.jpg");
	g_assert_true (g_file_set_contents ("switch.txt", "no GPS", -1, &error));
	g_assert_no_error (error);
	gps_to_tile (40.5, -70.25, GPS_MAP_ZOOM, &x, &y, &px, &py);
	cache = gps_map_cache_path (GPS_MAP_ZOOM, x, y);
	dir = g_path_get_dirname (cache);
	g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);
	g_assert_true (gdk_pixbuf_save (tile, cache, "png", &error, NULL));
	g_assert_no_error (error);
	g_object_unref (tile);
	nemo_preview_details_set_file (self, gps);
	wait_metadata (self);
	deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
	while (!gtk_widget_get_visible (self->map_box) && g_get_monotonic_time () < deadline) {
		g_main_context_iteration (NULL, FALSE);
		g_usleep (1000);
	}
	g_assert_true (gtk_widget_get_visible (self->map_box));
	g_assert_true (gtk_widget_get_visible (self->values[DETAIL_GPS]));
	nemo_preview_details_set_file (self, plain);
	g_assert_false (gtk_widget_get_visible (self->values[DETAIL_GPS]));
	g_assert_false (gtk_widget_get_visible (self->map_box));
	wait_metadata (self);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (self->values[DETAIL_NAME])), ==, "switch.txt");
	g_assert_false (gtk_widget_get_visible (self->map_box));
	g_assert_cmpint (g_remove (cache), ==, 0);
	g_assert_cmpint (g_remove ("switch.jpg"), ==, 0);
	g_assert_cmpint (g_remove ("switch.txt"), ==, 0);
	g_free (cache);
	g_free (dir);
	g_object_unref (gps);
	g_object_unref (plain);
	destroy_details (self);
}
#endif

#ifdef HAVE_LIBRAW
static void
put16 (guchar *bytes, guint16 value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
}

static void
put32 (guchar *bytes, guint32 value)
{
	for (guint i = 0; i < 4; i++)
		bytes[i] = value >> (i * 8);
}

typedef struct { GByteArray *bytes; guint offset, index; } FixtureIfd;

static FixtureIfd
new_ifd (GByteArray *bytes, guint entries)
{
	FixtureIfd ifd = { bytes, bytes->len, 0 };
	guint length = 2 + entries * 12 + 4;
	g_byte_array_set_size (bytes, bytes->len + length);
	memset (bytes->data + ifd.offset, 0, length);
	put16 (bytes->data + ifd.offset, entries);
	return ifd;
}

static void
ifd_entry (FixtureIfd *ifd, guint16 tag, guint16 type, guint32 count,
	   const guchar *value, guint length)
{
	guint at = ifd->offset + 2 + ifd->index++ * 12;
	guint offset = ifd->bytes->len;
	if (length > 4)
		g_byte_array_append (ifd->bytes, value, length);
	put16 (ifd->bytes->data + at, tag);
	put16 (ifd->bytes->data + at + 2, type);
	put32 (ifd->bytes->data + at + 4, count);
	if (length > 4)
		put32 (ifd->bytes->data + at + 8, offset);
	else
		memcpy (ifd->bytes->data + at + 8, value, length);
}

static void
ifd_number (FixtureIfd *ifd, guint16 tag, guint16 type, guint32 value)
{
	guchar bytes[4];
	put32 (bytes, value);
	ifd_entry (ifd, tag, type, 1, bytes, type == 3 ? 2 : 4);
}

static void
ifd_text (FixtureIfd *ifd, guint16 tag, const char *text)
{
	ifd_entry (ifd, tag, 2, strlen (text) + 1, (const guchar *) text, strlen (text) + 1);
}

static void
ifd_rational (FixtureIfd *ifd, guint16 tag, guint numerator, guint denominator)
{
	guchar bytes[8];
	put32 (bytes, numerator);
	put32 (bytes + 4, denominator);
	ifd_entry (ifd, tag, 5, 1, bytes, sizeof bytes);
}

static void
write_dng_fixture (gboolean with_gps)
{
	const guchar header[] = { 'I', 'I', 42, 0, 8, 0, 0, 0 };
	const guchar version[] = { 1, 4, 0, 0 }, cfa[] = { 0, 1, 1, 2 };
	const guchar repeat[] = { 2, 0, 2, 0 };
	guchar coords[24], matrix[72], pixels[2048] = { 0 };
	GByteArray *bytes = g_byte_array_new ();
	FixtureIfd image, exif, gps;
	GError *error = NULL;

	g_byte_array_append (bytes, header, sizeof header);
	image = new_ifd (bytes, with_gps ? 21 : 20);
	exif = new_ifd (bytes, 6);
	gps = new_ifd (bytes, 4);
	ifd_number (&image, 256, 4, 32);
	ifd_number (&image, 257, 4, 32);
	ifd_number (&image, 258, 3, 16);
	ifd_number (&image, 259, 3, 1);
	ifd_number (&image, 262, 3, 32803);
	ifd_text (&image, 271, "Fixture");
	ifd_text (&image, 272, "Metadata camera");
	ifd_number (&image, 277, 3, 1);
	ifd_number (&image, 278, 4, 32);
	ifd_number (&image, 279, 4, sizeof pixels);
	ifd_entry (&image, 33421, 3, 2, repeat, sizeof repeat);
	ifd_entry (&image, 33422, 1, 4, cfa, sizeof cfa);
	ifd_number (&image, 34665, 4, exif.offset);
	if (with_gps)
		ifd_number (&image, 34853, 4, gps.offset);
	ifd_entry (&image, 50706, 1, 4, version, sizeof version);
	ifd_entry (&image, 50707, 1, 4, version, sizeof version);
	ifd_text (&image, 50708, "Fixture Metadata camera");
	ifd_number (&image, 50714, 4, 0);
	ifd_number (&image, 50717, 4, 65535);
	for (guint i = 0; i < 9; i++) {
		put32 (matrix + i * 8, i % 4 == 0 ? 1 : 0);
		put32 (matrix + i * 8 + 4, 1);
	}
	ifd_entry (&image, 50721, 10, 9, matrix, sizeof matrix);
	ifd_rational (&exif, 33434, 1, 125);
	ifd_rational (&exif, 33437, 56, 10);
	ifd_number (&exif, 34855, 3, 400);
	ifd_text (&exif, 36867, "2026:01:02 03:04:05");
	ifd_rational (&exif, 37386, 50, 1);
	ifd_text (&exif, 42036, "Synthetic DNG lens");
	ifd_text (&gps, 1, "N");
	put32 (coords, 40); put32 (coords + 4, 1);
	put32 (coords + 8, 30); put32 (coords + 12, 1);
	put32 (coords + 16, 0); put32 (coords + 20, 1);
	ifd_entry (&gps, 2, 5, 3, coords, sizeof coords);
	ifd_text (&gps, 3, "W");
	put32 (coords, 70); put32 (coords + 8, 15);
	ifd_entry (&gps, 4, 5, 3, coords, sizeof coords);
	ifd_number (&image, 273, 4, bytes->len);
	g_byte_array_append (bytes, pixels, sizeof pixels);
	g_assert_true (g_file_set_contents ("fixture.dng", (const char *) bytes->data, bytes->len, &error));
	g_assert_no_error (error);
	g_byte_array_unref (bytes);
}

static void
test_raw_worker (void)
{
	NemoPreviewDetails *self = new_details ();
	Completion completion = { NULL, FALSE, NULL };
	GTask *task = new_task (self, "fixture.dng", &completion);
	DetailsResult *result;

	write_dng_fixture (TRUE);
	details_run_task (task, metadata_worker);
	wait_done (&completion);
	result = completion.metadata;
	g_assert_nonnull (result);
	g_assert_cmpstr (result->values[DETAIL_CAMERA], ==, "Fixture Metadata camera");
	g_assert_cmpstr (result->values[DETAIL_LENS], ==, "Synthetic DNG lens");
	g_assert_cmpstr (result->values[DETAIL_DIMENSIONS], ==, "32 × 32");
	g_assert_cmpstr (result->values[DETAIL_APERTURE], ==, "f/5.6");
	g_assert_cmpstr (result->values[DETAIL_SHUTTER], ==, "1/125 s");
	g_assert_cmpstr (result->values[DETAIL_ISO], ==, "400");
	g_assert_cmpstr (result->values[DETAIL_FOCAL], ==, "50.0 mm");
	g_assert_nonnull (result->values[DETAIL_TAKEN]);
	g_assert_true (result->has_gps);
	g_assert_cmpfloat (result->latitude, ==, 40.5);
	g_assert_cmpfloat (result->longitude, ==, -70.25);
	g_assert_null (result->warning);
	details_result_free (result);
	g_assert_cmpint (g_remove ("fixture.dng"), ==, 0);
	g_object_unref (task);
	destroy_details (self);
}

static void
test_raw_no_gps (void)
{
	NemoPreviewDetails *self = new_details ();
	Completion completion = { NULL, FALSE, NULL };
	GTask *task = new_task (self, "fixture.dng", &completion);

	write_dng_fixture (FALSE);
	details_run_task (task, metadata_worker);
	wait_done (&completion);
	g_assert_nonnull (completion.metadata);
	g_assert_cmpstr (completion.metadata->values[DETAIL_CAMERA], ==, "Fixture Metadata camera");
	g_assert_false (completion.metadata->has_gps);
	g_assert_null (completion.metadata->values[DETAIL_GPS]);
	g_assert_null (completion.metadata->warning);
	details_result_free (completion.metadata);
	g_assert_cmpint (g_remove ("fixture.dng"), ==, 0);
	g_object_unref (task);
	destroy_details (self);
}

static guint16
fixture_read16 (const guchar *bytes)
{
	return (guint16) bytes[0] | ((guint16) bytes[1] << 8);
}

static guint32
fixture_read32 (const guchar *bytes)
{
	return (guint32) bytes[0] | ((guint32) bytes[1] << 8) |
	       ((guint32) bytes[2] << 16) | ((guint32) bytes[3] << 24);
}

static void
relocate_fixture_ifd (guchar *bytes, guint32 ifd, guint32 shift)
{
	static const guint type_sizes[] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8 };
	guint count = fixture_read16 (bytes + ifd);

	for (guint i = 0; i < count; i++) {
		guchar *entry = bytes + ifd + 2 + i * 12;
		guint tag = fixture_read16 (entry), type = fixture_read16 (entry + 2);
		guint32 components = fixture_read32 (entry + 4);
		guint32 value = fixture_read32 (entry + 8);
		gboolean ifd_pointer = tag == 34665 || tag == 34853;

		g_assert_cmpuint (type, <, G_N_ELEMENTS (type_sizes));
		if (ifd_pointer)
			relocate_fixture_ifd (bytes, value, shift);
		if (ifd_pointer || tag == 273 || components * type_sizes[type] > 4)
			put32 (entry + 8, value + shift);
	}
}

static void
test_raw_late_ifd (void)
{
	NemoPreviewDetails *self = new_details ();
	Completion completion = { NULL, FALSE, NULL };
	GTask *task = new_task (self, "late-ifd.dng", &completion);
	GFile *file = g_file_new_for_path ("late-ifd.dng");
	GFileOutputStream *stream;
	GError *error = NULL;
	char *contents;
	gsize length;
	guint32 shift = METADATA_READ_LIMIT + 4096;

	write_dng_fixture (TRUE);
	g_assert_true (g_file_get_contents ("fixture.dng", &contents, &length, &error));
	g_assert_no_error (error);
	relocate_fixture_ifd ((guchar *) contents, 8, shift);
	put32 ((guchar *) contents + 4, 8 + shift);
	stream = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_PRIVATE, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (stream);
	g_assert_true (g_output_stream_write_all (G_OUTPUT_STREAM (stream), contents, 8,
						 NULL, NULL, &error));
	g_assert_no_error (error);
	/* Sparse padding puts all metadata past the former prefix limit without
	 * allocating a large buffer or writing full-sized image pixel data. */
	g_assert_true (g_seekable_seek (G_SEEKABLE (stream), 8 + shift, G_SEEK_SET, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (g_output_stream_write_all (G_OUTPUT_STREAM (stream), contents + 8, length - 8,
						 NULL, NULL, &error));
	g_assert_no_error (error);
	g_assert_true (g_output_stream_close (G_OUTPUT_STREAM (stream), NULL, &error));
	g_assert_no_error (error);
	g_object_unref (stream);
	g_free (contents);
	details_run_task (task, metadata_worker);
	wait_done (&completion);
	g_assert_nonnull (completion.metadata);
	g_assert_cmpstr (completion.metadata->values[DETAIL_CAMERA], ==, "Fixture Metadata camera");
	g_assert_cmpstr (completion.metadata->values[DETAIL_LENS], ==, "Synthetic DNG lens");
	g_assert_true (completion.metadata->has_gps);
	g_assert_cmpfloat (completion.metadata->latitude, ==, 40.5);
	g_assert_cmpfloat (completion.metadata->longitude, ==, -70.25);
	g_assert_null (completion.metadata->warning);
	details_result_free (completion.metadata);
	g_assert_cmpint (g_remove ("fixture.dng"), ==, 0);
	g_assert_cmpint (g_remove ("late-ifd.dng"), ==, 0);
	g_object_unref (task);
	g_object_unref (file);
	destroy_details (self);
}

static void
test_corrupt_raw (void)
{
	NemoPreviewDetails *self = new_details ();
	Completion completion = { NULL, FALSE, NULL };
	GTask *task = new_task (self, "corrupt.dng", &completion);
	GError *error = NULL;

	g_assert_true (g_file_set_contents ("corrupt.dng", "not a RAW image", -1, &error));
	g_assert_no_error (error);
	details_run_task (task, metadata_worker);
	wait_done (&completion);
	g_assert_nonnull (completion.metadata);
	g_assert_cmpstr (completion.metadata->values[DETAIL_NAME], ==, "corrupt.dng");
	g_assert_nonnull (completion.metadata->warning);
	g_assert_nonnull (strstr (completion.metadata->warning, "Unable to decode RAW metadata"));
	g_assert_false (completion.metadata->has_gps);
	g_assert_null (completion.metadata->values[DETAIL_CAMERA]);
	details_result_free (completion.metadata);
	g_assert_cmpint (g_remove ("corrupt.dng"), ==, 0);
	g_object_unref (task);
	destroy_details (self);
}
#endif

int
main (int argc, char **argv)
{
	char *scratch, *cwd;
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
	cwd = g_get_current_dir ();
	scratch = g_strdup_printf ("details-test-data-%u", (guint) getpid ());
	g_assert_cmpint (g_mkdir (scratch, 0700), ==, 0);
	g_assert_cmpint (g_chdir (scratch), ==, 0);
	g_test_add_func ("/preview-details/no-gps", test_no_gps);
	g_test_add_func ("/preview-details/navigation-only-parent", test_parent_entry);
	g_test_add_func ("/preview-details/directory-size", test_directory_size);
	g_test_add_func ("/preview-details/directory-size-incomplete", test_directory_size_incomplete);
	g_test_add_data_func ("/preview-details/directory-switch-before-scan",
			      GUINT_TO_POINTER (0), test_directory_size_cancel);
	g_test_add_data_func ("/preview-details/directory-switch-during-scan",
			      GUINT_TO_POINTER (1), test_directory_size_cancel);
	g_test_add_data_func ("/preview-details/directory-hide-during-scan",
			      GUINT_TO_POINTER (2), test_directory_size_cancel);
	g_test_add_data_func ("/preview-details/directory-destroy-during-scan",
			      GUINT_TO_POINTER (3), test_directory_size_cancel);
	g_test_add_data_func ("/preview-details/directory-hide-details-during-scan",
			      GUINT_TO_POINTER (4), test_directory_size_cancel);
	g_test_add_func ("/preview-details/image-without-metadata", test_image_without_metadata);
	g_test_add_func ("/preview-details/stale-metadata-map", test_stale_metadata_and_map);
	g_test_add_func ("/preview-details/generation", test_generation_guard);
	g_test_add_func ("/preview-details/destroy-pending", test_destroy_pending);
	g_test_add_func ("/preview-details/hide-reload", test_hide_reload);
	g_test_add_func ("/preview-details/map-cache", test_map_cache);
	g_test_add_func ("/preview-details/map-cache-write-failure", test_map_cache_write_failure);
	g_test_add_func ("/preview-details/errors", test_errors);
	g_test_add_func ("/preview-details/coordinates", test_coordinates);
	g_test_add_func ("/preview-details/bounded-read", test_bounded_read);
#ifdef HAVE_EXIF
	g_test_add_func ("/preview-details/exif-validation", test_exif_validation);
	g_test_add_func ("/preview-details/exif-worker", test_exif_worker);
	g_test_add_func ("/preview-details/switch-from-gps", test_switch_from_gps);
#endif
#ifdef HAVE_LIBRAW
	g_test_add_func ("/preview-details/raw-dng-worker", test_raw_worker);
	g_test_add_func ("/preview-details/raw-no-gps", test_raw_no_gps);
	g_test_add_func ("/preview-details/raw-late-ifd", test_raw_late_ifd);
	g_test_add_func ("/preview-details/corrupt-raw", test_corrupt_raw);
#endif
	result = g_test_run ();
	g_assert_cmpint (g_chdir (cwd), ==, 0);
	g_assert_cmpint (g_rmdir (scratch), ==, 0);
	g_free (scratch);
	g_free (cwd);
	return result;
}
