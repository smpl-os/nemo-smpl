/* Native media effects. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include "nemo-media-effects.h"
#include <gst/base/gstbasetransform.h>
#include <gst/audio/audio-format.h>
#include <glib/gi18n.h>
#include <math.h>

#define VISUAL_BANDS 512
#define TONE_LEVEL 0.01
#define CLOUD_PIXELS (384 * 216)
#define NOISE_SIZE 128

typedef struct {
	GstBaseTransform parent;
	gint enabled;
	gint rate;
	double left_phase, right_phase, gain;
} NemoBinauralFilter;

typedef GstBaseTransformClass NemoBinauralFilterClass;
G_DEFINE_TYPE (NemoBinauralFilter, nemo_binaural_filter, GST_TYPE_BASE_TRANSFORM)

static gboolean
binaural_caps (GstBaseTransform *base, GstCaps *input, GstCaps *output)
{
	NemoBinauralFilter *self = (NemoBinauralFilter *) base;
	return gst_structure_get_int (gst_caps_get_structure (input, 0), "rate", &self->rate) &&
	       self->rate > 0;
}

static GstFlowReturn
binaural_process (GstBaseTransform *base, GstBuffer *buffer)
{
	NemoBinauralFilter *self = (NemoBinauralFilter *) base;
	gboolean enabled = g_atomic_int_get (&self->enabled);
	if (!enabled && self->gain == 0)
		return GST_FLOW_OK;
	GstMapInfo map;
	if (!gst_buffer_map (buffer, &map, GST_MAP_READWRITE))
		return GST_FLOW_ERROR;
	if (map.size % (2 * sizeof (float)) != 0) {
		gst_buffer_unmap (buffer, &map);
		GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("Invalid stereo audio buffer"), (NULL));
		return GST_FLOW_ERROR;
	}
	float *samples = (float *) map.data;
	double ramp = TONE_LEVEL / (0.25 * self->rate);
	for (gsize i = 0; i < map.size / sizeof (float); i += 2) {
		self->gain = enabled ? MIN (TONE_LEVEL, self->gain + ramp) : MAX (0, self->gain - ramp);
		double music = 1 - self->gain;
		samples[i] = CLAMP (music * samples[i] + self->gain * sin (self->left_phase), -1, 1);
		samples[i + 1] = CLAMP (music * samples[i + 1] + self->gain * sin (self->right_phase), -1, 1);
		self->left_phase = fmod (self->left_phase + 2 * G_PI * 200 / self->rate, 2 * G_PI);
		self->right_phase = fmod (self->right_phase + 2 * G_PI * 240 / self->rate, 2 * G_PI);
	}
	gst_buffer_unmap (buffer, &map);
	return GST_FLOW_OK;
}

static void
nemo_binaural_filter_class_init (NemoBinauralFilterClass *klass)
{
	GstElementClass *element = GST_ELEMENT_CLASS (klass);
	GstBaseTransformClass *transform = GST_BASE_TRANSFORM_CLASS (klass);
	GstCaps *caps = gst_caps_from_string (
		"audio/x-raw,format=" GST_AUDIO_NE (F32) ",channels=2,"
		"layout=interleaved,rate=(int)[8000,192000]");
	gst_element_class_set_static_metadata (element, "Stereo binaural tones",
		"Filter/Effect/Audio", "Optional quiet 200/240 Hz stereo tones", "Nemo contributors");
	gst_element_class_add_pad_template (element,
		gst_pad_template_new ("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
	gst_element_class_add_pad_template (element,
		gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
	gst_caps_unref (caps);
	transform->set_caps = binaural_caps;
	transform->transform_ip = binaural_process;
}

static void
nemo_binaural_filter_init (NemoBinauralFilter *self)
{
	gst_base_transform_set_in_place (GST_BASE_TRANSFORM (self), TRUE);
	self->rate = 48000;
}

GstElement *
nemo_media_audio_filter_new (GError **error)
{
	GstElement *bin = gst_bin_new (NULL);
	GstElement *elements[] = {
		gst_element_factory_make ("audioconvert", NULL),
		gst_element_factory_make ("scaletempo", NULL),
		gst_element_factory_make ("audioconvert", NULL),
		gst_element_factory_make ("spectrum", "music-spectrum"),
		g_object_new (nemo_binaural_filter_get_type (), "name", "binaural-tones", NULL),
		gst_element_factory_make ("audioconvert", NULL)
	};
	for (guint i = 0; i < G_N_ELEMENTS (elements); i++) {
		if (elements[i] == NULL) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				_("Media effects require the GStreamer audio conversion, scaletempo and spectrum plugins."));
			for (guint j = 0; j < G_N_ELEMENTS (elements); j++)
				if (elements[j] != NULL)
					gst_object_unref (elements[j]);
			gst_object_unref (bin);
			return NULL;
		}
	}
	g_object_set (elements[3], "bands", VISUAL_BANDS, "threshold", -80,
		"interval", (guint64) (50 * GST_MSECOND), "post-messages", TRUE, NULL);
	gboolean linked = TRUE;
	for (guint i = 0; i < G_N_ELEMENTS (elements); i++) {
		gst_bin_add (GST_BIN (bin), elements[i]);
		if (i > 0)
			linked &= gst_element_link (elements[i - 1], elements[i]);
	}
	if (!linked) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                    _("Unable to connect the media audio effects."));
		gst_object_unref (bin);
		return NULL;
	}
	GstPad *sink = gst_element_get_static_pad (elements[0], "sink");
	GstPad *src = gst_element_get_static_pad (elements[G_N_ELEMENTS (elements) - 1], "src");
	gst_element_add_pad (bin, gst_ghost_pad_new ("sink", sink));
	gst_element_add_pad (bin, gst_ghost_pad_new ("src", src));
	gst_object_unref (sink);
	gst_object_unref (src);
	return gst_object_ref_sink (bin);
}

void
nemo_media_audio_set_binaural (GstElement *filter, gboolean enabled)
{
	GstElement *tones = gst_bin_get_by_name (GST_BIN (filter), "binaural-tones");
	g_return_if_fail (tones != NULL);
	g_atomic_int_set (&((NemoBinauralFilter *) tones)->enabled, enabled);
	gst_object_unref (tones);
}

struct _NemoMediaVisualizer {
	GtkDrawingArea parent;
	double target[3], levels[3];
	double time;
	gint64 last_frame;
	gint64 last_spectrum;
	guint tick;
	gboolean playing;
	gboolean destroyed;
	gboolean rendering;
	NemoMediaVisualStyle style;
	GCancellable *render_cancel;
	cairo_surface_t *frame;
	double frame_aspect;
	char *render_error;
};

G_DEFINE_TYPE (NemoMediaVisualizer, nemo_media_visualizer, GTK_TYPE_DRAWING_AREA)

typedef struct {
	gint width, height;
	double aspect, time, levels[3];
	NemoMediaVisualStyle style;
} CloudFrame;

static float noise_grid[NOISE_SIZE * NOISE_SIZE];

static void
cloud_noise_init (void)
{
	static gsize initialized;
	if (g_once_init_enter (&initialized)) {
		guint32 seed = 0x5ce29a7b;
		for (guint i = 0; i < G_N_ELEMENTS (noise_grid); i++) {
			seed ^= seed << 13;
			seed ^= seed >> 17;
			seed ^= seed << 5;
			noise_grid[i] = (seed & 0xffffff) / 16777215.0f;
		}
		g_once_init_leave (&initialized, 1);
	}
}

static inline double
cloud_noise (double x, double y)
{
	gint ix = floor (x), iy = floor (y);
	x -= ix;
	y -= iy;
	x = x * x * (3 - 2 * x);
	y = y * y * (3 - 2 * y);
	guint a = ((guint) iy & (NOISE_SIZE - 1)) * NOISE_SIZE;
	guint b = ((guint) (iy + 1) & (NOISE_SIZE - 1)) * NOISE_SIZE;
	guint left = (guint) ix & (NOISE_SIZE - 1), right = (guint) (ix + 1) & (NOISE_SIZE - 1);
	double upper = noise_grid[a + left] * (1 - x) + noise_grid[a + right] * x;
	double lower = noise_grid[b + left] * (1 - x) + noise_grid[b + right] * x;
	return upper * (1 - y) + lower * y;
}

static double
cloud_fbm (double x, double y)
{
	double value = 0, amplitude = 0.52;
	for (guint octave = 0; octave < 5; octave++) {
		value += amplitude * cloud_noise (x, y);
		double next_x = 1.65 * x - 1.2 * y + 17.1;
		y = 1.2 * x + 1.65 * y + 9.2;
		x = next_x;
		amplitude *= 0.48;
	}
	return value;
}

static void
cloud_render_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	CloudFrame *request = data;
	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
		request->width, request->height);
	if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
			"%s", cairo_status_to_string (cairo_surface_status (surface)));
		cairo_surface_destroy (surface);
		return;
	}
	g_autofree float *density = g_new (float, request->width * request->height);
	g_autofree float *detail = g_new (float, request->width * request->height);
	double bass = request->levels[0], mids = request->levels[1], highs = request->levels[2];
	double time = request->time;
	double drift = time * 0.075;
	for (gint y = 0; y < request->height; y++) {
		if (g_task_return_error_if_cancelled (task)) {
			cairo_surface_destroy (surface);
			return;
		}
		for (gint x = 0; x < request->width; x++) {
			double px = (2.0 * x / request->width - 1) * request->aspect;
			double py = 2.0 * y / request->height - 1;
			double warp_x = cloud_noise (px * 1.4 + drift, py * 1.4 + 19) - 0.5;
			double warp_y = cloud_noise (px * 1.4 + 31, py * 1.4 - drift) - 0.5;
			double nx = px * (2.5 + 0.8 * bass) + warp_x * (2 + mids) + drift;
			double ny = py * (2.5 + 0.8 * bass) + warp_y * (2 + mids) - drift * 0.6;
			double fractal = cloud_fbm (nx, ny);
			double mass;
			if (request->style == NEMO_MEDIA_VISUAL_CLOUD_TUNNEL) {
				double radius = hypot (px, py);
				double angular = cloud_noise (px * 2 + 40, py * 2 + drift);
				double ripple = sin (radius * (14 - 2 * bass) - time * 0.22 +
					3.5 * fractal + angular * 1.5);
				double hole = CLAMP ((radius - 0.26 - bass * 0.08) * 5, 0, 1);
				mass = CLAMP ((0.75 * fractal + 0.23 * ripple - 0.23) * 2.6, 0, 1) * hole;
			} else {
				mass = CLAMP ((fractal - 0.34 + bass * 0.06) * 3.4, 0, 1);
			}
			guint index = y * request->width + x;
			density[index] = mass;
			detail[index] = fractal;
		}
	}
	/* Shade neighboring density like a lit cloud surface; no external textures,
	 * GPU requirement, feedback accumulation, or full-resolution CPU rendering. */
	double palette[256][3];
	double hue = mids * 2.4 - highs * 1.5 + 0.06 * time;
	for (guint i = 0; i < G_N_ELEMENTS (palette); i++) {
		double phase = hue + i * 2 * G_PI / G_N_ELEMENTS (palette);
		palette[i][0] = 0.5 + 0.5 * cos (phase);
		palette[i][1] = 0.5 + 0.5 * cos (phase + 2.15);
		palette[i][2] = 0.5 + 0.5 * cos (phase + 4.3);
	}
	guint32 *pixels = (guint32 *) cairo_image_surface_get_data (surface);
	gint stride = cairo_image_surface_get_stride (surface) / 4;
	for (gint y = 0; y < request->height; y++) {
		if (g_task_return_error_if_cancelled (task)) {
			cairo_surface_destroy (surface);
			return;
		}
		for (gint x = 0; x < request->width; x++) {
			guint index = y * request->width + x;
			double mass = density[index];
			double previous = density[MAX (y - 1, 0) * request->width + MAX (x - 1, 0)];
			double next = density[MIN (y + 1, request->height - 1) * request->width +
				MIN (x + 1, request->width - 1)];
			double light = CLAMP (0.55 + (previous - next) * 7, 0.15, 1);
			double rim = CLAMP (fabs (previous - next) * 8, 0, 1);
			guint color = (guint) (detail[index] * 420 + x * 0.14 + y * 0.12) & 255;
			double edge = MIN (MIN (x, request->width - 1 - x) / 12.0,
				MIN (y, request->height - 1 - y) / 12.0);
			double alpha = CLAMP (mass * 2.6, 0, 1) * CLAMP (edge, 0, 1);
			double brightness = (0.18 + 0.65 * light + 0.35 * rim) * (0.7 + 0.3 * mass);
			double glow = rim * light * 0.32;
			guint red = 255 * alpha * CLAMP (brightness * (0.18 + palette[color][0] * 0.82) + glow, 0, 1);
			guint green = 255 * alpha * CLAMP (brightness * (0.12 + palette[color][1] * 0.88) + glow, 0, 1);
			guint blue = 255 * alpha * CLAMP (brightness * (0.25 + palette[color][2] * 0.75) + glow, 0, 1);
			pixels[y * stride + x] = ((guint) (alpha * 255) << 24) | (red << 16) | (green << 8) | blue;
		}
	}
	cairo_surface_mark_dirty (surface);
	g_task_return_pointer (task, surface, (GDestroyNotify) cairo_surface_destroy);
}

static void
cloud_render_dispatch (gpointer data, gpointer unused)
{
	GTask *task = data;
	cloud_render_worker (task, NULL, g_task_get_task_data (task), g_task_get_cancellable (task));
	g_object_unref (task);
}

static void
cloud_render_ready (GObject *source, GAsyncResult *result, gpointer unused)
{
	NemoMediaVisualizer *self = NEMO_MEDIA_VISUALIZER (source);
	GTask *task = G_TASK (result);
	CloudFrame *request = g_task_get_task_data (task);
	GError *error = NULL;
	cairo_surface_t *surface = g_task_propagate_pointer (task, &error);
	self->rendering = FALSE;
	if (!self->destroyed && self->render_cancel == g_task_get_cancellable (task)) {
		g_clear_object (&self->render_cancel);
		if (surface != NULL) {
			g_clear_pointer (&self->frame, cairo_surface_destroy);
			self->frame = g_steal_pointer (&surface);
			self->frame_aspect = request->aspect;
		} else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_free (self->render_error);
			self->render_error = g_strdup (error->message);
			g_warning ("Media visualization: %s", error->message);
		}
	}
	if (!self->destroyed)
		gtk_widget_queue_draw (GTK_WIDGET (self));
	g_clear_pointer (&surface, cairo_surface_destroy);
	g_clear_error (&error);
}

static void
cloud_request_frame (NemoMediaVisualizer *self)
{
	static GThreadPool *pool;
	static gsize initialized;
	if (self->rendering || self->destroyed || self->render_error != NULL ||
	    !gtk_widget_get_mapped (GTK_WIDGET (self)))
		return;
	gint width = gtk_widget_get_allocated_width (GTK_WIDGET (self));
	gint height = gtk_widget_get_allocated_height (GTK_WIDGET (self));
	if (width < 2 || height < 2)
		return;
	cloud_noise_init ();
	if (g_once_init_enter (&initialized)) {
		GError *error = NULL;
		pool = g_thread_pool_new (cloud_render_dispatch, NULL, 2, FALSE, &error);
		if (error != NULL) {
			self->render_error = g_strdup (error->message);
			g_warning ("Media visualization workers: %s", error->message);
			g_clear_error (&error);
		}
		g_once_init_leave (&initialized, 1);
	}
	if (pool == NULL) {
		if (self->render_error == NULL)
			self->render_error = g_strdup (_("Visualization workers are unavailable."));
		return;
	}
	double scale = MIN (1, sqrt ((double) CLOUD_PIXELS / ((double) width * height)));
	CloudFrame *request = g_new0 (CloudFrame, 1);
	request->width = MAX (2, (gint) (width * scale));
	request->height = MAX (2, (gint) (height * scale));
	request->aspect = (double) width / height;
	request->time = self->time;
	memcpy (request->levels, self->levels, sizeof request->levels);
	request->style = self->style;
	self->render_cancel = g_cancellable_new ();
	GTask *task = g_task_new (self, self->render_cancel, cloud_render_ready, NULL);
	g_task_set_task_data (task, request, g_free);
	self->rendering = TRUE;
	GError *error = NULL;
	/* GLib retains a queued item even if starting a worker fails. */
	if (!g_thread_pool_push (pool, task, &error)) {
		self->render_error = g_strdup (error->message);
		g_warning ("Media visualization worker: %s", error->message);
		g_cancellable_cancel (self->render_cancel);
		g_clear_error (&error);
	}
}

static gboolean
visualizer_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer unused)
{
	NemoMediaVisualizer *self = NEMO_MEDIA_VISUALIZER (widget);
	gint64 now = gdk_frame_clock_get_frame_time (clock);
	if (self->last_frame != 0 && now - self->last_frame < 41666)
		return G_SOURCE_CONTINUE;
	double dt = self->last_frame == 0 ? 0.042 : CLAMP ((now - self->last_frame) / 1000000.0, 0, 0.1);
	self->last_frame = now;
	for (guint i = 0; i < 3; i++) {
		double target = g_get_monotonic_time () - self->last_spectrum > 300000 ? 0 : self->target[i];
		double smooth = 1 - exp (-dt / (target > self->levels[i] ? 0.2 : 0.65));
		self->levels[i] += (target - self->levels[i]) * smooth;
	}
	self->time = fmod (self->time + dt * (0.5 + self->levels[0] * 0.8), 4096);
	cloud_request_frame (self);
	return G_SOURCE_CONTINUE;
}

static void
visualizer_update_tick (NemoMediaVisualizer *self)
{
	gboolean animate;
	g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)), "gtk-enable-animations", &animate, NULL);
	gboolean active = self->playing && !self->destroyed && animate &&
		gtk_widget_get_mapped (GTK_WIDGET (self));
	if (active && self->tick == 0) {
		self->last_frame = 0;
		self->tick = gtk_widget_add_tick_callback (GTK_WIDGET (self), visualizer_tick, NULL, NULL);
	} else if (!active && self->tick != 0) {
		gtk_widget_remove_tick_callback (GTK_WIDGET (self), self->tick);
		self->tick = 0;
	}
}

static void
visualizer_map (GtkWidget *widget)
{
	GTK_WIDGET_CLASS (nemo_media_visualizer_parent_class)->map (widget);
	visualizer_update_tick (NEMO_MEDIA_VISUALIZER (widget));
}

static void
visualizer_unmap (GtkWidget *widget)
{
	NemoMediaVisualizer *self = NEMO_MEDIA_VISUALIZER (widget);
	if (self->tick != 0) {
		gtk_widget_remove_tick_callback (widget, self->tick);
		self->tick = 0;
	}
	if (self->render_cancel != NULL) {
		g_cancellable_cancel (self->render_cancel);
		g_clear_object (&self->render_cancel);
	}
	GTK_WIDGET_CLASS (nemo_media_visualizer_parent_class)->unmap (widget);
}

static gboolean
visualizer_draw (GtkWidget *widget, cairo_t *cr)
{
	NemoMediaVisualizer *self = NEMO_MEDIA_VISUALIZER (widget);
	double width = gtk_widget_get_allocated_width (widget);
	double height = gtk_widget_get_allocated_height (widget);
	GtkStyleContext *context = gtk_widget_get_style_context (gtk_widget_get_toplevel (widget));
	double background_alpha = 1;
	if (gtk_style_context_has_class (context, "smplos-native-alpha")) {
		GdkRGBA background;
		gtk_style_context_get_background_color (context, gtk_style_context_get_state (context), &background);
		background_alpha = background.alpha;
	}
	/* Replace inherited light-theme paint while retaining native background alpha. */
	cairo_save (cr);
	cairo_set_operator (cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba (cr, 0.012, 0.008, 0.025, background_alpha);
	cairo_rectangle (cr, 0, 0, width, height);
	cairo_fill (cr);
	cairo_restore (cr);
	if (self->frame == NULL || fabs (self->frame_aspect - width / MAX (height, 1)) > 0.01)
		cloud_request_frame (self);
	if (self->frame != NULL) {
		cairo_save (cr);
		cairo_scale (cr, width / cairo_image_surface_get_width (self->frame),
			height / cairo_image_surface_get_height (self->frame));
		cairo_set_source_surface (cr, self->frame, 0, 0);
		cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_BILINEAR);
		cairo_paint (cr);
		cairo_restore (cr);
	}
	/* Stable stars, not flashing particles; highlights remain fully opaque. */
	cairo_set_source_rgb (cr, 0.75, 0.82, 1);
	for (guint i = 0; i < 140; i++) {
		double x = (0.04 + 0.92 * noise_grid[i * 13]) * width;
		double y = (0.04 + 0.92 * noise_grid[i * 17 + 7]) * height;
		double radius = i % 11 == 0 ? 1.2 : 0.65;
		cairo_arc (cr, x, y, radius, 0, 2 * G_PI);
		cairo_fill (cr);
	}
	if (self->render_error != NULL) {
		PangoLayout *layout = gtk_widget_create_pango_layout (widget, self->render_error);
		pango_layout_set_width (layout, MAX (1, width - 32) * PANGO_SCALE);
		cairo_move_to (cr, 16, 16);
		pango_cairo_show_layout (cr, layout);
		g_object_unref (layout);
	}
	return FALSE;
}

static void
visualizer_destroy (GtkWidget *widget)
{
	NemoMediaVisualizer *self = NEMO_MEDIA_VISUALIZER (widget);
	self->destroyed = TRUE;
	if (self->render_cancel != NULL)
		g_cancellable_cancel (self->render_cancel);
	self->playing = FALSE;
	visualizer_update_tick (self);
	GTK_WIDGET_CLASS (nemo_media_visualizer_parent_class)->destroy (widget);
}

static void
visualizer_finalize (GObject *object)
{
	NemoMediaVisualizer *self = NEMO_MEDIA_VISUALIZER (object);
	g_clear_pointer (&self->frame, cairo_surface_destroy);
	g_clear_object (&self->render_cancel);
	g_free (self->render_error);
	G_OBJECT_CLASS (nemo_media_visualizer_parent_class)->finalize (object);
}

static void
nemo_media_visualizer_class_init (NemoMediaVisualizerClass *klass)
{
	GtkWidgetClass *widget = GTK_WIDGET_CLASS (klass);
	widget->draw = visualizer_draw;
	widget->map = visualizer_map;
	widget->unmap = visualizer_unmap;
	widget->destroy = visualizer_destroy;
	G_OBJECT_CLASS (klass)->finalize = visualizer_finalize;
}

static void
nemo_media_visualizer_init (NemoMediaVisualizer *self)
{
	gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
	gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
	atk_object_set_name (gtk_widget_get_accessible (GTK_WIDGET (self)), _("Music visualization"));
	atk_object_set_description (gtk_widget_get_accessible (GTK_WIDGET (self)),
		_("Colorful nebula clouds or a concentric cloud tunnel, with smooth music-reactive shapes and colors."));
	g_signal_connect_object (gtk_widget_get_settings (GTK_WIDGET (self)),
		"notify::gtk-enable-animations", G_CALLBACK (visualizer_update_tick), self, G_CONNECT_SWAPPED);
}

void
nemo_media_visualizer_set_style (NemoMediaVisualizer *self, NemoMediaVisualStyle style)
{
	g_return_if_fail (style == NEMO_MEDIA_VISUAL_NEBULA || style == NEMO_MEDIA_VISUAL_CLOUD_TUNNEL);
	self->style = style;
	if (self->render_cancel != NULL) {
		g_cancellable_cancel (self->render_cancel);
		g_clear_object (&self->render_cancel);
	}
	g_clear_pointer (&self->frame, cairo_surface_destroy);
	gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
nemo_media_visualizer_reset (NemoMediaVisualizer *self)
{
	nemo_media_visualizer_set_playing (self, FALSE);
	memset (self->target, 0, sizeof self->target);
	memset (self->levels, 0, sizeof self->levels);
	self->last_spectrum = 0;
	if (self->render_cancel != NULL) {
		g_cancellable_cancel (self->render_cancel);
		g_clear_object (&self->render_cancel);
	}
	g_clear_pointer (&self->frame, cairo_surface_destroy);
	if (!self->destroyed)
		gtk_widget_queue_draw (GTK_WIDGET (self));
}

GtkWidget *
nemo_media_visualizer_new (void)
{
	return g_object_new (NEMO_TYPE_MEDIA_VISUALIZER, NULL);
}

void
nemo_media_visualizer_set_playing (NemoMediaVisualizer *self, gboolean playing)
{
	self->playing = playing;
	visualizer_update_tick (self);
}

void
nemo_media_visualizer_handle_message (NemoMediaVisualizer *self, GstMessage *message)
{
	const GstStructure *structure = gst_message_get_structure (message);
	if (structure == NULL || !gst_structure_has_name (structure, "spectrum"))
		return;
	const GValue *magnitudes = gst_structure_get_value (structure, "magnitude");
	if (magnitudes == NULL || !GST_VALUE_HOLDS_LIST (magnitudes) ||
	    gst_value_list_get_size (magnitudes) != VISUAL_BANDS)
		return;
	gint rate = 0;
	if (GST_IS_ELEMENT (GST_MESSAGE_SRC (message))) {
		GstPad *pad = gst_element_get_static_pad (GST_ELEMENT (GST_MESSAGE_SRC (message)), "sink");
		GstCaps *caps = pad != NULL ? gst_pad_get_current_caps (pad) : NULL;
		if (caps != NULL && !gst_caps_is_empty (caps))
			gst_structure_get_int (gst_caps_get_structure (caps, 0), "rate", &rate);
		if (caps != NULL)
			gst_caps_unref (caps);
		if (pad != NULL)
			gst_object_unref (pad);
	}
	if (rate <= 0)
		return;
	double peak[3] = { 0, 0, 0 };
	for (guint i = 0; i < VISUAL_BANDS; i++) {
		const GValue *value = gst_value_list_get_value (magnitudes, i);
		if (G_VALUE_HOLDS_FLOAT (value)) {
			float magnitude = g_value_get_float (value);
			double frequency = (i + 0.5) * rate / (2.0 * VISUAL_BANDS);
			if (!isfinite (magnitude) || frequency < 30 || frequency > 12000)
				continue;
			guint group = frequency < 250 ? 0 : frequency < 2500 ? 1 : 2;
			peak[group] = MAX (peak[group], pow (10, CLAMP (magnitude, -80, 0) / 20));
		}
	}
	for (guint i = 0; i < 3; i++)
		self->target[i] = log1p (peak[i] * 12) / log (13);
	self->last_spectrum = g_get_monotonic_time ();
}
