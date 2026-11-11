/* Native media effects. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include "nemo-media-effects.h"
#include <gst/base/gstbasetransform.h>
#include <gst/audio/audio-format.h>
#include <glib/gi18n.h>
#include <math.h>

#define VISUAL_BANDS 64
#define TONE_LEVEL 0.01

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
	double target[VISUAL_BANDS], bands[VISUAL_BANDS];
	double phase;
	gint64 last_frame;
	guint tick;
	gboolean playing;
};

G_DEFINE_TYPE (NemoMediaVisualizer, nemo_media_visualizer, GTK_TYPE_DRAWING_AREA)

static gboolean
visualizer_tick (GtkWidget *widget, GdkFrameClock *clock, gpointer unused)
{
	NemoMediaVisualizer *self = NEMO_MEDIA_VISUALIZER (widget);
	gint64 now = gdk_frame_clock_get_frame_time (clock);
	if (self->last_frame != 0 && now - self->last_frame < 33000)
		return G_SOURCE_CONTINUE;
	double dt = self->last_frame == 0 ? 0.033 : CLAMP ((now - self->last_frame) / 1000000.0, 0, 0.1);
	self->last_frame = now;
	double smooth = 1 - exp (-dt / 0.35);
	for (guint i = 0; i < VISUAL_BANDS; i++)
		self->bands[i] += (self->target[i] - self->bands[i]) * smooth;
	self->phase = fmod (self->phase + dt * 0.15, 2 * G_PI);
	gtk_widget_queue_draw (widget);
	return G_SOURCE_CONTINUE;
}

static void
visualizer_update_tick (NemoMediaVisualizer *self)
{
	gboolean animate;
	g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)), "gtk-enable-animations", &animate, NULL);
	gboolean active = self->playing && animate && gtk_widget_get_mapped (GTK_WIDGET (self));
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
	GTK_WIDGET_CLASS (nemo_media_visualizer_parent_class)->unmap (widget);
}

static gboolean
visualizer_draw (GtkWidget *widget, cairo_t *cr)
{
	NemoMediaVisualizer *self = NEMO_MEDIA_VISUALIZER (widget);
	double width = gtk_widget_get_allocated_width (widget);
	double height = gtk_widget_get_allocated_height (widget);
	double radius = MIN (width, height) * 0.25;
	GdkRGBA accent;
	GtkStyleContext *context = gtk_widget_get_style_context (widget);
	if (!gtk_style_context_lookup_color (context, "theme_selected_bg_color", &accent))
		gtk_style_context_get_color (context, GTK_STATE_FLAG_NORMAL, &accent);
	accent.alpha = 1;
	gdk_cairo_set_source_rgba (cr, &accent);
	cairo_set_line_width (cr, 2);
	for (guint ring = 0; ring < 3; ring++) {
		for (guint i = 0; i <= 192; i++) {
			double angle = 2 * G_PI * i / 192;
			guint band = (i * VISUAL_BANDS / 192) % VISUAL_BANDS;
			double r = radius * (0.75 + 0.22 * ring + 0.12 * self->bands[band] +
				0.035 * sin (angle * 3 + self->phase + ring));
			double x = width / 2 + r * cos (angle);
			double y = height / 2 + r * sin (angle);
			if (i == 0)
				cairo_move_to (cr, x, y);
			else
				cairo_line_to (cr, x, y);
		}
		cairo_close_path (cr);
		cairo_stroke (cr);
	}
	return FALSE;
}

static void
nemo_media_visualizer_class_init (NemoMediaVisualizerClass *klass)
{
	GtkWidgetClass *widget = GTK_WIDGET_CLASS (klass);
	widget->draw = visualizer_draw;
	widget->map = visualizer_map;
	widget->unmap = visualizer_unmap;
}

static void
nemo_media_visualizer_init (NemoMediaVisualizer *self)
{
	gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
	gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
	atk_object_set_name (gtk_widget_get_accessible (GTK_WIDGET (self)), _("Music visualization"));
	atk_object_set_description (gtk_widget_get_accessible (GTK_WIDGET (self)),
		_("Smooth, non-flashing rings responding to the current track's sound."));
	g_signal_connect_object (gtk_widget_get_settings (GTK_WIDGET (self)),
		"notify::gtk-enable-animations", G_CALLBACK (visualizer_update_tick), self, G_CONNECT_SWAPPED);
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
	for (guint i = 0; i < VISUAL_BANDS; i++) {
		const GValue *value = gst_value_list_get_value (magnitudes, i);
		if (G_VALUE_HOLDS_FLOAT (value)) {
			float magnitude = g_value_get_float (value);
			self->target[i] = isfinite (magnitude) ? CLAMP ((magnitude + 80) / 80, 0, 1) : 0;
		}
	}
}
