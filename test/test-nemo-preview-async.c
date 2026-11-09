/* Isolated GTK regressions: no Nemo application, desktop, or device access.
 * Include the widgets so delayed backend completions can be injected without
 * adding fault switches or test-only APIs to production code. */
#include <config.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <libnemo-private/nemo-file-operations.h>
#include <libnemo-private/nemo-file-utilities.h>
#include "../src/nemo-quick-preview.h"
#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include "preview-media-fixture.h"
#endif
#ifdef HAVE_LIBRAW
#include <libraw/libraw.h>
#endif

static GThread *main_thread;
static const char *test_program;
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
static GFile *delete_target;
static NemoDeleteCallback delete_callback;
static gpointer delete_callback_data;
static guint delete_calls;
static gboolean real_preview_delete;

int
__wrap_nemo_inhibit_power_manager (const char *message)
{
	return -1;
}

static void
test_preview_trash (GList *files, GtkWindow *parent, NemoDeleteCallback callback, gpointer data)
{
	g_assert_cmpuint (g_list_length (files), ==, 1);
	g_assert_true (NEMO_IS_QUICK_PREVIEW (parent));
	if (real_preview_delete) {
		nemo_file_operations_trash_or_delete (files, parent, callback, data);
		return;
	}
	delete_calls++;
	g_set_object (&delete_target, files->data);
	delete_callback = callback;
	delete_callback_data = data;
}

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
#define nemo_file_operations_trash_or_delete test_preview_trash
#define gst_element_factory_make preview_media_factory_make
#include "../src/nemo-quick-preview.c"
#undef gst_element_factory_make
#undef nemo_file_operations_trash_or_delete
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

typedef struct {
	const char *text;
	gboolean found;
} VisibleText;

static void
find_visible_text (GtkWidget *widget, gpointer data)
{
	VisibleText *match = data;
	if (GTK_IS_LABEL (widget) && gtk_widget_get_visible (widget) &&
	    g_str_equal (gtk_label_get_text (GTK_LABEL (widget)), match->text))
		match->found = TRUE;
	else if (!match->found && GTK_IS_CONTAINER (widget))
		gtk_container_forall (GTK_CONTAINER (widget), find_visible_text, data);
}

static gboolean
details_have_text (NemoQuickPreview *preview, const char *text)
{
	VisibleText match = { text, FALSE };
	find_visible_text (GTK_WIDGET (preview->details), &match);
	return match.found;
}

static void
test_quick_image_information (void)
{
	char *directory = g_dir_make_tmp ("nemo-quick-info-XXXXXX", NULL);
	GFile *file = write_image (directory, "image & <one>.png", 0x00ff00ff);
	GFile *other = write_image (directory, "other.png", 0x0000ffff);
	NemoQuickPreview *preview = g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL);
	g_object_ref_sink (preview);
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->mode == PREVIEW_IMAGE &&
		    preview->image_viewer->original_pixbuf != NULL);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (preview->filename_label)),
			==, "image & <one>.png");
	g_assert_true (gtk_widget_get_visible (preview->image_viewer->ctrl_box));
	g_assert_false (gtk_widget_get_visible (preview->details_scroll));

	GdkEventKey key = { .type = GDK_KEY_PRESS, .keyval = GDK_KEY_i };
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	g_assert_true (gtk_widget_get_visible (preview->details_scroll));
	g_assert_true (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (preview->info_button)));
	WAIT_UNTIL (details_have_text (preview, "4 × 4"));
	g_assert_true (details_have_text (preview, "1:1 (1.000:1)"));
	assert_responsive ();
	double fit = MIN ((gtk_widget_get_allocated_width (preview->image_viewer->scroll) - 20.0) / 4,
			  (gtk_widget_get_allocated_height (preview->image_viewer->scroll) - 20.0) / 4);
	g_assert_cmpfloat_with_epsilon (nemo_image_viewer_get_zoom (preview->image_viewer), fit, 0.001);
	gint footer_x, footer_y, image_x, image_y;
	g_assert_true (gtk_widget_translate_coordinates (preview->filename_label,
		GTK_WIDGET (preview), 0, 0, &footer_x, &footer_y));
	g_assert_true (gtk_widget_translate_coordinates (GTK_WIDGET (preview->image_viewer),
		GTK_WIDGET (preview), 0, 0, &image_x, &image_y));
	g_assert_cmpint (footer_y, >=,
		image_y + gtk_widget_get_allocated_height (GTK_WIDGET (preview->image_viewer)));

	nemo_quick_preview_show_file (preview, other, NULL);
	WAIT_UNTIL (preview->mode == PREVIEW_IMAGE &&
		    preview->image_viewer->original_pixbuf != NULL);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (preview->filename_label)), ==, "other.png");
	g_assert_true (gtk_widget_get_visible (preview->details_scroll));
	key.keyval = GDK_KEY_I;
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	g_assert_false (gtk_widget_get_visible (preview->details_scroll));
	key.state = GDK_CONTROL_MASK;
	g_assert_false (on_key_press (GTK_WIDGET (preview), &key, NULL));
	g_assert_false (gtk_widget_get_visible (preview->details_scroll));

	nemo_quick_preview_dismiss (preview);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (preview->filename_label)), ==, "");
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	iterate_for (100);
	remove_fixture (file);
	remove_fixture (other);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (directory);
}

static void
test_quick_page_keys (void)
{
	char *directory = g_dir_make_tmp ("nemo-page-keys-XXXXXX", NULL);
	char *path = g_build_filename (directory, "first.txt", NULL);
	char *other_path = g_build_filename (directory, "second.txt", NULL);
	GString *text = g_string_new (NULL);
	for (guint i = 0; i < 1000; i++)
		g_string_append_printf (text, "Line %u: page navigation fixture\n", i);
	g_assert_true (g_file_set_contents (path, text->str, text->len, NULL));
	g_assert_true (g_file_set_contents (other_path, text->str, text->len, NULL));
	g_string_free (text, TRUE);
	GFile *file = g_file_new_for_path (path);
	GFile *other = g_file_new_for_path (other_path);
	NemoQuickPreview *preview = g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL);
	g_object_ref_sink (preview);
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->mode == PREVIEW_TEXT && preview->dir_files != NULL);
	GList *children = gtk_container_get_children (GTK_CONTAINER (preview->paged_viewer));
	GtkAdjustment *adjustment = NULL;
	for (GList *l = children; l != NULL; l = l->next)
		if (GTK_IS_SCROLLBAR (l->data))
			adjustment = gtk_range_get_adjustment (GTK_RANGE (l->data));
	g_list_free (children);
	g_assert_nonnull (adjustment);
	WAIT_UNTIL (gtk_adjustment_get_upper (adjustment) > 100);
	gtk_widget_grab_focus (preview->next_button);
	GdkEventKey event = { .type = GDK_KEY_PRESS, .keyval = GDK_KEY_Page_Down };
	g_assert_true (on_key_press (GTK_WIDGET (preview), &event, NULL));
	WAIT_UNTIL (gtk_adjustment_get_value (adjustment) > 0);
	g_assert_true (g_file_equal (preview->current_file, file));
	event.keyval = GDK_KEY_KP_Page_Up;
	g_assert_true (on_key_press (GTK_WIDGET (preview), &event, NULL));
	WAIT_UNTIL (gtk_adjustment_get_value (adjustment) == 0);
	event.keyval = GDK_KEY_Right;
	g_assert_false (on_key_press (GTK_WIDGET (preview), &event, NULL));
	g_assert_true (g_file_equal (preview->current_file, file));
	event.state = GDK_CONTROL_MASK;
	g_assert_true (on_key_press (GTK_WIDGET (preview), &event, NULL));
	WAIT_UNTIL (g_file_equal (preview->current_file, other));
	g_settings_set_string (nemo_keybinding_settings, "quick-preview-previous-file", "<Control><Alt>p");
	g_autofree char *label = gtk_accelerator_get_label (GDK_KEY_p, GDK_CONTROL_MASK | GDK_MOD1_MASK);
	g_autofree char *tooltip = gtk_widget_get_tooltip_text (preview->prev_button);
	g_assert_nonnull (strstr (tooltip, label));
	event.keyval = GDK_KEY_Left;
	g_assert_false (on_key_press (GTK_WIDGET (preview), &event, NULL));
	event.keyval = GDK_KEY_p;
	event.state = GDK_CONTROL_MASK | GDK_MOD1_MASK;
	g_assert_true (on_key_press (GTK_WIDGET (preview), &event, NULL));
	WAIT_UNTIL (g_file_equal (preview->current_file, file));
	gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (preview->search_bar), TRUE);
	gtk_widget_grab_focus (preview->search_entry);
	event.keyval = GDK_KEY_Right;
	event.state = GDK_CONTROL_MASK;
	g_assert_false (on_key_press (GTK_WIDGET (preview), &event, NULL));
	event.keyval = GDK_KEY_space;
	event.state = 0;
	g_assert_false (on_key_press (GTK_WIDGET (preview), &event, NULL));
	g_assert_true (g_file_equal (preview->current_file, file));
	gtk_widget_grab_focus (preview->next_button);
	g_settings_set_string (nemo_keybinding_settings, "quick-preview-next-file", "");
	event.keyval = GDK_KEY_Right;
	event.state = GDK_CONTROL_MASK;
	g_assert_false (on_key_press (GTK_WIDGET (preview), &event, NULL));
	g_settings_reset (nemo_keybinding_settings, "quick-preview-next-file");
	g_settings_reset (nemo_keybinding_settings, "quick-preview-previous-file");
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	iterate_for (100);
	remove_fixture (file);
	remove_fixture (other);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (path);
	g_free (other_path);
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
	GdkEventKey info_key = { .type = GDK_KEY_PRESS, .keyval = GDK_KEY_i };
	gtk_widget_grab_focus (preview->search_entry);
	g_assert_false (on_key_press (GTK_WIDGET (preview), &info_key, NULL));
	g_assert_false (gtk_widget_get_visible (preview->details_scroll));
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

#ifdef HAVE_DOCUMENT_PREVIEW
#include "document-preview-fixture.h"

static void
test_quick_rich_document (void)
{
	gboolean sandbox = document_sandbox_available ();
	char *directory = g_dir_make_tmp ("nemo-rich-preview-XXXXXX", NULL);
	char *path = g_build_filename (directory, "book.md", NULL);
	char *other_path = g_build_filename (directory, "next.txt", NULL);
	GString *markdown = g_string_new ("# Native document preview\n");
	for (guint i = 0; i < 48; i++)
		g_string_append_printf (markdown, "\n\n## Section %u\n\n"
			"Readable native document content with **bold words**, tables and paging.\n\n"
			"| Name | Value |\n| --- | --- |\n| Entry | %u |\n", i, i);
	g_string_append (markdown, "\n\nunique document needle\n");
	g_assert_true (g_file_set_contents (path, markdown->str, markdown->len, NULL));
	g_string_free (markdown, TRUE);
	g_assert_true (g_file_set_contents (other_path, "Another file", -1, NULL));
	GFile *file = g_file_new_for_path (path);
	GFile *other = g_file_new_for_path (other_path);
	NemoQuickPreview *preview = g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL);
	g_object_ref_sink (preview);
	DocumentResult result = { 0 };
	g_signal_connect (preview->document_viewer, "load-finished", G_CALLBACK (document_finished_cb), &result);
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->mode == PREVIEW_DOCUMENT);
	g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (preview->stack)), ==, "document");
	WAIT_UNTIL (result.finished);
	if (!sandbox) {
		g_assert_nonnull (result.error);
		g_assert_cmpuint (nemo_document_viewer_get_page_count (preview->document_viewer), ==, 0);
	} else {
		g_assert_no_error (result.error);
		g_assert_true (preview->document_ready);
		g_assert_cmpuint (nemo_document_viewer_get_page_count (preview->document_viewer), >, 1);
		gtk_widget_grab_focus (preview->next_button);
		GdkEventKey event = { .type = GDK_KEY_PRESS, .keyval = GDK_KEY_Page_Down };
		for (guint i = 0; i < 12 && nemo_document_viewer_get_page (preview->document_viewer) == 0; i++) {
			g_assert_true (on_key_press (GTK_WIDGET (preview), &event, NULL));
			iterate_for (250);
		}
		WAIT_UNTIL (nemo_document_viewer_get_page (preview->document_viewer) > 0);
		g_assert_true (g_file_equal (preview->current_file, file));
		gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (preview->search_bar), TRUE);
		gtk_entry_set_text (GTK_ENTRY (preview->search_entry), "unique document needle");
		on_search_changed (GTK_SEARCH_ENTRY (preview->search_entry), preview);
		WAIT_UNTIL (!nemo_document_viewer_search_is_pending (preview->document_viewer) &&
		            nemo_document_viewer_search_has_match (preview->document_viewer));
		iterate_for (500);
		g_assert_false (nemo_document_viewer_search_is_pending (preview->document_viewer));
	}
	WAIT_UNTIL (preview->dir_files != NULL);
	gtk_widget_grab_focus (preview->next_button);
	GdkEventKey next = { .type = GDK_KEY_PRESS, .keyval = GDK_KEY_Right, .state = GDK_CONTROL_MASK };
	g_assert_true (on_key_press (GTK_WIDGET (preview), &next, NULL));
	WAIT_UNTIL (g_file_equal (preview->current_file, other) && preview->mode == PREVIEW_TEXT);
	g_assert_cmpuint (nemo_document_viewer_get_page_count (preview->document_viewer), ==, 0);
	g_signal_handlers_disconnect_by_data (preview->document_viewer, &result);
	g_clear_error (&result.error);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	iterate_for (100);
	remove_fixture (file);
	remove_fixture (other);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (path);
	g_free (other_path);
	g_free (directory);
}
#endif

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
	preview_show_media (preview, file, FALSE);
	WAIT_UNTIL (g_atomic_int_get (&delayed_calls) == 1);
	GdkEventKey key = { .type = GDK_KEY_PRESS, .keyval = GDK_KEY_Right };
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	key.keyval = GDK_KEY_period;
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	seek_position_update_cb (preview);
	seek_release_cb (preview->seek_scale, NULL, preview);
	preview_show_media (preview, file, FALSE);
	WAIT_UNTIL (g_atomic_int_get (&delayed_calls) == 2);
	for (guint i = 0; i < 20; i++)
		preview_show_media (preview, file, FALSE);
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
	preview_show_media (preview, file, FALSE);
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

static void
test_media_long_webm (void)
{
	g_autofree char *path = preview_media_fixture (3840, 2160);
	GFile *file = g_file_new_for_path (path);

	for (int cycle = 0; cycle < 8; cycle++) {
		NemoQuickPreview *preview = g_object_ref_sink (
			g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
		nemo_quick_preview_show_file (preview, file, NULL);
		WAIT_UNTIL (preview->media_ready && preview->frame_surface != NULL);
		g_assert_nonnull (preview->pipeline);
		g_assert_cmpuint (preview->media_load_timeout_id, ==, 0);
		g_assert_cmpint (preview->video_width, ==, 1920);
		g_assert_cmpint (preview->video_height, ==, 1080);
		if (cycle == 0) {
			GdkEventKey key = { .type = GDK_KEY_PRESS, .keyval = GDK_KEY_i };
			g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
			WAIT_UNTIL (details_have_text (preview, "3840 × 2160"));
			g_assert_true (details_have_text (preview, "16:9 (1.778:1)"));
		}
		guint32 pixel;
		memcpy (&pixel, cairo_image_surface_get_data (preview->frame_surface), sizeof pixel);
		g_assert_cmphex (pixel & 0x00ff0000, >, 0x00800000);
		gint64 duration = -1;
		g_assert_true (gst_element_query_duration (preview->pipeline, GST_FORMAT_TIME, &duration));
		g_assert_cmpint (duration, >=, 5400 * GST_SECOND);
		preview->media_ready = FALSE;
		g_assert_true (gst_element_seek_simple (preview->pipeline, GST_FORMAT_TIME,
			GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 3600 * GST_SECOND));
		WAIT_UNTIL (preview->media_ready);
		gint64 position = -1;
		g_assert_true (gst_element_query_position (preview->pipeline, GST_FORMAT_TIME, &position));
		g_assert_cmpint (position, >=, 3600 * GST_SECOND);
		play_pause_clicked_cb (GTK_BUTTON (preview->play_btn), preview);
		iterate_for (30);
		play_pause_clicked_cb (GTK_BUTTON (preview->play_btn), preview);
		gpointer weak_pipeline = preview->pipeline;
		GstElement *sink;
		g_object_add_weak_pointer (G_OBJECT (preview->pipeline), &weak_pipeline);
		g_object_get (preview->pipeline, "video-sink", &sink, NULL);
		gpointer weak_sink = sink;
		g_object_add_weak_pointer (G_OBJECT (sink), &weak_sink);
		gst_object_unref (sink);
		gtk_widget_destroy (GTK_WIDGET (preview));
		g_object_unref (preview);
		WAIT_UNTIL (weak_pipeline == NULL && weak_sink == NULL);
		g_assert_cmpint (g_atomic_int_get (&media_pipeline_count), ==, 0);
	}
	g_object_unref (file);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
test_media_missing_video (void)
{
	g_autofree char *path = preview_media_fixture (128, 72);
	GFile *file = g_file_new_for_path (path);
	GPtrArray *ranks = preview_disable_video_decoders ();
	NemoQuickPreview *preview = g_object_ref_sink (
		g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	preview_show_media (preview, file, FALSE);
	WAIT_UNTIL (preview->pipeline == NULL);
	g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (preview->stack)), ==, "status");
	g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (preview->status_label)), "VP8"));
	g_assert_cmpuint (preview->media_load_timeout_id, ==, 0);
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	g_ptr_array_unref (ranks);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	g_object_unref (file);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static void
test_media_loading_timeout (void)
{
	NemoQuickPreview *preview = g_object_ref_sink (
		g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	GFile *file = g_file_new_for_uri ("test-preview:///video.webm");
	reset_delay ();
	g_atomic_int_set (&delay_state, TRUE);
	preview_show_media (preview, file, FALSE);
	WAIT_UNTIL (g_atomic_int_get (&delayed_calls) == 1);
	g_assert_cmpuint (preview->media_load_timeout_id, >, 0);
	g_source_remove (preview->media_load_timeout_id);
	g_assert_false (media_load_timed_out (preview));
	g_assert_null (preview->pipeline);
	g_assert_cmpuint (preview->media_load_timeout_id, ==, 0);
	g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (preview->status_label)), "timed out"));
	assert_responsive ();
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	release_backend ();
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	g_atomic_int_set (&delay_state, FALSE);
	g_object_unref (file);
}

static void
test_media_audio_only (void)
{
	g_autofree char *path = preview_media_fixture (0, 0);
	GFile *file = g_file_new_for_path (path);
	NemoQuickPreview *preview = g_object_ref_sink (
		g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	preview_show_media (preview, file, TRUE);
	WAIT_UNTIL (preview->media_ready);
	g_assert_nonnull (preview->pipeline);
	g_assert_null (preview->frame_surface);
	g_assert_cmpuint (preview->media_load_timeout_id, ==, 0);
	media_stop (preview);
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	preview_show_media (preview, file, FALSE);
	WAIT_UNTIL (preview->pipeline == NULL);
	g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (preview->status_label)), "video stream"));
	g_assert_cmpuint (preview->media_load_timeout_id, ==, 0);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	g_object_unref (file);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static gint64
media_position (NemoQuickPreview *preview)
{
	gint64 position = -1;
	g_assert_true (gst_element_query_position (preview->pipeline, GST_FORMAT_TIME, &position));
	return position;
}

static void
media_seek_to (NemoQuickPreview *preview, gint64 position)
{
	preview->media_ready = FALSE;
	g_assert_true (gst_element_seek_simple (preview->pipeline, GST_FORMAT_TIME,
		GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, position));
	WAIT_UNTIL (preview->media_ready);
	g_assert_cmpint (media_position (preview), ==, position);
}

static void
test_media_resume (gconstpointer data)
{
	gboolean audio = GPOINTER_TO_INT (data);
	if (g_test_subprocess ()) {
		GFile *file = g_file_new_for_uri (g_getenv ("NEMO_TEST_SAVED_MEDIA_URI"));
		NemoQuickPreview *preview = g_object_ref_sink (g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
		nemo_quick_preview_show_file (preview, file, NULL);
		WAIT_UNTIL (preview->media_ready);
		g_assert_cmpint (media_position (preview), ==, 5 * GST_SECOND);
		g_assert_false (preview->video_playing);
		gtk_widget_destroy (GTK_WIDGET (preview));
		g_object_unref (preview);
		WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
		g_object_unref (file);
		return;
	}
	g_autofree char *generated = audio ? preview_media_encode_fixture (
		"audiotestsrc num-buffers=480 samplesperbuffer=1000 ! "
		"audio/x-raw,rate=48000 ! audioconvert ! opusenc ! oggmux ! filesink name=output", "ogg")
		: preview_media_encode_fixture (
		"videotestsrc num-buffers=300 ! video/x-raw,width=128,height=72,framerate=30/1 ! "
		"vp8enc deadline=1 keyframe-max-dist=1 ! webmmux ! filesink name=output", "webm");
	g_autofree char *directory = g_dir_make_tmp ("nemo-media-resume-XXXXXX", NULL);
	g_autofree char *path = g_build_filename (directory, audio ? "first.ogg" : "first.webm", NULL);
	g_assert_cmpint (g_rename (generated, path), ==, 0);
	GFile *file = g_file_new_for_path (path);
	GFile *other = write_image (directory, "second.png", 0x00ff00ff);
	NemoQuickPreview *preview = g_object_ref_sink (g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_REMEMBER_DOCUMENT_POSITION, TRUE);
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->media_ready && preview->dir_files != NULL);
	g_assert_cmpint (preview->media_is_audio, ==, audio);
	media_seek_to (preview, 3 * GST_SECOND);
	seek_position_update_cb (preview);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (preview->time_label)), ==,
		audio ? "00:00:03" : "00:00:03:00  Frame 90");

	/* Navigation changes current_file before teardown; save the outgoing file. */
	navigate_to_offset (preview, 1);
	WAIT_UNTIL (preview->mode == PREVIEW_IMAGE);
	gint64 saved;
	g_assert_true (nemo_media_position_lookup (file, &saved));
	g_assert_cmpint (saved, ==, 3 * GST_SECOND);
	g_assert_false (nemo_media_position_lookup (other, &saved));
	navigate_to_offset (preview, -1);
	WAIT_UNTIL (preview->media_ready);
	g_assert_cmpint (media_position (preview), ==, 3 * GST_SECOND);
	g_assert_false (preview->video_playing);
	GstState state;
	gst_element_get_state (preview->pipeline, &state, NULL, 0);
	g_assert_cmpint (state, ==, GST_STATE_PAUSED);
	media_seek_to (preview, 5 * GST_SECOND);
	nemo_quick_preview_dismiss (preview);

	g_autofree char *user_dir = nemo_get_user_directory ();
	g_autofree char *positions_path = g_build_filename (user_dir, "document-positions", NULL);
	g_autoptr (GKeyFile) positions = g_key_file_new ();
	g_assert_true (g_key_file_load_from_file (positions, positions_path, G_KEY_FILE_NONE, NULL));
	g_autofree char *uri = g_file_get_uri (file);
	g_autofree char *escaped = g_uri_escape_string (uri, "/:@+$,-_.!~*'()", FALSE);
	g_autofree char *group = g_strconcat ("media:", escaped, NULL);
	g_assert_cmpint (g_key_file_get_int64 (positions, group, "nanoseconds", NULL), ==, 5 * GST_SECOND);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);

	g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_setenv (launcher, "NEMO_TEST_SAVED_MEDIA_URI", uri, TRUE);
	g_autoptr (GError) error = NULL;
	g_autoptr (GSubprocess) reader = g_subprocess_launcher_spawn (
		launcher, &error, test_program, "-p", g_test_get_path (), "--GTestSubprocess", NULL);
	g_assert_no_error (error);
	g_assert_nonnull (reader);
	g_autofree char *reader_output = NULL;
	g_autofree char *reader_error = NULL;
	g_assert_true (g_subprocess_communicate_utf8 (reader, NULL, NULL,
		&reader_output, &reader_error, &error));
	g_assert_no_error (error);
	if (!g_subprocess_get_successful (reader))
		g_error ("Fresh Nemo preview process failed: %s\n%s", reader_output, reader_error);

	preview = g_object_ref_sink (g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->media_ready);
	g_assert_cmpint (media_position (preview), ==, 5 * GST_SECOND);
	nemo_quick_preview_dismiss (preview);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_REMEMBER_DOCUMENT_POSITION, FALSE);
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->media_ready);
	g_assert_cmpint (media_position (preview), ==, 0);
	nemo_quick_preview_dismiss (preview);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_REMEMBER_DOCUMENT_POSITION, TRUE);
	g_assert_true (nemo_media_position_lookup (file, &saved));
	g_assert_cmpint (saved, ==, 5 * GST_SECOND);
	g_autoptr (GFileInfo) info = g_file_query_info (file, G_FILE_ATTRIBUTE_TIME_MODIFIED,
		G_FILE_QUERY_INFO_NONE, NULL, NULL);
	g_assert_nonnull (info);
	g_assert_true (g_file_set_attribute_uint64 (file, G_FILE_ATTRIBUTE_TIME_MODIFIED,
		g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED) + 10,
		G_FILE_QUERY_INFO_NONE, NULL, NULL));
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->media_ready);
	g_assert_cmpint (media_position (preview), ==, 0);
	g_assert_false (nemo_media_position_lookup (file, &saved));
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	remove_fixture (file);
	remove_fixture (other);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
test_media_fractional_frame_number (void)
{
	g_autofree char *path = preview_media_encode_fixture (
		"videotestsrc num-buffers=90 ! video/x-raw,width=128,height=72,framerate=30000/1001 ! "
		"vp8enc deadline=1 keyframe-max-dist=1 ! webmmux ! filesink name=output", "webm");
	GFile *file = g_file_new_for_path (path);
	NemoQuickPreview *preview = g_object_ref_sink (g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->media_ready && preview->frame_surface != NULL);
	media_seek_to (preview, 1001 * GST_MSECOND);
	seek_position_update_cb (preview);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (preview->time_label)), ==,
		"00:00:01:00  Frame 30");
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	remove_fixture (file);
}

static void
test_media_fullscreen_controls (void)
{
	g_autofree char *path = preview_media_fixture (128, 72);
	GFile *file = g_file_new_for_path (path);
	NemoQuickPreview *preview = g_object_ref_sink (g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->media_ready && gtk_widget_get_mapped (preview->media_overlay));
	g_assert_true (gtk_revealer_get_reveal_child (GTK_REVEALER (preview->transport_revealer)));
	g_assert_true (gtk_widget_get_parent (preview->transport_revealer) == preview->media_box);
	/* Xvfb has no window manager: deliver the actual GTK window-state signal. */
	GdkEventWindowState state = { .type = GDK_WINDOW_STATE,
		.changed_mask = GDK_WINDOW_STATE_FULLSCREEN, .new_window_state = GDK_WINDOW_STATE_FULLSCREEN };
	gboolean handled;
	g_signal_emit_by_name (preview, "window-state-event", &state, &handled);
	iterate_for (30);
	g_assert_false (gtk_revealer_get_reveal_child (GTK_REVEALER (preview->transport_revealer)));
	g_assert_true (gtk_widget_get_parent (preview->transport_revealer) == preview->media_overlay);
	g_assert_false (gtk_widget_get_visible (preview->filename_label));
	gint x, y, origin_x, origin_y;
	g_assert_true (gtk_widget_translate_coordinates (preview->media_overlay,
		GTK_WIDGET (preview), 0, 0, &x, &y));
	gdk_window_get_origin (gtk_widget_get_window (GTK_WIDGET (preview)), &origin_x, &origin_y);
	GdkEventMotion motion = { .type = GDK_MOTION_NOTIFY,
		.x_root = origin_x + x + gtk_widget_get_allocated_width (preview->media_overlay) / 2,
		.y_root = origin_y + y + gtk_widget_get_allocated_height (preview->media_overlay) - 2 };
	g_signal_emit_by_name (preview->video_area, "motion-notify-event", &motion, &handled);
	g_assert_true (gtk_revealer_get_reveal_child (GTK_REVEALER (preview->transport_revealer)));
	iterate_for (30);
	motion.y_root -= 32;
	g_signal_emit_by_name (preview, "motion-notify-event", &motion, &handled);
	g_assert_true (gtk_revealer_get_reveal_child (GTK_REVEALER (preview->transport_revealer)));
	motion.y_root = origin_y + y + 5;
	g_signal_emit_by_name (preview->video_area, "motion-notify-event", &motion, &handled);
	g_assert_false (gtk_revealer_get_reveal_child (GTK_REVEALER (preview->transport_revealer)));
	media_seek_to (preview, 1800 * GST_SECOND);
	state.new_window_state = 0;
	g_signal_emit_by_name (preview, "window-state-event", &state, &handled);
	g_assert_true (gtk_revealer_get_reveal_child (GTK_REVEALER (preview->transport_revealer)));
	g_assert_true (gtk_widget_get_parent (preview->transport_revealer) == preview->media_box);
	g_assert_true (gtk_widget_get_visible (preview->filename_label));
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	remove_fixture (file);
}

static void
assert_transport_theme (NemoQuickPreview *preview, guint32 background, const char *foreground)
{
	GtkWidget *events = gtk_bin_get_child (GTK_BIN (preview->transport_revealer));
	GtkWidget *transport = gtk_bin_get_child (GTK_BIN (events));
	g_assert_false (gtk_event_box_get_visible_window (GTK_EVENT_BOX (events)));
	iterate_for (100);
	gtk_container_check_resize (GTK_CONTAINER (preview));
	gint width = gtk_widget_get_allocated_width (transport);
	gint height = gtk_widget_get_allocated_height (transport);
	g_assert_cmpint (width, >, 10);
	g_assert_cmpint (height, >, 10);
	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
	cairo_t *cr = cairo_create (surface);
	gtk_widget_draw (transport, cr);
	cairo_destroy (cr);
	cairo_surface_flush (surface);
	guint32 pixel;
	memcpy (&pixel, cairo_image_surface_get_data (surface) +
		(height - 2) * cairo_image_surface_get_stride (surface) + 2 * 4, sizeof pixel);
	g_assert_cmphex (pixel, ==, background);
	cairo_surface_destroy (surface);
	GdkRGBA actual, expected;
	g_assert_true (gdk_rgba_parse (&expected, foreground));
	gtk_style_context_get_color (gtk_widget_get_style_context (preview->time_label),
		GTK_STATE_FLAG_NORMAL, &actual);
	g_assert_true (gdk_rgba_equal (&actual, &expected));
}

static void
test_media_transport_theme (void)
{
	g_autoptr (GtkCssProvider) provider = gtk_css_provider_new ();
	g_autoptr (GError) error = NULL;
	g_assert_true (gtk_css_provider_load_from_data (provider,
		".nemo-quick-preview .background { background-color: white; color: black; }"
		".nemo-quick-preview box.toolbar { background-color: #2d353b;"
		"background-image: none; color: #d3c6aa; transition: none; }", -1, &error));
	g_assert_no_error (error);
	gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
		GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
	g_autofree char *path = preview_media_fixture (128, 72);
	GFile *file = g_file_new_for_path (path);
	NemoQuickPreview *preview = g_object_ref_sink (g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->media_ready);
	assert_transport_theme (preview, 0xff2d353b, "#d3c6aa");
	GdkEventWindowState state = { .type = GDK_WINDOW_STATE,
		.changed_mask = GDK_WINDOW_STATE_FULLSCREEN, .new_window_state = GDK_WINDOW_STATE_FULLSCREEN };
	gboolean handled;
	g_signal_emit_by_name (preview, "window-state-event", &state, &handled);
	gtk_revealer_set_reveal_child (GTK_REVEALER (preview->transport_revealer), TRUE);
	assert_transport_theme (preview, 0xff2d353b, "#d3c6aa");
	g_assert_true (gtk_css_provider_load_from_data (provider,
		".nemo-quick-preview .background { background-color: white; color: black; }"
		".nemo-quick-preview box.toolbar { background-color: #eee8d5;"
		"background-image: none; color: #073642; transition: none; }", -1, &error));
	g_assert_no_error (error);
	gtk_style_context_reset_widgets (gdk_screen_get_default ());
	assert_transport_theme (preview, 0xffeee8d5, "#073642");
	state.new_window_state = 0;
	g_signal_emit_by_name (preview, "window-state-event", &state, &handled);
	assert_transport_theme (preview, 0xffeee8d5, "#073642");
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	gtk_style_context_remove_provider_for_screen (gdk_screen_get_default (), GTK_STYLE_PROVIDER (provider));
	remove_fixture (file);
}

static void
test_media_frame_metadata (void)
{
	GstCaps *caps = gst_caps_from_string (
		"video/x-raw,format=BGRx,width=2,height=2,framerate=25/1");
	GstBuffer *buffer = gst_buffer_new_allocate (NULL, 40, NULL);
	gsize offsets[GST_VIDEO_MAX_PLANES] = { 8 };
	gint strides[GST_VIDEO_MAX_PLANES] = { 16 };
	gst_buffer_memset (buffer, 0, 0, 40);
	gst_buffer_memset (buffer, 8, 0x44, 8);
	gst_buffer_memset (buffer, 24, 0x88, 8);
	g_assert_nonnull (gst_buffer_add_video_meta_full (
		buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_BGRx, 2, 2, 1, offsets, strides));
	GstSample *sample = gst_sample_new (buffer, caps, NULL, NULL);
	GstVideoInfo info;
	GError *error = NULL;
	cairo_surface_t *surface = nemo_preview_media_render_frame (sample, &info, &error);
	g_assert_no_error (error);
	g_assert_nonnull (surface);
	const guint8 *pixels = cairo_image_surface_get_data (surface);
	int stride = cairo_image_surface_get_stride (surface);
	for (int column = 0; column < 8; column++) {
		g_assert_cmphex (pixels[column], ==, 0x44);
		g_assert_cmphex (pixels[stride + column], ==, 0x88);
	}
	cairo_surface_destroy (surface);
	gst_sample_unref (sample);
	gst_buffer_unref (buffer);
	buffer = gst_buffer_new_allocate (NULL, 1, NULL);
	sample = gst_sample_new (buffer, caps, NULL, NULL);
	g_assert_null (nemo_preview_media_render_frame (sample, &info, &error));
	g_assert_error (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_READ);
	g_clear_error (&error);
	gst_sample_unref (sample);
	gst_buffer_unref (buffer);
	gst_caps_unref (caps);
}

static void
test_media_aspect_ratio (void)
{
	const struct {
		int width, height, par_n, par_d, output_width, output_height;
	} cases[] = {
		{ 2160, 3840, 1, 1, 608, 1080 },
		{ 3840, 1600, 1, 1, 1920, 800 },
		{ 720, 576, 16, 15, 768, 576 },
		{ 128, 72, 1, 1, 128, 72 },
	};
	for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
		g_autofree char *description = g_strdup_printf (
			"videotestsrc num-buffers=1 pattern=red ! "
			"video/x-raw,width=%d,height=%d,pixel-aspect-ratio=%d/%d ! "
			"videoconvert ! videoscale",
			cases[i].width, cases[i].height, cases[i].par_n, cases[i].par_d);
		GError *error = NULL;
		GstElement *source = gst_parse_bin_from_description (description, TRUE, &error);
		g_assert_no_error (error);
		GstElement *sink = nemo_preview_media_create_sink (&error);
		g_assert_no_error (error);
		GstElement *pipeline = gst_pipeline_new (NULL);
		gst_bin_add_many (GST_BIN (pipeline), source, sink, NULL);
		g_assert_true (gst_element_link (source, sink));
		g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PAUSED),
				 !=, GST_STATE_CHANGE_FAILURE);
		g_assert_cmpint (gst_element_get_state (pipeline, NULL, NULL, 5 * GST_SECOND),
				 ==, GST_STATE_CHANGE_SUCCESS);
		GstSample *sample = gst_app_sink_pull_preroll (GST_APP_SINK (sink));
		g_assert_nonnull (sample);
		GstVideoInfo info;
		cairo_surface_t *surface = nemo_preview_media_render_frame (sample, &info, &error);
		g_assert_no_error (error);
		g_assert_nonnull (surface);
		g_assert_cmpint (cairo_image_surface_get_width (surface), ==, cases[i].output_width);
		g_assert_cmpint (cairo_image_surface_get_height (surface), ==, cases[i].output_height);
		g_assert_cmpint (GST_VIDEO_INFO_PAR_N (&info), ==, 1);
		g_assert_cmpint (GST_VIDEO_INFO_PAR_D (&info), ==, 1);
		gboolean retain_sample;
		guint max_buffers;
		g_object_get (sink, "enable-last-sample", &retain_sample, "max-buffers", &max_buffers, NULL);
		g_assert_false (retain_sample);
		g_assert_cmpuint (max_buffers, ==, 1);
		cairo_surface_destroy (surface);
		gst_sample_unref (sample);
		gst_element_set_state (pipeline, GST_STATE_NULL);
		gst_object_unref (pipeline);
	}
}

static void
test_media_optional_decoders (void)
{
	GstElement *source = gst_pipeline_new (NULL);
	const char *types[] = { "audio/x-opus", "video/x-vp8", "text/x-raw" };

	for (guint i = 0; i < G_N_ELEMENTS (types); i++) {
		GstCaps *caps = gst_caps_from_string (types[i]);
		GstMessage *message = gst_missing_decoder_message_new (source, caps);
		g_autofree char *video_error = nemo_preview_media_missing_plugin (message, FALSE);
		g_autofree char *audio_error = nemo_preview_media_missing_plugin (message, TRUE);
		g_assert_cmpint (video_error != NULL, ==, i == 1);
		g_assert_cmpint (audio_error != NULL, ==, i == 0);
		gst_message_unref (message);
		gst_caps_unref (caps);
	}
	gst_object_unref (source);
}
#endif

static void
test_optional_hidden_attribute (void)
{
	GFileInfo *info = g_file_info_new ();

	g_file_info_set_name (info, "member.txt");
	g_assert_false (preview_file_info_is_hidden (info));
	g_file_info_set_name (info, ".hidden");
	g_assert_true (preview_file_info_is_hidden (info));
	g_file_info_set_is_hidden (info, FALSE);
	g_assert_false (preview_file_info_is_hidden (info));
	g_file_info_set_name (info, "member.txt");
	g_file_info_set_is_hidden (info, TRUE);
	g_assert_true (preview_file_info_is_hidden (info));
	g_object_unref (info);
}

static void
test_optional_file_information (void)
{
	char *directory = g_dir_make_tmp ("nemo-preview-info-XXXXXX", NULL);
	char *path = g_build_filename (directory, "unknown.dat", NULL);
	GFile *file = g_file_new_for_path (path);
	GFileInfo *info = g_file_info_new ();
	NemoQuickPreview *preview = g_object_ref_sink (
		g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));

	g_assert_true (g_file_set_contents (path, "\0\1\2", 3, NULL));
	g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);
	show_file_content (preview, file, info);
	g_assert_cmpint (preview->mode, ==, PREVIEW_HEX);
	g_assert_cmpint (nemo_paged_viewer_get_mode (preview->paged_viewer), ==, NEMO_VIEWER_MODE_HEX);
	g_assert_cmpstr (gtk_header_bar_get_subtitle (GTK_HEADER_BAR (preview->header_bar)),
			 ==, "Size unknown — unknown");
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	g_object_unref (info);
	remove_fixture (file);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (path);
	g_free (directory);
}

static void
test_parent_entry (void)
{
	NemoQuickPreview *preview = g_object_ref_sink (
		g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	NemoFile *parent = nemo_file_new_parent_entry ();
	GFile *location = nemo_file_get_location (parent);
	nemo_quick_preview_show_file (preview, location, NULL);
	g_assert_null (preview->current_file);
	g_assert_false (gtk_widget_get_visible (GTK_WIDGET (preview)));
	g_object_unref (location);
	nemo_file_unref (parent);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
}

static void
complete_preview_delete (gboolean cancelled, gboolean remove)
{
	g_assert_nonnull (delete_callback);
	if (remove)
		g_assert_true (g_file_delete (delete_target, NULL, NULL));
	NemoDeleteCallback callback = delete_callback;
	gpointer data = delete_callback_data;
	delete_callback = NULL;
	delete_callback_data = NULL;
	callback (NULL, cancelled, data);
	g_clear_object (&delete_target);
}

static void
test_quick_delete (void)
{
	char *directory = g_dir_make_tmp ("nemo-preview-delete-XXXXXX", NULL);
	GFile *first = write_image (directory, "a.png", 0xff0000ff);
	GFile *second = write_image (directory, "b.png", 0x00ff00ff);
	GFile *third = write_image (directory, "c.png", 0x0000ffff);
	NemoQuickPreview *preview = g_object_ref_sink (g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	nemo_quick_preview_show_file (preview, first, NULL);
	WAIT_UNTIL (preview->mode == PREVIEW_IMAGE && preview->dir_files != NULL);
	GdkEventKey key = { .keyval = GDK_KEY_Delete };
	delete_calls = 0;
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	g_assert_true (g_file_equal (delete_target, first));
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	g_assert_cmpuint (delete_calls, ==, 1);
	navigate_to_offset (preview, 1);
	g_assert_true (g_file_equal (preview->current_file, first));
	complete_preview_delete (TRUE, FALSE);
	g_assert_null (preview->delete_request);
	g_assert_true (g_file_query_exists (first, NULL));
	/* A skipped/failed operation must not remove a file from navigation. */
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	complete_preview_delete (FALSE, FALSE);
	WAIT_UNTIL (preview->delete_request == NULL);
	g_assert_true (g_file_equal (preview->current_file, first));
	g_assert_cmpuint (preview->dir_files->len, ==, 3);
	/* Search entry Delete edits text rather than deleting the preview file. */
	gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (preview->search_bar), TRUE);
	gtk_widget_grab_focus (preview->search_entry);
	g_assert_false (on_key_press (GTK_WIDGET (preview), &key, NULL));
	g_assert_null (preview->delete_request);
	gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (preview->search_bar), FALSE);
	gtk_widget_grab_focus (preview->next_button);
	key.state = GDK_SHIFT_MASK;
	g_assert_false (on_key_press (GTK_WIDGET (preview), &key, NULL));
	key.state = 0;
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	complete_preview_delete (FALSE, TRUE);
	WAIT_UNTIL (preview->delete_request == NULL && g_file_equal (preview->current_file, second));
	g_assert_cmpuint (preview->dir_files->len, ==, 2);
	navigate_to_offset (preview, 1);
	g_assert_true (g_file_equal (preview->current_file, third));
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	complete_preview_delete (FALSE, TRUE);
	WAIT_UNTIL (preview->delete_request == NULL && g_file_equal (preview->current_file, second));
	g_assert_cmpuint (preview->dir_files->len, ==, 1);
	g_assert_true (on_key_press (GTK_WIDGET (preview), &key, NULL));
	complete_preview_delete (FALSE, TRUE);
	WAIT_UNTIL (!gtk_widget_get_visible (GTK_WIDGET (preview)));
	g_assert_null (preview->current_file);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	g_object_unref (first);
	g_object_unref (second);
	g_object_unref (third);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (directory);
}

static void
test_quick_delete_stale (void)
{
	char *directory = g_dir_make_tmp ("nemo-preview-delete-stale-XXXXXX", NULL);
	GFile *first = write_image (directory, "a.png", 0xff0000ff);
	GFile *second = write_image (directory, "b.png", 0x00ff00ff);
	NemoQuickPreview *preview = g_object_ref_sink (g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	nemo_quick_preview_show_file (preview, first, NULL);
	WAIT_UNTIL (preview->dir_files != NULL);
	preview_delete_current_file (preview);
	nemo_quick_preview_dismiss (preview);
	nemo_quick_preview_show_file (preview, second, NULL);
	complete_preview_delete (FALSE, TRUE);
	iterate_for (50);
	g_assert_true (g_file_equal (preview->current_file, second));
	preview_delete_current_file (preview);
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	complete_preview_delete (TRUE, FALSE);
	remove_fixture (second);
	g_object_unref (first);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (directory);
}

static void
test_quick_delete_real_trash (void)
{
	char *directory = g_dir_make_tmp ("nemo-preview-real-trash-XXXXXX", NULL);
	GFile *file = write_image (directory, "cleanup.png", 0xff0000ff);
	NemoQuickPreview *preview = g_object_ref_sink (g_object_new (NEMO_TYPE_QUICK_PREVIEW, NULL));
	nemo_quick_preview_show_file (preview, file, NULL);
	WAIT_UNTIL (preview->dir_files != NULL);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_CONFIRM_TRASH, FALSE);
	real_preview_delete = TRUE;
	preview_delete_current_file (preview);
	WAIT_UNTIL (!gtk_widget_get_visible (GTK_WIDGET (preview)));
	real_preview_delete = FALSE;
	g_assert_false (g_file_query_exists (file, NULL));
	g_autofree char *trashed_path = g_build_filename (g_get_user_data_dir (), "Trash", "files", "cleanup.png", NULL);
	g_assert_true (g_file_test (trashed_path, G_FILE_TEST_IS_REGULAR));
	gtk_widget_destroy (GTK_WIDGET (preview));
	g_object_unref (preview);
	g_object_unref (file);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (directory);
}

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
	test_program = argv[0];
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
	g_test_add_func ("/preview/quick/delete-navigation-cancel-failure", test_quick_delete);
	g_test_add_func ("/preview/quick/delete-stale-destroy", test_quick_delete_stale);
	g_test_add_func ("/preview/quick/delete-real-trash", test_quick_delete_real_trash);
	g_test_add_func ("/preview/quick/navigation-only-parent", test_parent_entry);
	g_test_add_func ("/preview/quick/optional-hidden-attribute", test_optional_hidden_attribute);
	g_test_add_func ("/preview/quick/optional-file-information", test_optional_file_information);
	g_test_add_func ("/preview/quick/filename-image-information", test_quick_image_information);
	g_test_add_func ("/preview/quick/text-search-hex-folder", test_quick_text_search_and_modes);
	g_test_add_func ("/preview/quick/page-and-file-shortcuts", test_quick_page_keys);
#ifdef HAVE_DOCUMENT_PREVIEW
	g_test_add_func ("/preview/quick/rich-document", test_quick_rich_document);
#endif
#ifdef HAVE_GSTREAMER
	g_test_add_func ("/preview/media/bounded-delayed-stop", test_bounded_media_stop);
	g_test_add_func ("/preview/media/quick-close-loading", test_quick_close_while_media_loading);
	g_test_add_func ("/preview/media/start-error", test_media_start_error);
	g_test_add_func ("/preview/media/push-failure", test_media_push_failure);
	g_test_add_func ("/preview/media/frame-lifetime", test_media_frame_lifetime);
	g_test_add_func ("/preview/media/long-4k-webm", test_media_long_webm);
	g_test_add_func ("/preview/media/missing-video-decoder", test_media_missing_video);
	g_test_add_func ("/preview/media/loading-timeout", test_media_loading_timeout);
	g_test_add_func ("/preview/media/audio-only", test_media_audio_only);
	g_test_add_data_func ("/preview/media/resume-video", GINT_TO_POINTER (FALSE), test_media_resume);
	g_test_add_data_func ("/preview/media/resume-audio", GINT_TO_POINTER (TRUE), test_media_resume);
	g_test_add_func ("/preview/media/fullscreen-hover-controls", test_media_fullscreen_controls);
	g_test_add_func ("/preview/media/transport-theme", test_media_transport_theme);
	g_test_add_func ("/preview/media/fractional-frame-number", test_media_fractional_frame_number);
	g_test_add_func ("/preview/media/frame-metadata", test_media_frame_metadata);
	g_test_add_func ("/preview/media/scaled-aspect-ratio", test_media_aspect_ratio);
	g_test_add_func ("/preview/media/optional-decoders", test_media_optional_decoders);
#endif
	int result = g_test_run ();
	g_source_remove (tick);
	return result;
}
