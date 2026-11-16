/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <gtk/gtk.h>
#include <gst/gst.h>
#include <glib/gstdio.h>
#include "../src/nemo-media-effects.c"

static guint
preset_index (const char *id)
{
	for (guint i = 0; i < nemo_media_visualizer_get_n_presets (); i++)
		if (g_str_equal (id, nemo_media_visualizer_get_preset_id (i)))
			return i;
	g_error ("Missing flight visualization: %s", id);
}

static cairo_surface_t *
render_frame (NemoMediaVisualizer *visual, guint preset, guint32 seed, double time)
{
	VisualFrame frame = { .preset = preset, .scene_seed = seed, .scene_time = time, .energy = 0.3 };
	for (guint i = 0; i < G_N_ELEMENTS (frame.pcm); i++)
		frame.pcm[i] = 0.3 * sin (2 * G_PI * 440 * (i / 2) / 44100.0);
	GTask *task = g_task_new (visual, NULL, NULL, NULL);
	gint64 start = g_get_monotonic_time ();
	visual_render_worker (task, visual, &frame, NULL);
	GError *error = NULL;
	cairo_surface_t *surface = g_task_propagate_pointer (task, &error);
	g_assert_no_error (error);
	g_assert_nonnull (surface);
	g_object_unref (task);
	g_test_message ("%s seed %u time %.1f: %.1f ms",
		nemo_media_visualizer_get_preset_id (preset), seed, time,
		(g_get_monotonic_time () - start) / 1000.0);
	return surface;
}

static double
frame_difference (cairo_surface_t *a, cairo_surface_t *b)
{
	guint32 *first = (guint32 *) cairo_image_surface_get_data (a);
	guint32 *second = (guint32 *) cairo_image_surface_get_data (b);
	guint count = cairo_image_surface_get_width (a) * cairo_image_surface_get_height (a);
	g_assert_cmpint (cairo_image_surface_get_width (a), ==, cairo_image_surface_get_width (b));
	g_assert_cmpint (cairo_image_surface_get_height (a), ==, cairo_image_surface_get_height (b));
	double difference = 0;
	for (guint i = 0; i < count; i++)
		for (guint c = 0; c < 3; c++)
			difference += abs ((int) ((first[i] >> (c * 8)) & 255) -
				(int) ((second[i] >> (c * 8)) & 255));
	return difference / (count * 3);
}

static void
assert_scene (cairo_surface_t *surface, gboolean terrain)
{
	guint32 *pixels = (guint32 *) cairo_image_surface_get_data (surface);
	gboolean colors[4096] = { FALSE };
	guint distinct = 0, dark = 0, sky = 0, land = 0;
	guint count = cairo_image_surface_get_width (surface) * cairo_image_surface_get_height (surface);
	for (guint i = 0; i < count; i++) {
		guint r = (pixels[i] >> 16) & 255, g = (pixels[i] >> 8) & 255, b = pixels[i] & 255;
		guint key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
		if (!colors[key]) {
			colors[key] = TRUE;
			distinct++;
		}
		dark += MAX (r, MAX (g, b)) < 20;
		sky += b > r + 20 && b > g + 10;
		land += r > b + 8 || (g > r + 8 && g > b + 8);
	}
	g_assert_cmpuint (distinct, >, 60);
	if (terrain) {
		g_assert_cmpuint (sky, >, count / 70);
		g_assert_cmpuint (land, >, count / 70);
	} else
		g_assert_cmpuint (dark, >, count / 2);
}

static void
save_scene (cairo_surface_t *surface, const char *name)
{
	const char *directory = g_getenv ("NEMO_TEST_VISUAL_OUTPUT");
	if (directory == NULL)
		return;
	g_autofree char *path = g_build_filename (directory, name, NULL);
	g_assert_cmpint (cairo_surface_write_to_png (surface, path), ==, CAIRO_STATUS_SUCCESS);
}

static void
test_flight_scenes (void)
{
	NemoMediaVisualizer *visual = NEMO_MEDIA_VISUALIZER (nemo_media_visualizer_new ());
	g_object_ref_sink (visual);
	guint space = preset_index ("stellar-voyage"), terrain = preset_index ("earthflight");
	cairo_surface_t *a = render_frame (visual, space, 27317, 20);
	g_assert_true (eglMakeCurrent (visual->gpu.display, visual->gpu.surface,
		visual->gpu.surface, visual->gpu.context));
	g_test_message ("OpenGL renderer: %s", glGetString (GL_RENDERER));
	g_assert_true (eglMakeCurrent (visual->gpu.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
	assert_scene (a, FALSE);
	save_scene (a, "player-renderer-stellar-voyage.png");
	cairo_surface_t *b = render_frame (visual, space, 27317, 24);
	g_assert_cmpfloat (frame_difference (a, b), >, 0.2);
	cairo_surface_t *c = render_frame (visual, space, 999, 20);
	g_assert_cmpfloat (frame_difference (a, c), >, 0.2);
	cairo_surface_destroy (a);
	cairo_surface_destroy (b);
	cairo_surface_destroy (c);
	a = render_frame (visual, terrain, 27317, 35);
	assert_scene (a, TRUE);
	save_scene (a, "player-renderer-earthflight.png");
	b = render_frame (visual, terrain, 27317, 40);
	g_assert_cmpfloat (frame_difference (a, b), >, 1);
	c = render_frame (visual, terrain, 999, 35);
	g_assert_cmpfloat (frame_difference (a, c), >, 1);
	cairo_surface_destroy (b);
	cairo_surface_destroy (c);
	b = render_frame (visual, terrain, 27317, 35);
	g_assert_cmpfloat_with_epsilon (frame_difference (a, b), 0, 0.001);
	cairo_surface_destroy (a);
	cairo_surface_destroy (b);
	gboolean projectm_visible = FALSE;
	for (guint frame = 0; frame < 8; frame++) {
		a = render_frame (visual, 0, 27317, frame / 24.0);
		guint32 *pixels = (guint32 *) cairo_image_surface_get_data (a);
		guint colored = 0;
		for (guint i = 0; i < VISUAL_WIDTH * VISUAL_HEIGHT; i++) {
			guint r = (pixels[i] >> 16) & 255, g = (pixels[i] >> 8) & 255, b = pixels[i] & 255;
			colored += MAX (r, MAX (g, b)) > MIN (r, MIN (g, b)) + 30;
		}
		projectm_visible |= colored > 500;
		cairo_surface_destroy (a);
	}
	g_assert_true (projectm_visible);
	a = render_frame (visual, terrain, 27317, 1024.0 / 95 - 0.0001);
	b = render_frame (visual, terrain, 27317, 1024.0 / 95 + 0.0001);
	g_assert_cmpfloat (frame_difference (a, b), <, 2);
	cairo_surface_destroy (b);
	cairo_surface_destroy (a);
	a = render_frame (visual, terrain, 27317, 10000000);
	assert_scene (a, TRUE);
	cairo_surface_destroy (a);
	gtk_widget_destroy (GTK_WIDGET (visual));
	g_object_unref (visual);
}

int
main (int argc, char **argv)
{
	if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
		return 77;
	gtk_test_init (&argc, &argv, NULL);
	gst_init (&argc, &argv);
	g_test_add_func ("/visuals/flight/frames-and-engine-switching", test_flight_scenes);
	int result = g_test_run ();
	visual_application_shutdown (NULL, NULL);
	return result;
}
