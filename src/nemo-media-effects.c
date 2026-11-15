/* Native media effects. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include "nemo-media-effects.h"
#include <gst/base/gstbasetransform.h>
#include <gst/audio/audio-format.h>
#include <gst/fft/gstfftf32.h>
#include <glib/gi18n.h>
#include <math.h>
#ifdef HAVE_PROJECTM
#include <projectM-4/projectM.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#endif

#define TONE_LEVEL 0.01
#define PCM_FRAMES 512
#define ANALYSIS_FRAMES 2048
#define SPECTRUM_BANDS 128
#define HISTORY_COLUMNS 600
#define VISUAL_WIDTH 800
#define VISUAL_HEIGHT 450

typedef struct {
	GstBaseTransform parent;
	gint enabled, visuals, rate;
	guint visual_frames;
	guint pcm_write, pcm_count;
	guint analysis_write;
	gboolean previous_visuals;
	double resample_position;
	float visual_pcm[PCM_FRAMES * 2];
	float analysis_pcm[ANALYSIS_FRAMES * 2];
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
	gboolean visuals = g_atomic_int_get (&self->visuals);
	if (visuals != self->previous_visuals) {
		self->previous_visuals = visuals;
		self->visual_frames = self->pcm_write = self->pcm_count = 0;
		self->resample_position = 0;
		memset (self->visual_pcm, 0, sizeof self->visual_pcm);
		memset (self->analysis_pcm, 0, sizeof self->analysis_pcm);
		self->analysis_write = 0;
	}
	if (!enabled && self->gain == 0 && !visuals)
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
	guint frames = map.size / (2 * sizeof (float));
	if (enabled || self->gain > 0) {
		double ramp = TONE_LEVEL / (0.25 * self->rate);
		for (guint i = 0; i < frames * 2; i += 2) {
			self->gain = enabled ? MIN (TONE_LEVEL, self->gain + ramp) : MAX (0, self->gain - ramp);
			double music = 1 - self->gain;
			samples[i] = CLAMP (music * samples[i] + self->gain * sin (self->left_phase), -1, 1);
			samples[i + 1] = CLAMP (music * samples[i + 1] + self->gain * sin (self->right_phase), -1, 1);
			self->left_phase = fmod (self->left_phase + 2 * G_PI * 200 / self->rate, 2 * G_PI);
			self->right_phase = fmod (self->right_phase + 2 * G_PI * 240 / self->rate, 2 * G_PI);
		}
	}
	if (visuals && frames > 0) {
		/* Resample into a bounded stereo history across input buffers, so
		 * high sample rates and small decoder buffers retain a full window. */
		double step = self->rate / 44100.0;
		double pos = self->resample_position;
		for (; pos < frames; pos += step) {
			guint a = (guint) floor (pos);
			guint b = MIN (a + 1, frames - 1);
			double fraction = pos - a;
			for (guint channel = 0; channel < 2; channel++) {
				float value = samples[a * 2 + channel] * (1 - fraction) +
					samples[b * 2 + channel] * fraction;
				self->visual_pcm[self->pcm_write * 2 + channel] = value;
				self->analysis_pcm[self->analysis_write * 2 + channel] = value;
			}
			self->pcm_write = (self->pcm_write + 1) % PCM_FRAMES;
			self->analysis_write = (self->analysis_write + 1) % ANALYSIS_FRAMES;
			self->pcm_count = MIN (self->pcm_count + 1, PCM_FRAMES);
		}
		self->resample_position = pos - frames;
		self->visual_frames += frames;
	}
	if (visuals && self->pcm_count == PCM_FRAMES && self->visual_frames >= self->rate / 30) {
		self->visual_frames = 0;
		float pcm[PCM_FRAMES * 2];
		for (guint i = 0; i < PCM_FRAMES; i++)
			for (guint channel = 0; channel < 2; channel++)
				pcm[i * 2 + channel] = self->visual_pcm[
					((self->pcm_write + i) % PCM_FRAMES) * 2 + channel];
		GstBuffer *copy = gst_buffer_new_allocate (NULL, sizeof pcm, NULL);
		gst_buffer_fill (copy, 0, pcm, sizeof pcm);
		float analysis[ANALYSIS_FRAMES * 2];
		for (guint i = 0; i < ANALYSIS_FRAMES; i++)
			for (guint channel = 0; channel < 2; channel++)
				analysis[i * 2 + channel] = self->analysis_pcm[
					((self->analysis_write + i) % ANALYSIS_FRAMES) * 2 + channel];
		GstBuffer *spectrum = gst_buffer_new_allocate (NULL, sizeof analysis, NULL);
		gst_buffer_fill (spectrum, 0, analysis, sizeof analysis);
		GstStructure *structure = gst_structure_new ("nemo-music-pcm",
			"samples", GST_TYPE_BUFFER, copy, "analysis", GST_TYPE_BUFFER, spectrum, NULL);
		gst_buffer_unref (copy);
		gst_buffer_unref (spectrum);
		gst_element_post_message (GST_ELEMENT (self),
			gst_message_new_element (GST_OBJECT (self), structure));
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
	gst_element_class_set_static_metadata (element, "Stereo binaural tones and visualization PCM",
		"Filter/Effect/Audio", "Optional stereo tones and bounded player audio samples", "Nemo contributors");
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
		g_object_new (nemo_binaural_filter_get_type (), "name", "binaural-tones", NULL),
		gst_element_factory_make ("audioconvert", NULL)
	};
	for (guint i = 0; i < G_N_ELEMENTS (elements); i++) {
		if (elements[i] == NULL) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				_("Media effects require the GStreamer audio conversion and scaletempo plugins."));
			for (guint j = 0; j < G_N_ELEMENTS (elements); j++)
				if (elements[j] != NULL)
					gst_object_unref (elements[j]);
			gst_object_unref (bin);
			return NULL;
		}
	}
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

void
nemo_media_audio_set_visuals (GstElement *filter, gboolean enabled)
{
	GstElement *tones = gst_bin_get_by_name (GST_BIN (filter), "binaural-tones");
	g_return_if_fail (tones != NULL);
	g_atomic_int_set (&((NemoBinauralFilter *) tones)->visuals, enabled);
	gst_object_unref (tones);
}

typedef struct {
#ifdef HAVE_PROJECTM
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
	projectm_handle engine;
#endif
	guint preset;
	char *error;
} VisualEngine;

typedef struct {
	GstFFTF32 *fft;
	double levels[2][SPECTRUM_BANDS];
	cairo_surface_t *history;
	guint history_write, serial;
	gint64 last_pcm, last_frame;
	double rain[50];
} NativeVisual;

struct _NemoMediaVisualizer {
	GtkDrawingArea parent;
	GApplication *application;
	gboolean playing, destroyed, rendering, automatic;
	guint tick, preset, rendered_frames;
	gint64 last_frame, last_pcm;
	double preset_time;
	float pcm[PCM_FRAMES * 2];
	float analysis_pcm[ANALYSIS_FRAMES * 2];
	guint reset_serial;
	GCancellable *render_cancel;
	cairo_surface_t *frame;
	VisualEngine gpu;
	NativeVisual native;
	char *render_error;
};

G_DEFINE_TYPE (NemoMediaVisualizer, nemo_media_visualizer, GTK_TYPE_DRAWING_AREA)

static GThreadPool *visual_pool;
static gboolean visual_shutting_down;

typedef struct {
	guint preset;
	float pcm[PCM_FRAMES * 2];
	float analysis_pcm[ANALYSIS_FRAMES * 2];
	gint64 pcm_time;
	guint reset_serial;
} VisualFrame;

typedef enum {
	VISUAL_PROJECTM,
	VISUAL_SPECTRUM,
	VISUAL_SPECTROGRAM,
	VISUAL_MATRIX
} VisualKind;

static const struct {
	const char *id;
	const char *name;
	const char *resource;
	VisualKind kind;
} visual_presets[] = {
	{ "aurora-cathedral", N_("Aurora Cathedral"), "aurora-cathedral.milk", VISUAL_PROJECTM },
	{ "bioluminescent-abyss", N_("Bioluminescent Abyss"), "bioluminescent-abyss.milk", VISUAL_PROJECTM },
	{ "liquid-chrome-kaleidoscope", N_("Liquid Chrome Kaleidoscope"), "liquid-chrome-kaleidoscope.milk", VISUAL_PROJECTM },
	{ "nebula-forge", N_("Nebula Forge"), "nebula-forge.milk", VISUAL_PROJECTM },
	{ "neon-sacred-geometry", N_("Neon Sacred Geometry"), "neon-sacred-geometry.milk", VISUAL_PROJECTM },
	{ "tramonto-sul-golfo", N_("Tramonto sul Golfo"), "tramonto-sul-golfo.milk", VISUAL_PROJECTM },
	{ "spectrum-vortex", N_("Spectrum Vortex"), NULL, VISUAL_PROJECTM },
	{ "prismatic-lattice", N_("Prismatic Lattice"), NULL, VISUAL_PROJECTM },
	{ "spiral-bloom", N_("Spiral Bloom"), NULL, VISUAL_PROJECTM },
	{ "liquid-ribbons", N_("Liquid Ribbons"), NULL, VISUAL_PROJECTM },
	{ "emerald-vortex", N_("Emerald Vortex"), NULL, VISUAL_PROJECTM },
	{ "violet-lattice", N_("Violet Lattice"), NULL, VISUAL_PROJECTM },
	{ "solar-bloom", N_("Solar Bloom"), NULL, VISUAL_PROJECTM },
	{ "sapphire-ribbons", N_("Sapphire Ribbons"), NULL, VISUAL_PROJECTM },
	{ "crimson-vortex", N_("Crimson Vortex"), NULL, VISUAL_PROJECTM },
	{ "amber-lattice", N_("Amber Lattice"), NULL, VISUAL_PROJECTM },
	{ "midnight-bloom", N_("Midnight Bloom"), NULL, VISUAL_PROJECTM },
	{ "iridescent-ribbons", N_("Iridescent Ribbons"), NULL, VISUAL_PROJECTM },
	{ "stereo-spectrum", N_("Stereo spectrum"), NULL, VISUAL_SPECTRUM },
	{ "spectrogram", N_("Spectrogram"), NULL, VISUAL_SPECTROGRAM },
	{ "digital-rain", N_("Digital rain"), NULL, VISUAL_MATRIX }
};

guint
nemo_media_visualizer_get_n_presets (void)
{
	return G_N_ELEMENTS (visual_presets);
}

const char *
nemo_media_visualizer_get_preset_id (guint index)
{
	g_return_val_if_fail (index < G_N_ELEMENTS (visual_presets), NULL);
	return visual_presets[index].id;
}

const char *
nemo_media_visualizer_get_preset_name (guint index)
{
	g_return_val_if_fail (index < G_N_ELEMENTS (visual_presets), NULL);
	return _(visual_presets[index].name);
}

static double
spectrum_frequency (guint band)
{
	return 43.0 * pow (20000.0 / 43.0, band / (double) SPECTRUM_BANDS);
}

static void
native_label (cairo_t *cr, const char *text, double x, double y)
{
	PangoLayout *layout = pango_cairo_create_layout (cr);
	PangoFontDescription *font = pango_font_description_from_string ("Monospace 10");
	pango_layout_set_font_description (layout, font);
	pango_font_description_free (font);
	pango_layout_set_text (layout, text, -1);
	cairo_set_source_rgb (cr, 0.65, 0.85, 0.9);
	cairo_move_to (cr, x, y);
	pango_cairo_show_layout (cr, layout);
	g_object_unref (layout);
}

static guint32
spectrogram_color (double level)
{
	static const guint colors[][3] = {
		{ 0, 0, 0 }, { 0, 0, 65 }, { 30, 0, 150 }, { 170, 0, 110 },
		{ 240, 40, 0 }, { 255, 180, 0 }, { 255, 255, 150 }, { 255, 255, 255 }
	};
	double position = CLAMP (level, 0, 1) * (G_N_ELEMENTS (colors) - 1);
	guint low = MIN ((guint) position, G_N_ELEMENTS (colors) - 2);
	double fraction = position - low;
	guint rgb[3];
	for (guint i = 0; i < 3; i++)
		rgb[i] = colors[low][i] * (1 - fraction) + colors[low + 1][i] * fraction;
	return 0xff000000 | (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
}

static cairo_surface_t *
native_render (NemoMediaVisualizer *self, VisualFrame *request, GError **error)
{
	NativeVisual *native = &self->native;
	VisualKind kind = visual_presets[request->preset].kind;
	if (native->fft == NULL) {
		native->fft = gst_fft_f32_new (ANALYSIS_FRAMES, FALSE);
		if (native->fft == NULL) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
				_("Cannot initialize frequency analysis."));
			return NULL;
		}
	}
	if (native->serial != request->reset_serial) {
		memset (native->levels, 0, sizeof native->levels);
		g_clear_pointer (&native->history, cairo_surface_destroy);
		native->history_write = 0;
		native->last_pcm = 0;
		native->serial = request->reset_serial;
	}
	gboolean fresh = request->pcm_time != native->last_pcm;
	if (fresh) {
		gfloat input[ANALYSIS_FRAMES];
		GstFFTF32Complex frequency[ANALYSIS_FRAMES / 2 + 1];
		for (guint channel = 0; channel < 2; channel++) {
			for (guint i = 0; i < ANALYSIS_FRAMES; i++)
				input[i] = request->analysis_pcm[i * 2 + channel];
			gst_fft_f32_window (native->fft, input, GST_FFT_WINDOW_HANN);
			gst_fft_f32_fft (native->fft, input, frequency);
			for (guint band = 0; band < SPECTRUM_BANDS; band++) {
				guint start = MAX (1, (guint) floor (spectrum_frequency (band) * ANALYSIS_FRAMES / 44100));
				guint end = MIN (ANALYSIS_FRAMES / 2 + 1,
					MAX (start + 1, (guint) ceil (spectrum_frequency (band + 1) * ANALYSIS_FRAMES / 44100)));
				double power = 0;
				for (guint i = start; i < end; i++)
					power = MAX (power, frequency[i].r * frequency[i].r + frequency[i].i * frequency[i].i);
				double db = 20 * log10 (MAX (1e-5, 4 * sqrt (power) / ANALYSIS_FRAMES));
				double level = CLAMP ((db + 80) / 80, 0, 1);
				double *previous = &native->levels[channel][band];
				*previous += (level - *previous) * (level > *previous ? 0.8 : 0.2);
			}
		}
		native->last_pcm = request->pcm_time;
	}
	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, VISUAL_WIDTH, VISUAL_HEIGHT);
	if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, cairo_status_to_string (cairo_surface_status (surface)));
		cairo_surface_destroy (surface);
		return NULL;
	}
	cairo_t *cr = cairo_create (surface);
	cairo_set_source_rgb (cr, 0, 0, 0);
	cairo_paint (cr);
	if (kind == VISUAL_SPECTRUM) {
		for (guint channel = 0; channel < 2; channel++) {
			double top = channel * VISUAL_HEIGHT / 2.0;
			native_label (cr, channel == 0 ? _("Left") : _("Right"), 12, top + 8);
			for (guint band = 0; band < 32; band++) {
				double level = 0;
				for (guint i = 0; i < SPECTRUM_BANDS / 32; i++)
					level = MAX (level, native->levels[channel][band * (SPECTRUM_BANDS / 32) + i]);
				double width = (VISUAL_WIDTH - 70) / 32.0;
				for (guint step = 0; step < (guint) (level * 25); step++) {
					cairo_set_source_rgb (cr, step > 21 ? 1 : 0.02, step > 21 ? 0.65 : 0.65, step > 21 ? 0.1 : 0.9);
					cairo_rectangle (cr, 52 + band * width, top + 194 - step * 6, width - 3, 4);
					cairo_fill (cr);
				}
			}
			native_label (cr, "43 Hz", 52, top + 205);
			native_label (cr, "1 kHz", 410, top + 205);
			native_label (cr, "20 kHz", 725, top + 205);
		}
	} else if (kind == VISUAL_SPECTROGRAM) {
		gboolean append = fresh || native->history == NULL;
		if (native->history == NULL) {
			native->history = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, HISTORY_COLUMNS, SPECTRUM_BANDS * 2);
			if (cairo_surface_status (native->history) != CAIRO_STATUS_SUCCESS) {
				g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
					cairo_status_to_string (cairo_surface_status (native->history)));
				cairo_destroy (cr);
				cairo_surface_destroy (surface);
				return NULL;
			}
			cairo_t *clear = cairo_create (native->history);
			cairo_set_source_rgb (clear, 0, 0, 0);
			cairo_paint (clear);
			cairo_destroy (clear);
		}
		if (append) {
			cairo_surface_flush (native->history);
			guint32 *pixels = (guint32 *) cairo_image_surface_get_data (native->history);
			for (guint channel = 0; channel < 2; channel++)
				for (guint band = 0; band < SPECTRUM_BANDS; band++)
					pixels[(channel * SPECTRUM_BANDS + SPECTRUM_BANDS - 1 - band) * HISTORY_COLUMNS +
						native->history_write] = spectrogram_color (native->levels[channel][band]);
			native->history_write = (native->history_write + 1) % HISTORY_COLUMNS;
			cairo_surface_mark_dirty (native->history);
		}
		cairo_save (cr);
		cairo_rectangle (cr, 52, 22, VISUAL_WIDTH - 68, VISUAL_HEIGHT - 48);
		cairo_clip (cr);
		cairo_translate (cr, 52, 22);
		cairo_scale (cr, (VISUAL_WIDTH - 68) / (double) HISTORY_COLUMNS,
			(VISUAL_HEIGHT - 48) / (double) (SPECTRUM_BANDS * 2));
		cairo_set_source_surface (cr, native->history, -(double) native->history_write, 0);
		cairo_paint (cr);
		cairo_set_source_surface (cr, native->history, HISTORY_COLUMNS - native->history_write, 0);
		cairo_paint (cr);
		cairo_restore (cr);
		native_label (cr, _("Left"), 8, 24);
		native_label (cr, _("Right"), 8, 225);
		for (guint channel = 0; channel < 2; channel++) {
			double top = 22 + channel * (VISUAL_HEIGHT - 48) / 2.0;
			native_label (cr, "20k", 12, top + 20);
			native_label (cr, "1k", 12, top + 96);
			native_label (cr, "43", 12, top + 180);
		}
		native_label (cr, _("History"), 52, VISUAL_HEIGHT - 22);
		native_label (cr, _("Now"), VISUAL_WIDTH - 60, VISUAL_HEIGHT - 22);
	} else {
		gint64 now = g_get_monotonic_time ();
		double dt = native->last_frame == 0 ? 0 : CLAMP ((now - native->last_frame) / 1000000.0, 0, 0.1);
		native->last_frame = now;
		cairo_select_font_face (cr, "Monospace", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
		cairo_set_font_size (cr, 14);
		for (guint column = 0; column < G_N_ELEMENTS (native->rain); column++) {
			double level = native->levels[column % 2][column * SPECTRUM_BANDS / G_N_ELEMENTS (native->rain)];
			native->rain[column] += dt * (2 + 9 * level + column % 5);
			double head = fmod (native->rain[column] + (column * 17) % 39, 39);
			for (guint tail = 0; tail < 12; tail++) {
				gint row = (gint) head - tail;
				if (row < 0 || row > 28)
					continue;
				double light = (1 - tail / 12.0) * (0.4 + 0.6 * level);
				cairo_set_source_rgb (cr, tail == 0 ? 0.6 : 0, tail == 0 ? 1 : light,
					tail == 0 ? 0.75 : light * 0.2);
				char glyph[] = { "0123456789ABCDEF"[(column * 13 + row * 7 + (guint) native->rain[column]) % 16], 0 };
				cairo_move_to (cr, column * 16, row * 16 + 14);
				cairo_show_text (cr, glyph);
			}
		}
	}
	cairo_destroy (cr);
	return surface;
}

#ifdef HAVE_PROJECTM
static char *
visual_preset (guint index, GError **error)
{
	if (visual_presets[index].resource != NULL) {
		g_autofree char *path = g_strconcat ("/org/nemo/player-presets/",
			visual_presets[index].resource, NULL);
		g_autoptr (GBytes) bytes = g_resources_lookup_data (path, G_RESOURCE_LOOKUP_FLAGS_NONE, error);
		if (bytes == NULL)
			return NULL;
		gsize size;
		const char *data = g_bytes_get_data (bytes, &size);
		return g_strndup (data, size);
	}
	index -= 6;
	/* Original MilkDrop equations, not third-party preset files. Feedback,
	 * waveforms and polygon emitters are rendered by the real projectM engine. */
	static const char *fields[] = {
		"rot=0.025*sin(ang*3+time*0.23)+0.008*bass_att; zoom=1.018+0.008*sin(rad*9-time*0.4);",
		"rot=0.018*sin(rad*12-time*0.31); dx=0.009*sin(y*10+time*0.2); dy=0.009*cos(x*10-time*0.17);",
		"zoom=1.015+0.012*sin(ang*6+time*0.12); rot=0.018*sin(rad*8+time*0.21);",
		"dx=0.014*sin(y*8+time*0.24); dy=0.012*sin(x*7-time*0.19); rot=0.006*sin(ang*2);"
	};
	double phase = (index % 12) * 0.5235987756;
	char phase_text[G_ASCII_DTOSTR_BUF_SIZE];
	g_ascii_dtostr (phase_text, sizeof phase_text, phase);
	return g_strdup_printf (
		"[preset00]\n"
		"fDecay=0.97\nfGammaAdj=1.0\nfVideoEchoZoom=1.006\nfVideoEchoAlpha=0.18\n"
		"nVideoEchoOrientation=%u\nnWaveMode=%u\nfWaveAlpha=0.85\nfWaveScale=0.85\n"
		"bAdditiveWaves=1\nbWaveDots=0\nbWaveThick=1\nbMaximizeWaveColor=1\n"
		"bModWaveAlphaByVolume=1\nfModWaveAlphaStart=0.1\nfModWaveAlphaEnd=1.5\n"
		"zoom=1.02\nrot=0.008\nwave_x=0.5\nwave_y=0.5\n"
		"per_frame_1=q1=%s; wave_r=0.5+0.45*sin(time*0.17+q1);"
		"wave_g=0.5+0.45*sin(time*0.13+q1+2.1);wave_b=0.5+0.45*sin(time*0.11+q1+4.2);\n"
		"per_frame_2=wave_x=0.5+0.12*sin(time*0.21);wave_y=0.5+0.12*cos(time*0.19);"
		"wave_scale=0.6+0.22*min(bass_att,2);decay=0.965;\n"
		"per_pixel_1=%s\n"
		"shapecode_0_enabled=1\nshapecode_0_sides=%u\nshapecode_0_additive=1\n"
		"shapecode_0_thickOutline=1\nshapecode_0_textured=0\nshapecode_0_a=0.08\n"
		"shapecode_0_a2=0.015\nshapecode_0_border_a=0.6\n"
		"shape_0_per_frame1=x=0.5+0.24*sin(time*0.27+q1);y=0.5+0.24*cos(time*0.23+q1);"
		"rad=0.06+0.03*min(bass_att,2);ang=time*0.18;"
		"r=wave_b;g=wave_r;b=wave_g;r2=wave_r;g2=wave_g;b2=wave_b;"
		"border_r=wave_r;border_g=wave_g;border_b=wave_b;\n",
		index % 4, index % 3 == 0 ? 7 : index % 3 == 1 ? 1 : 6,
		phase_text, fields[index % G_N_ELEMENTS (fields)], 3 + index % 6);
}

static void
visual_preset_failed (const char *filename, const char *message, void *data)
{
	VisualEngine *gpu = data;
	g_free (gpu->error);
	gpu->error = g_strdup (message);
}

static gboolean
visual_engine_init (VisualEngine *gpu)
{
	gpu->display = eglGetDisplay (EGL_DEFAULT_DISPLAY);
	if (gpu->display == EGL_NO_DISPLAY || !eglInitialize (gpu->display, NULL, NULL)) {
		gpu->error = g_strdup (_("Cannot initialize EGL for projectM visualization."));
		return FALSE;
	}
	EGLint attributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLConfig config;
	EGLint count;
	EGLint size[] = { EGL_WIDTH, VISUAL_WIDTH, EGL_HEIGHT, VISUAL_HEIGHT, EGL_NONE };
	EGLint context[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
	if (!eglBindAPI (EGL_OPENGL_API) ||
	    !eglChooseConfig (gpu->display, attributes, &config, 1, &count) || count == 0) {
		gpu->error = g_strdup (_("No compatible OpenGL configuration for projectM."));
		return FALSE;
	}
	gpu->surface = eglCreatePbufferSurface (gpu->display, config, size);
	gpu->context = eglCreateContext (gpu->display, config, EGL_NO_CONTEXT, context);
	if (gpu->surface == EGL_NO_SURFACE || gpu->context == EGL_NO_CONTEXT ||
	    !eglMakeCurrent (gpu->display, gpu->surface, gpu->surface, gpu->context)) {
		gpu->error = g_strdup (_("Cannot create an OpenGL 3.3 context for projectM."));
		return FALSE;
	}
	static GMutex creation_lock;
	g_mutex_lock (&creation_lock);
	gpu->engine = projectm_create ();
	g_mutex_unlock (&creation_lock);
	if (gpu->engine == NULL) {
		gpu->error = g_strdup (_("projectM could not initialize its renderer."));
		return FALSE;
	}
	projectm_set_window_size (gpu->engine, VISUAL_WIDTH, VISUAL_HEIGHT);
	projectm_set_mesh_size (gpu->engine, 48, 32);
	projectm_set_fps (gpu->engine, 24);
	projectm_set_hard_cut_enabled (gpu->engine, FALSE);
	projectm_set_soft_cut_duration (gpu->engine, 4);
	projectm_set_preset_locked (gpu->engine, TRUE);
	projectm_set_preset_switch_failed_event_callback (gpu->engine, visual_preset_failed, gpu);
	gpu->preset = G_MAXUINT;
	return TRUE;
}
#endif

static void
visual_render_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	NemoMediaVisualizer *self = source;
	VisualEngine *gpu = &self->gpu;
	VisualFrame *request = data;
	if (g_task_return_error_if_cancelled (task))
		return;
	if (visual_presets[request->preset].kind != VISUAL_PROJECTM) {
		GError *error = NULL;
		cairo_surface_t *surface = native_render (self, request, &error);
		if (surface != NULL)
			g_task_return_pointer (task, surface, (GDestroyNotify) cairo_surface_destroy);
		else
			g_task_return_error (task, error);
		return;
	}
#ifdef HAVE_PROJECTM
	g_autofree guint8 *rgba = NULL;
	if (gpu->engine == NULL && !visual_engine_init (gpu))
		goto failed;
	if (!eglBindAPI (EGL_OPENGL_API) ||
	    !eglMakeCurrent (gpu->display, gpu->surface, gpu->surface, gpu->context)) {
		g_free (gpu->error);
		gpu->error = g_strdup (_("Cannot activate the projectM OpenGL context."));
		goto failed;
	}
	if (gpu->preset != request->preset) {
		GError *error = NULL;
		g_autofree char *preset = visual_preset (request->preset, &error);
		if (preset == NULL) {
			gpu->error = g_strdup (error->message);
			g_clear_error (&error);
			goto failed;
		}
		projectm_load_preset_data (gpu->engine, preset, gpu->preset != G_MAXUINT);
		gpu->preset = request->preset;
		if (gpu->error != NULL)
			goto failed;
	}
	projectm_pcm_add_float (gpu->engine, request->pcm, PCM_FRAMES, PROJECTM_STEREO);
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	projectm_opengl_render_frame (gpu->engine);
	/* projectM 4.1 can leave its internal read framebuffer bound. */
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	rgba = g_malloc (VISUAL_WIDTH * VISUAL_HEIGHT * 4);
	glReadPixels (0, 0, VISUAL_WIDTH, VISUAL_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	GLenum gl_error = glGetError ();
	eglMakeCurrent (gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (gl_error != GL_NO_ERROR) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
			"projectM OpenGL rendering error: 0x%x", gl_error);
		return;
	}
	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, VISUAL_WIDTH, VISUAL_HEIGHT);
	if (cairo_surface_status (surface) != CAIRO_STATUS_SUCCESS) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s",
			cairo_status_to_string (cairo_surface_status (surface)));
		cairo_surface_destroy (surface);
		return;
	}
	guint32 *pixels = (guint32 *) cairo_image_surface_get_data (surface);
	for (guint y = 0; y < VISUAL_HEIGHT; y++)
		for (guint x = 0; x < VISUAL_WIDTH; x++) {
			guint i = ((VISUAL_HEIGHT - 1 - y) * VISUAL_WIDTH + x) * 4;
			/* Remove the near-black feedback pedestal, keeping full-white output. */
			guint rgb[3];
			for (guint c = 0; c < 3; c++)
				rgb[c] = MAX (0, (gint) rgba[i + c] - 12) * 255 / 243;
			pixels[y * VISUAL_WIDTH + x] = 0xff000000 | (rgb[0] << 16) | (rgb[1] << 8) | rgb[2];
		}
	cairo_surface_mark_dirty (surface);
	g_task_return_pointer (task, surface, (GDestroyNotify) cairo_surface_destroy);
	return;
failed:
	if (gpu->display != EGL_NO_DISPLAY)
		eglMakeCurrent (gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
#else
	g_free (gpu->error);
	gpu->error = g_strdup (_("This build has no projectM 4 support. Playback remains available."));
#endif
	g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", gpu->error);
}

static gboolean
visual_render_release (gpointer unused)
{
	return G_SOURCE_REMOVE;
}

static void
visual_render_dispatch (gpointer data, gpointer unused)
{
	GTask *task = data;
	visual_render_worker (task, g_task_get_source_object (task),
		g_task_get_task_data (task), g_task_get_cancellable (task));
	/* The final task reference can finalize its GTK source object. */
	GSource *release = g_idle_source_new ();
	g_source_set_callback (release, visual_render_release, task, g_object_unref);
	g_source_attach (release, g_task_get_context (task));
	g_source_unref (release);
}

static void visual_request_frame (NemoMediaVisualizer *self);

static void
visual_application_shutdown (GApplication *application, gpointer unused)
{
	visual_shutting_down = TRUE;
	if (visual_pool != NULL) {
		/* Explicit application quit bypasses holds. Finish OpenGL work before
		 * projectM's process-global destructors can run. */
		g_thread_pool_free (visual_pool, FALSE, TRUE);
		visual_pool = NULL;
	}
}

static void
visual_render_ready (GObject *source, GAsyncResult *result, gpointer unused)
{
	NemoMediaVisualizer *self = NEMO_MEDIA_VISUALIZER (source);
	GTask *task = G_TASK (result);
	VisualFrame *request = g_task_get_task_data (task);
	GError *error = NULL;
	cairo_surface_t *surface = g_task_propagate_pointer (task, &error);
	self->rendering = FALSE;
	if (!self->destroyed && self->render_cancel == g_task_get_cancellable (task)) {
		g_clear_object (&self->render_cancel);
		if (surface != NULL && request->preset == self->preset && request->reset_serial == self->reset_serial) {
			g_clear_pointer (&self->frame, cairo_surface_destroy);
			self->frame = g_steal_pointer (&surface);
			self->rendered_frames++;
		} else if (error != NULL && request->preset == self->preset &&
			   request->reset_serial == self->reset_serial &&
			   !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			self->render_error = g_strdup (error->message);
			g_debug ("Media visualization: %s", error->message);
		}
	}
	if (!self->destroyed)
		gtk_widget_queue_draw (GTK_WIDGET (self));
	if (!self->destroyed && request->preset != self->preset)
		visual_request_frame (self);
	g_clear_pointer (&surface, cairo_surface_destroy);
	g_clear_error (&error);
}

static void
visual_request_frame (NemoMediaVisualizer *self)
{
	static gsize initialized;
	if (self->rendering || self->destroyed || visual_shutting_down || self->render_error != NULL ||
	    !gtk_widget_get_mapped (GTK_WIDGET (self)))
		return;
	if (g_once_init_enter (&initialized)) {
		GError *error = NULL;
		visual_pool = g_thread_pool_new (visual_render_dispatch, NULL, 2, TRUE, &error);
		if (error != NULL) {
			self->render_error = g_strdup (error->message);
			g_warning ("Media visualization workers: %s", error->message);
			g_clear_error (&error);
			g_thread_pool_free (visual_pool, TRUE, TRUE);
			visual_pool = NULL;
		}
		g_once_init_leave (&initialized, 1);
	}
	if (visual_pool == NULL) {
		if (self->render_error == NULL)
			self->render_error = g_strdup (_("Visualization workers are unavailable."));
		return;
	}
	if (self->application == NULL) {
		GApplication *application = g_application_get_default ();
		if (application != NULL && g_application_get_is_registered (application)) {
			self->application = g_object_ref (application);
			g_application_hold (application);
			if (g_object_get_data (G_OBJECT (application), "nemo-visual-shutdown") == NULL) {
				g_object_set_data (G_OBJECT (application), "nemo-visual-shutdown", GINT_TO_POINTER (1));
				g_signal_connect (application, "shutdown", G_CALLBACK (visual_application_shutdown), NULL);
			}
		}
	}
	VisualFrame *request = g_new0 (VisualFrame, 1);
	request->preset = self->preset;
	request->reset_serial = self->reset_serial;
	request->pcm_time = self->last_pcm;
	memcpy (request->analysis_pcm, self->analysis_pcm, sizeof request->analysis_pcm);
	if (g_get_monotonic_time () - self->last_pcm < 300000)
		memcpy (request->pcm, self->pcm, sizeof request->pcm);
	self->render_cancel = g_cancellable_new ();
	GTask *task = g_task_new (self, self->render_cancel, visual_render_ready, NULL);
	g_task_set_task_data (task, request, g_free);
	self->rendering = TRUE;
	GError *error = NULL;
	if (!g_thread_pool_push (visual_pool, task, &error)) {
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
	double dt = self->last_frame == 0 ? 0 : CLAMP ((now - self->last_frame) / 1000000.0, 0, 0.1);
	self->last_frame = now;
	if (self->automatic)
		self->preset_time += dt;
	if (self->automatic && self->preset_time >= 30)
		nemo_media_visualizer_next (self);
	visual_request_frame (self);
	return G_SOURCE_CONTINUE;
}

static void
visualizer_update_tick (NemoMediaVisualizer *self)
{
	gboolean animate;
	g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)), "gtk-enable-animations", &animate, NULL);
	gboolean active = self->playing && !self->destroyed && animate &&
		gtk_widget_get_mapped (GTK_WIDGET (self)) && self->render_error == NULL;
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
	cairo_set_source_rgb (cr, 0, 0, 0);
	cairo_paint (cr);
	if (self->frame == NULL && self->render_error == NULL)
		visual_request_frame (self);
	if (self->frame != NULL) {
		cairo_save (cr);
		double scale = MIN (width / VISUAL_WIDTH, height / VISUAL_HEIGHT);
		cairo_translate (cr, (width - VISUAL_WIDTH * scale) / 2, (height - VISUAL_HEIGHT * scale) / 2);
		cairo_scale (cr, scale, scale);
		cairo_set_source_surface (cr, self->frame, 0, 0);
		cairo_paint (cr);
		cairo_restore (cr);
	}
	if (self->render_error != NULL) {
		cairo_set_source_rgb (cr, 1, 1, 1);
		PangoLayout *layout = gtk_widget_create_pango_layout (widget, self->render_error);
		pango_layout_set_width (layout, MAX (1, width - 32) * PANGO_SCALE);
		cairo_move_to (cr, 16, 16);
		pango_cairo_show_layout (cr, layout);
		g_object_unref (layout);
		visualizer_update_tick (self);
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
#ifdef HAVE_PROJECTM
	VisualEngine *gpu = &self->gpu;
	EGLDisplay previous_display = eglGetCurrentDisplay ();
	EGLContext previous_context = eglGetCurrentContext ();
	EGLSurface previous_draw = eglGetCurrentSurface (EGL_DRAW);
	EGLSurface previous_read = eglGetCurrentSurface (EGL_READ);
	EGLenum previous_api = eglQueryAPI ();
	if (gpu->display != EGL_NO_DISPLAY) {
		if (gpu->context != EGL_NO_CONTEXT && gpu->surface != EGL_NO_SURFACE &&
		    eglBindAPI (EGL_OPENGL_API) &&
		    eglMakeCurrent (gpu->display, gpu->surface, gpu->surface, gpu->context)) {
			if (gpu->engine != NULL)
				projectm_destroy (gpu->engine);
			eglMakeCurrent (gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		} else if (gpu->engine != NULL) {
			g_warning ("Cannot activate projectM context during renderer cleanup.");
		}
		if (gpu->context != EGL_NO_CONTEXT)
			eglDestroyContext (gpu->display, gpu->context);
		if (gpu->surface != EGL_NO_SURFACE)
			eglDestroySurface (gpu->display, gpu->surface);
		eglBindAPI (previous_api);
		if (previous_display != EGL_NO_DISPLAY && previous_context != EGL_NO_CONTEXT &&
		    !eglMakeCurrent (previous_display, previous_draw, previous_read, previous_context))
			g_warning ("Cannot restore the OpenGL context after projectM cleanup.");
		/* The EGL display is shared across players; terminating it would
		 * invalidate another player's context. Each context/surface is owned. */
	}
#endif
	g_clear_pointer (&self->frame, cairo_surface_destroy);
	g_clear_pointer (&self->native.history, cairo_surface_destroy);
	if (self->native.fft != NULL)
		gst_fft_f32_free (self->native.fft);
	g_clear_object (&self->render_cancel);
	g_free (self->gpu.error);
	g_free (self->render_error);
	if (self->application != NULL) {
		g_application_release (self->application);
		g_clear_object (&self->application);
	}
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
	self->automatic = TRUE;
	gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
	gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
	atk_object_set_name (gtk_widget_get_accessible (GTK_WIDGET (self)), _("Music visualization"));
	g_signal_connect_object (gtk_widget_get_settings (GTK_WIDGET (self)),
		"notify::gtk-enable-animations", G_CALLBACK (visualizer_update_tick), self, G_CONNECT_SWAPPED);
}

void
nemo_media_visualizer_next (NemoMediaVisualizer *self)
{
	self->preset = (self->preset + 1) % G_N_ELEMENTS (visual_presets);
	self->preset_time = 0;
	gtk_widget_queue_draw (GTK_WIDGET (self));
	if (!self->rendering)
		visual_request_frame (self);
}

void
nemo_media_visualizer_set_preset (NemoMediaVisualizer *self, gint index)
{
	g_return_if_fail (index >= -1 && index < (gint) G_N_ELEMENTS (visual_presets));
	self->automatic = index == -1;
	if (index >= 0)
		self->preset = index;
	g_clear_pointer (&self->render_error, g_free);
	self->preset_time = 0;
	visualizer_update_tick (self);
	gtk_widget_queue_draw (GTK_WIDGET (self));
	if (!self->rendering)
		visual_request_frame (self);
}

void
nemo_media_visualizer_reset (NemoMediaVisualizer *self)
{
	nemo_media_visualizer_set_playing (self, FALSE);
	memset (self->pcm, 0, sizeof self->pcm);
	memset (self->analysis_pcm, 0, sizeof self->analysis_pcm);
	self->reset_serial++;
	self->last_pcm = 0;
	if (self->render_cancel != NULL) {
		g_cancellable_cancel (self->render_cancel);
		g_clear_object (&self->render_cancel);
	}
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
	if (structure == NULL || !gst_structure_has_name (structure, "nemo-music-pcm"))
		return;
	const GValue *value = gst_structure_get_value (structure, "samples");
	if (value == NULL || !GST_VALUE_HOLDS_BUFFER (value))
		return;
	GstBuffer *buffer = gst_value_get_buffer (value);
	if (gst_buffer_get_size (buffer) != sizeof self->pcm)
		return;
	gst_buffer_extract (buffer, 0, self->pcm, sizeof self->pcm);
	value = gst_structure_get_value (structure, "analysis");
	if (value != NULL && GST_VALUE_HOLDS_BUFFER (value)) {
		buffer = gst_value_get_buffer (value);
		if (gst_buffer_get_size (buffer) == sizeof self->analysis_pcm)
			gst_buffer_extract (buffer, 0, self->analysis_pcm, sizeof self->analysis_pcm);
	}
	self->last_pcm = g_get_monotonic_time ();
}
