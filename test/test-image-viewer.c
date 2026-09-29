#include <config.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>

static guint generic_decodes;

static GdkPixbufAnimation *
count_generic_decode (GInputStream *stream, GCancellable *cancel, GError **error)
{
	generic_decodes++;
	return gdk_pixbuf_animation_new_from_stream (stream, cancel, error);
}

#define gdk_pixbuf_animation_new_from_stream count_generic_decode
#include "../src/nemo-image-viewer.c"
#undef gdk_pixbuf_animation_new_from_stream
#ifdef HAVE_LIBRAW
#include "preview-fixtures.h"
#endif

static void
iterate_for (guint milliseconds)
{
	gint64 end = g_get_monotonic_time () + milliseconds * 1000;
	while (g_get_monotonic_time () < end) {
		while (g_main_context_iteration (NULL, FALSE));
		g_usleep (1000);
	}
}

static NemoImageViewer *
create_viewer (GtkWidget **window, int width, int height)
{
	*window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	NemoImageViewer *viewer = nemo_image_viewer_new ();
	gtk_container_add (GTK_CONTAINER (*window), GTK_WIDGET (viewer));
	gtk_window_set_default_size (GTK_WINDOW (*window), 640, 480);
	nemo_image_viewer_set_fit (viewer, TRUE);
	GdkPixbuf *pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, width, height);
	gdk_pixbuf_fill (pixbuf, 0xff0000ff);
	GdkPixbufSimpleAnim *animation = gdk_pixbuf_simple_anim_new (width, height, 0);
	gdk_pixbuf_simple_anim_add_frame (animation, pixbuf);
	g_object_unref (pixbuf);
	set_animation (viewer, GDK_PIXBUF_ANIMATION (animation));
	gtk_widget_show_all (*window);
	iterate_for (150);
	return viewer;
}

static void
assert_fit (NemoImageViewer *viewer)
{
	GdkPixbuf *base = get_base_pixbuf (viewer);
	double expected = MIN ((gtk_widget_get_allocated_width (viewer->scroll) - 20.0) /
			       gdk_pixbuf_get_width (base),
			       (gtk_widget_get_allocated_height (viewer->scroll) - 20.0) /
			       gdk_pixbuf_get_height (base));
	g_assert_true (nemo_image_viewer_get_fit (viewer));
	g_assert_true (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (viewer->fit_check)));
	g_assert_cmpfloat_with_epsilon (nemo_image_viewer_get_zoom (viewer), expected, 0.001);
	g_assert_cmpfloat (gdk_pixbuf_get_width (base) * expected, <=,
			  gtk_widget_get_allocated_width (viewer->image));
	g_assert_cmpfloat (gdk_pixbuf_get_height (base) * expected, <=,
			  gtk_widget_get_allocated_height (viewer->image));
}

static void
test_fit_resize (gconstpointer small)
{
	GtkWidget *window;
	NemoImageViewer *viewer = create_viewer (&window,
		GPOINTER_TO_INT (small) ? 16 : 3000, GPOINTER_TO_INT (small) ? 12 : 2000);
	assert_fit (viewer);
	if (GPOINTER_TO_INT (small))
		g_assert_cmpfloat (nemo_image_viewer_get_zoom (viewer), >, 4);

	gtk_window_resize (GTK_WINDOW (window), 900, 600);
	iterate_for (150);
	assert_fit (viewer);
	gtk_window_resize (GTK_WINDOW (window), 400, 700);
	iterate_for (150);
	assert_fit (viewer);

	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
		gtk_widget_get_allocated_width (viewer->image),
		gtk_widget_get_allocated_height (viewer->image));
	cairo_t *cr = cairo_create (surface);
	gtk_widget_draw (viewer->image, cr);
	cairo_destroy (cr);
	cairo_surface_flush (surface);
	guint32 *row = (guint32 *) (cairo_image_surface_get_data (surface) +
		(gtk_widget_get_allocated_height (viewer->image) / 2) *
		cairo_image_surface_get_stride (surface));
	g_assert_cmphex (row[gtk_widget_get_allocated_width (viewer->image) / 2], ==, 0xffff0000);
	cairo_surface_destroy (surface);
	nemo_image_viewer_set_fit (viewer, FALSE);
	double zoom = nemo_image_viewer_get_zoom (viewer);
	gtk_window_resize (GTK_WINDOW (window), 250, 300);
	iterate_for (100);
	g_assert_cmpfloat_with_epsilon (nemo_image_viewer_get_zoom (viewer), zoom, 0.001);
	GtkAdjustment *h = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (viewer->scroll));
	g_assert_cmpfloat (gtk_adjustment_get_upper (h), >, gtk_adjustment_get_page_size (h));
	gtk_widget_destroy (window);
}

static void
test_wheel_zoom_and_pan (void)
{
	GtkWidget *window;
	NemoImageViewer *viewer = create_viewer (&window, 1000, 800);
	GtkAdjustment *h = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (viewer->scroll));
	GtkAdjustment *v = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (viewer->scroll));
	double old_zoom = viewer->zoom_level;
	GdkEventScroll scroll = { .type = GDK_SCROLL, .direction = GDK_SCROLL_UP,
		.x = 280, .y = 200 };
	double image_x = (scroll.x - (gtk_widget_get_allocated_width (viewer->image) -
			1000 * old_zoom) / 2) / old_zoom;
	double image_y = (scroll.y - (gtk_widget_get_allocated_height (viewer->image) -
			800 * old_zoom) / 2) / old_zoom;
	gboolean handled = FALSE;
	g_signal_emit_by_name (viewer->image, "scroll-event", &scroll, &handled);
	g_assert_true (handled);
	iterate_for (150);
	g_assert_false (nemo_image_viewer_get_fit (viewer));
	g_assert_false (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (viewer->fit_check)));
	g_assert_cmpfloat_with_epsilon (viewer->zoom_level, old_zoom * 1.2, 0.001);
	/* Zoom farther until both axes can retain the pointer's image position. */
	scroll.x += gtk_adjustment_get_value (h);
	scroll.y += gtk_adjustment_get_value (v);
	g_signal_emit_by_name (viewer->image, "scroll-event", &scroll, &handled);
	iterate_for (150);
	g_assert_cmpfloat (gtk_adjustment_get_upper (h), >, gtk_adjustment_get_page_size (h));
	g_assert_cmpfloat (gtk_adjustment_get_upper (v), >, gtk_adjustment_get_page_size (v));
	/* Centre coordinates remain stable unless the edge clamp was necessary. */
	g_assert_cmpfloat_with_epsilon (
		(280 + gtk_adjustment_get_value (h) -
		 (gtk_widget_get_allocated_width (viewer->image) - 1000 * viewer->zoom_level) / 2) /
		viewer->zoom_level, image_x, 5);
	g_assert_cmpfloat_with_epsilon (
		(200 + gtk_adjustment_get_value (v) -
		 (gtk_widget_get_allocated_height (viewer->image) - 800 * viewer->zoom_level) / 2) /
		viewer->zoom_level, image_y, 5);

	double before_h = gtk_adjustment_get_value (h), before_v = gtk_adjustment_get_value (v);
	GdkEventButton press = { .type = GDK_BUTTON_PRESS, .button = 1, .x_root = 200, .y_root = 200 };
	g_signal_emit_by_name (viewer->image, "button-press-event", &press, &handled);
	g_assert_true (handled);
	GdkEventMotion motion = { .type = GDK_MOTION_NOTIFY, .x_root = 180, .y_root = 170 };
	g_signal_emit_by_name (viewer->image, "motion-notify-event", &motion, &handled);
	g_assert_true (handled);
	g_assert_cmpfloat_with_epsilon (gtk_adjustment_get_value (h), before_h + 20, 0.001);
	g_assert_cmpfloat_with_epsilon (gtk_adjustment_get_value (v), before_v + 30, 0.001);
	press.type = GDK_BUTTON_RELEASE;
	g_signal_emit_by_name (viewer->image, "button-release-event", &press, &handled);
	g_assert_true (handled);
	g_assert_false (viewer->dragging);

	scroll.direction = GDK_SCROLL_SMOOTH;
	scroll.delta_y = 0.5;
	old_zoom = viewer->zoom_level;
	g_signal_emit_by_name (viewer->image, "scroll-event", &scroll, &handled);
	iterate_for (100);
	g_assert_cmpfloat (viewer->zoom_level, <, old_zoom);

	nemo_image_viewer_set_fit (viewer, TRUE);
	iterate_for (100);
	assert_fit (viewer);
	g_assert_cmpfloat (gtk_adjustment_get_value (h), ==, 0);
	g_assert_cmpfloat (gtk_adjustment_get_value (v), ==, 0);
	gtk_widget_destroy (window);
}

static void
test_controls_and_clear (void)
{
	GtkWidget *window;
	NemoImageViewer *viewer = create_viewer (&window, 1000, 800);
	nemo_image_viewer_set_show_controls (viewer, FALSE);
	gtk_widget_show_all (window);
	g_assert_false (gtk_widget_get_visible (viewer->ctrl_box));
	nemo_image_viewer_set_show_controls (viewer, TRUE);
	g_assert_true (gtk_widget_get_visible (viewer->ctrl_box));
	nemo_image_viewer_set_zoom (viewer, 2);
	iterate_for (100);
	GdkEventButton press = { .button = 1 };
	image_button_press_cb (viewer->image, &press, viewer);
	g_assert_true (viewer->dragging);
	nemo_image_viewer_clear (viewer);
	g_assert_false (viewer->dragging);
	g_assert_null (get_base_pixbuf (viewer));
	gtk_widget_destroy (window);
}

#ifdef HAVE_LIBRAW
static void
test_dng_uses_raw_image (void)
{
	g_autofree char *directory = g_dir_make_tmp ("nemo-dng-XXXXXX", NULL);
	g_autofree char *path = g_build_filename (directory, "image.DNG", NULL);
	write_dng_fixture (path);

	GtkWidget *window;
	NemoImageViewer *viewer = create_viewer (&window, 4, 4);
	GFile *file = g_file_new_for_path (path);
	generic_decodes = 0;
	nemo_image_viewer_load_location (viewer, file);
	gint64 deadline = g_get_monotonic_time () + 5000000;
	while (viewer->original_pixbuf == NULL && g_get_monotonic_time () < deadline)
		iterate_for (10);
	g_assert_nonnull (viewer->original_pixbuf);
	g_assert_cmpuint (generic_decodes, ==, 0);
	g_assert_cmpint (gdk_pixbuf_get_width (viewer->original_pixbuf), ==, 64);
	g_assert_cmpint (gdk_pixbuf_get_height (viewer->original_pixbuf), ==, 48);
	assert_fit (viewer);
	gtk_widget_destroy (window);
	g_object_unref (file);
	g_assert_cmpint (g_remove (path), ==, 0);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}
#endif

int
main (int argc, char **argv)
{
	if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
		return 77;
#ifndef HAVE_LIBRAW
	if (g_strcmp0 (g_getenv ("NEMO_TEST_REQUIRE_RAW"), "1") == 0) {
		g_printerr ("Release image previews require LibRaw and the DNG regression.\n");
		return 1;
	}
#endif
	gtk_test_init (&argc, &argv, NULL);
	g_test_add_data_func ("/image-viewer/fit-large-resize", GINT_TO_POINTER (FALSE), test_fit_resize);
	g_test_add_data_func ("/image-viewer/fit-small-upscale", GINT_TO_POINTER (TRUE), test_fit_resize);
	g_test_add_func ("/image-viewer/wheel-zoom-drag-pan", test_wheel_zoom_and_pan);
	g_test_add_func ("/image-viewer/controls-clear", test_controls_and_clear);
#ifdef HAVE_LIBRAW
	g_test_add_func ("/image-viewer/dng-full-preview", test_dng_uses_raw_image);
#endif
	return g_test_run ();
}
