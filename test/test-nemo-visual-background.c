/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <gtk/gtk.h>
#include <gst/gst.h>
#include "../src/nemo-media-effects.c"

#define WAIT_FOR(condition) G_STMT_START { \
	gint64 deadline = g_get_monotonic_time () + 8000000; \
	while (!(condition) && g_get_monotonic_time () < deadline) { \
		while (g_main_context_iteration (NULL, FALSE)); \
		g_usleep (1000); \
	} \
	g_assert_true (condition); \
} G_STMT_END

static void
drain_for (gint64 microseconds)
{
	gint64 deadline = g_get_monotonic_time () + microseconds;
	while (g_get_monotonic_time () < deadline) {
		while (g_main_context_iteration (NULL, FALSE));
		g_usleep (1000);
	}
}

static void
sampling_changed (GObject *object, GParamSpec *spec, gpointer data)
{
	nemo_media_audio_set_visuals (data,
		nemo_media_visualizer_get_active (NEMO_MEDIA_VISUALIZER (object)));
}

static gboolean
audio_message (GstBus *bus, GstMessage *message, gpointer data)
{
	if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
		GError *error = NULL;
		gst_message_parse_error (message, &error, NULL);
		g_error ("Background playback: %s", error->message);
	}
	if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ELEMENT)
		nemo_media_visualizer_handle_message (data, message);
	return G_SOURCE_CONTINUE;
}

static GtkWidget *
make_window (int x, int y, int width, int height)
{
	GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	gtk_window_move (GTK_WINDOW (window), x, y);
	gtk_window_set_default_size (GTK_WINDOW (window), width, height);
	return window;
}

static void
test_rendering_and_audio (void)
{
	GError *error = NULL;
	GstElement *pipeline = gst_parse_launch (
		"audiotestsrc is-live=true ! audioconvert ! identity name=audio", &error);
	g_assert_no_error (error);
	GstElement *filter = nemo_media_audio_filter_new (&error);
	g_assert_no_error (error);
	GstElement *sink = gst_element_factory_make ("fakesink", NULL);
	g_object_set (sink, "sync", TRUE, NULL);
	gst_bin_add_many (GST_BIN (pipeline), filter, sink, NULL);
	GstElement *audio = gst_bin_get_by_name (GST_BIN (pipeline), "audio");
	g_assert_true (gst_element_link_many (audio, filter, sink, NULL));
	gst_object_unref (audio);
	GstBus *bus = gst_element_get_bus (pipeline);
	GtkWidget *window = make_window (40, 40, 320, 240);
	NemoMediaVisualizer *visual = NEMO_MEDIA_VISUALIZER (nemo_media_visualizer_new ());
	nemo_media_visualizer_set_preset (visual, 18);
	g_signal_connect (visual, "notify::render-active", G_CALLBACK (sampling_changed), filter);
	gtk_container_add (GTK_CONTAINER (window), GTK_WIDGET (visual));
	guint watch = gst_bus_add_watch (bus, audio_message, visual);
	gtk_widget_show_all (window);
	g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
	nemo_media_visualizer_set_playing (visual, TRUE);
	WAIT_FOR (visual->rendered_frames >= 4 && visual->last_pcm > 0);

	gtk_widget_hide (window);
	WAIT_FOR (!visual->rendering && !visual->render_active);
	guint frames = visual->rendered_frames;
	drain_for (150000);
	gint64 pcm = visual->last_pcm;
	drain_for (200000);
	g_assert_cmpuint (visual->rendered_frames, ==, frames);
	g_assert_cmpint (visual->last_pcm, ==, pcm);
	g_assert_cmpuint (visual->tick, ==, 0);
	gtk_widget_show (window);
	WAIT_FOR (visual->rendered_frames > frames + 2 && visual->render_active);

	GtkWidget *cover = make_window (20, 20, 400, 340);
	gboolean wayland = FALSE;
#ifdef GDK_WINDOWING_WAYLAND
	wayland = GDK_IS_WAYLAND_DISPLAY (gtk_widget_get_display (window));
#endif
	if (wayland)
		gtk_window_fullscreen (GTK_WINDOW (cover));
	gtk_widget_show_all (cover);
	gtk_window_present (GTK_WINDOW (cover));
	WAIT_FOR (!visual->render_active && !visual->rendering);
	frames = visual->rendered_frames;
	drain_for (150000);
	pcm = visual->last_pcm;
	gint64 before = -1, after = -1;
	g_assert_true (gst_element_query_position (pipeline, GST_FORMAT_TIME, &before));
	drain_for (250000);
	g_assert_true (gst_element_query_position (pipeline, GST_FORMAT_TIME, &after));
	g_assert_cmpint (after - before, >, 150 * GST_MSECOND);
	g_assert_cmpuint (visual->rendered_frames, ==, frames);
	g_assert_cmpint (visual->last_pcm, ==, pcm);
	if (wayland) {
		g_assert_true (visual->compositor_idle);
		gtk_widget_hide (cover);
		WAIT_FOR (visual->render_active && visual->rendered_frames > frames + 2);
		gtk_widget_show (cover);
		gtk_window_present (GTK_WINDOW (cover));
		WAIT_FOR (!visual->render_active && !visual->rendering);
		nemo_media_visualizer_set_playing (visual, FALSE);
		frames = visual->rendered_frames;
		nemo_media_visualizer_set_preset (visual, 20);
		gtk_widget_hide (cover);
		WAIT_FOR (!visual->compositor_idle && visual->rendered_frames > frames);
		g_assert_false (visual->render_active);
		g_assert_cmpuint (visual->tick, ==, 0);
	} else {
		g_assert_cmpuint (visual->tick, ==, 0);
		gtk_window_move (GTK_WINDOW (cover), 300, 40);
		WAIT_FOR (visual->render_active && visual->rendered_frames > frames + 2);
		frames = visual->rendered_frames;
		gtk_window_present (GTK_WINDOW (cover));
		WAIT_FOR (visual->rendered_frames > frames + 2);
		g_assert_true (nemo_media_visualizer_get_active (visual));
	}
	gtk_widget_destroy (cover);
	gtk_widget_destroy (window);
	gst_element_set_state (pipeline, GST_STATE_NULL);
	g_source_remove (watch);
	gst_object_unref (bus);
	gst_object_unref (pipeline);
	drain_for (150000);
}

#ifdef GDK_WINDOWING_X11
static void
test_composited_coverage (void)
{
	GtkWidget *window = make_window (50, 50, 320, 240);
	NemoMediaVisualizer *visual = NEMO_MEDIA_VISUALIZER (nemo_media_visualizer_new ());
	nemo_media_visualizer_set_preset (visual, 18);
	gtk_container_add (GTK_CONTAINER (window), GTK_WIDGET (visual));
	gtk_widget_show_all (window);
	GtkWidget *first = make_window (50, 50, 160, 240);
	GtkWidget *second = make_window (210, 50, 160, 240);
	gtk_widget_show_all (first);
	gtk_widget_show_all (second);
	drain_for (150000);
	GdkDisplay *gdk_display = gtk_widget_get_display (window);
	Display *display = gdk_x11_display_get_xdisplay (gdk_display);
	Window root = gdk_x11_window_get_xid (gdk_screen_get_root_window (gtk_widget_get_screen (window)));
	Window windows[] = { GDK_WINDOW_XID (gtk_widget_get_window (window)),
		GDK_WINDOW_XID (gtk_widget_get_window (first)), GDK_WINDOW_XID (gtk_widget_get_window (second)) };
	Atom stacking = XInternAtom (display, "_NET_CLIENT_LIST_STACKING", False);
	Atom opacity = XInternAtom (display, "_NET_WM_WINDOW_OPACITY", False);
	Atom current = XInternAtom (display, "_NET_CURRENT_DESKTOP", False);
	Atom desktop = XInternAtom (display, "_NET_WM_DESKTOP", False);
	XChangeProperty (display, root, stacking, XA_WINDOW, 32, PropModeReplace,
		(unsigned char *) windows, G_N_ELEMENTS (windows));
	g_assert_true (visual_x11_is_obscured (visual));

	unsigned long half = G_MAXUINT32 / 2;
	XChangeProperty (display, windows[1], opacity, XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *) &half, 1);
	g_assert_false (visual_x11_is_obscured (visual));
	XDeleteProperty (display, windows[1], opacity);

	XRectangle hole[] = { {0, 0, 160, 60}, {0, 180, 160, 60} };
	XShapeCombineRectangles (display, windows[1], ShapeBounding, 0, 0,
		hole, G_N_ELEMENTS (hole), ShapeSet, Unsorted);
	g_assert_false (visual_x11_is_obscured (visual));
	XShapeCombineMask (display, windows[1], ShapeBounding, 0, 0, None, ShapeSet);
	g_assert_true (visual_x11_is_obscured (visual));

	unsigned long workspace = 2, different = 3;
	XChangeProperty (display, root, current, XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *) &workspace, 1);
	XChangeProperty (display, windows[1], desktop, XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *) &different, 1);
	g_assert_false (visual_x11_is_obscured (visual));
	XChangeProperty (display, windows[0], desktop, XA_CARDINAL, 32, PropModeReplace,
		(unsigned char *) &different, 1);
	g_assert_true (visual_x11_is_obscured (visual));
	XDeleteProperty (display, root, stacking);
	XDeleteProperty (display, root, current);
	gtk_widget_destroy (second);
	gtk_widget_destroy (first);
	gtk_widget_destroy (window);
	drain_for (150000);
}
#endif

int
main (int argc, char **argv)
{
	if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
		return 77;
	gtk_test_init (&argc, &argv, NULL);
	gst_init (&argc, &argv);
	g_test_add_func ("/visuals/background/rendering-and-audio", test_rendering_and_audio);
#ifdef GDK_WINDOWING_X11
	if (GDK_IS_X11_DISPLAY (gdk_display_get_default ()))
		g_test_add_func ("/visuals/background/composited-coverage", test_composited_coverage);
#endif
	int result = g_test_run ();
	visual_application_shutdown (NULL, NULL);
	return result;
}
