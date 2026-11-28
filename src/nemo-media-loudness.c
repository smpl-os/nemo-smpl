/* Whole-track, bounded-memory EBU R128 analysis. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include "nemo-media-loudness.h"
#include "nemo-preview-utils.h"
#include <libnemo-private/nemo-file-utilities.h>
#include <gst/app/gstappsink.h>
#include <gst/audio/audio-format.h>
#include <ebur128.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <math.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_REQUESTS 8
#define ANALYSIS_VERSION 2
#define TRUE_PEAK_TAIL_FRAMES 64
typedef struct {
	GFile *file;
	GApplication *application;
	gboolean analyze;
} LoudnessRequest;

/* Only the dedicated thread decodes; the shared GTask pool does metadata I/O
 * only and must never be reniced. The queue is bounded across all players. */
static GThread *scan_thread;
static GAsyncQueue *scan_queue;
static GPtrArray *scan_tasks;
static gint stop_token;

static void
loudness_application_shutdown (GApplication *application, gpointer unused)
{
	nemo_media_loudness_shutdown ();
}

static void
request_free (gpointer data)
{
	LoudnessRequest *request = data;
	g_object_unref (request->file);
	g_free (request);
}

static char *
stat_identity (const struct stat *info, GError **error)
{
	struct stat st = *info;
	if (!S_ISREG (st.st_mode)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			_("Loudness analysis supports local regular files only."));
		return NULL;
	}
	return g_strdup_printf ("%" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT
		":%" G_GINT64_FORMAT ":%" G_GINT64_FORMAT ":%ld:%" G_GINT64_FORMAT ":%ld",
		(guint64) st.st_dev, (guint64) st.st_ino, (gint64) st.st_size,
		(gint64) st.st_mtim.tv_sec, st.st_mtim.tv_nsec,
		(gint64) st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
}

static char *
file_identity (const char *path, GError **error)
{
	struct stat st;
	if (g_stat (path, &st) != 0) {
		int saved_errno = errno;
		g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
			_("Cannot inspect loudness input: %s"), g_strerror (saved_errno));
		return NULL;
	}
	return stat_identity (&st, error);
}

static char *
fd_identity (int fd, GError **error)
{
	struct stat st;
	if (fstat (fd, &st) != 0) {
		g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
			_("Cannot inspect loudness input: %s"), g_strerror (errno));
		return NULL;
	}
	return stat_identity (&st, error);
}

static void
calculate_gain (NemoMediaLoudness *result)
{
	result->gain = 1;
	if (result->true_peak == 0)
		return;
	/* No integrated reading (e.g. a clip shorter than 400 ms) cannot justify
	 * a boost, but still needs attenuation if its measured peak is unsafe. */
	double target_db = result->silent ? 0 : -16 - result->lufs;
	double ceiling_db = -1 - 20 * log10 (result->true_peak);
	result->peak_limited = ceiling_db < target_db;
	result->gain = pow (10, MIN (target_db, ceiling_db) / 20);
}

static NemoMediaLoudness *
cache_read (const char *cache, const char *identity)
{
	g_autoptr (GKeyFile) key = g_key_file_new ();
	g_autoptr (GError) error = NULL;
	g_autofree char *stored = NULL;
	if (!g_key_file_load_from_file (key, cache, G_KEY_FILE_NONE, &error)) {
		if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			g_debug ("Cannot read loudness cache %s: %s", cache, error->message);
		return NULL;
	}
	gint version = g_key_file_get_integer (key, "r128", "analysis-version", &error);
	if (error != NULL)
		goto invalid;
	if (version != ANALYSIS_VERSION) {
		g_debug ("Ignoring incompatible loudness cache analysis version: %s", cache);
		return NULL;
	}
	stored = g_key_file_get_string (key, "r128", "identity", &error);
	if (error != NULL)
		goto invalid;
	if (g_strcmp0 (stored, identity) != 0)
		return NULL;
	NemoMediaLoudness value = { .cached = TRUE };
	value.silent = g_key_file_get_boolean (key, "r128", "silent", &error);
	if (error != NULL)
		goto invalid;
	value.lufs = g_key_file_get_double (key, "r128", "lufs", &error);
	if (error != NULL)
		goto invalid;
	value.true_peak = g_key_file_get_double (key, "r128", "peak", &error);
	if (error != NULL || !isfinite (value.lufs) || !isfinite (value.true_peak) ||
	    value.true_peak < 0 || (!value.silent && value.true_peak == 0))
		goto invalid;
	calculate_gain (&value);
	if (!isfinite (value.gain) || value.gain <= 0)
		goto invalid;
	NemoMediaLoudness *result = g_new (NemoMediaLoudness, 1);
	*result = value;
	return result;
invalid:
	g_debug ("Ignoring corrupt loudness cache %s: %s", cache,
		error != NULL ? error->message : "Invalid loudness or true-peak measurement");
	return NULL;
}

static gboolean
cache_write (const char *cache, const char *identity,
             NemoMediaLoudness *result, GError **error)
{
	g_autofree char *directory = g_path_get_dirname (cache);
	if (g_mkdir_with_parents (directory, 0700) != 0) {
		g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
			_("Cannot create the loudness cache: %s"), g_strerror (errno));
		return FALSE;
	}
	g_autoptr (GKeyFile) key = g_key_file_new ();
	g_key_file_set_integer (key, "r128", "analysis-version", ANALYSIS_VERSION);
	g_key_file_set_string (key, "r128", "identity", identity);
	g_key_file_set_boolean (key, "r128", "silent", result->silent);
	g_key_file_set_double (key, "r128", "lufs", result->lufs);
	g_key_file_set_double (key, "r128", "peak", result->true_peak);
	gsize length;
	g_autofree char *contents = g_key_file_to_data (key, &length, NULL);
	/* Consistent replacement: a partial result is never a cache hit. */
	return g_file_set_contents (cache, contents, length, error);
}

static void
audio_pad_added (GstElement *decoder, GstPad *pad, gpointer data)
{
	GstElement *convert = data;
	g_autoptr (GstCaps) caps = gst_pad_get_current_caps (pad);
	if (caps == NULL || gst_caps_is_empty (caps) ||
	    !g_str_has_prefix (gst_structure_get_name (gst_caps_get_structure (caps, 0)), "audio/x-raw"))
		return;
	GstPad *sink = gst_element_get_static_pad (convert, "sink");
	if (!gst_pad_is_linked (sink))
		gst_pad_link (pad, sink);
	gst_object_unref (sink);
}

/* Decodebin stops at encoded video pads, so video decoders are never started. */
static gboolean
audio_autoplug_continue (GstElement *decoder, GstPad *pad, GstCaps *caps, gpointer unused)
{
	const char *name = gst_structure_get_name (gst_caps_get_structure (caps, 0));
	if (!g_str_has_prefix (name, "video/") && !g_str_has_prefix (name, "image/"))
		return TRUE;
	/* Some containers (QuickTime, Matroska, MPEG PS/TS) use video media types.
	 * Let their demuxers run, but never proceed into a video decoder. */
	GList *factories = gst_element_factory_list_get_elements (
		GST_ELEMENT_FACTORY_TYPE_DEMUXER, GST_RANK_MARGINAL);
	GList *matching = gst_element_factory_list_filter (factories, caps, GST_PAD_SINK, FALSE);
	gboolean container = matching != NULL;
	gst_plugin_feature_list_free (matching);
	gst_plugin_feature_list_free (factories);
	return container;
}

static void
audio_no_more_pads (GstElement *decoder, gpointer data)
{
	GstPad *sink = gst_element_get_static_pad (data, "sink");
	if (!gst_pad_is_linked (sink))
		GST_ELEMENT_ERROR (decoder, STREAM, WRONG_TYPE,
			("No supported audio stream was found for loudness analysis"), (NULL));
	gst_object_unref (sink);
}

static gint
audio_autoplug_select (GstElement *decoder, GstPad *pad, GstCaps *caps,
                       GstElementFactory *factory, gpointer unused)
{
	const char *klass = gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_KLASS);
	/* In particular, never instantiate HLS/DASH demuxers that could fetch
	 * external URIs from an otherwise local manifest. */
	if (!nemo_preview_media_decoder_is_safe (factory) ||
	    klass == NULL || strstr (klass, "Adaptive") != NULL ||
	    strstr (klass, "Network") != NULL || strstr (klass, "Source") != NULL ||
	    (strstr (klass, "Decoder") != NULL &&
	     (strstr (klass, "Video") != NULL || strstr (klass, "Image") != NULL)))
		return 2; /* GST_AUTOPLUG_SELECT_SKIP (decodebin's private enum). */
	return 0;
}

static NemoMediaLoudness *
scan_audio (int fd, GCancellable *cancel, GError **error)
{
	GstElement *pipeline = gst_pipeline_new (NULL);
	GstElement *source = gst_element_factory_make ("fdsrc", NULL);
	GstElement *decoder = gst_element_factory_make ("decodebin", NULL);
	GstElement *convert = gst_element_factory_make ("audioconvert", NULL);
	GstElement *sink = gst_element_factory_make ("appsink", NULL);
	GstElement *elements[] = { source, decoder, convert, sink };
	NemoMediaLoudness *result = NULL;
	ebur128_state *meter = NULL;
	GstBus *bus = NULL;
	guint64 frames = 0;
	gboolean complete = FALSE;
	gint rate = 0;
	for (guint i = 0; i < G_N_ELEMENTS (elements); i++) {
		if (elements[i] == NULL) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				_("Required GStreamer loudness analysis plugins are unavailable."));
			for (guint j = 0; j < G_N_ELEMENTS (elements); j++)
				if (elements[j] != NULL)
					gst_object_unref (elements[j]);
			gst_object_unref (pipeline);
			return NULL;
		}
	}
	gst_bin_add_many (GST_BIN (pipeline), source, decoder, convert, sink, NULL);
	g_object_set (source, "fd", fd, NULL);
	g_autoptr (GstCaps) caps = gst_caps_from_string (
		"audio/x-raw,format=" GST_AUDIO_NE (F32) ",channels=2,rate=(int)[8000,192000],layout=interleaved");
	g_object_set (sink, "caps", caps, "sync", FALSE, "max-buffers", 2,
		"enable-last-sample", FALSE, "wait-on-eos", FALSE, NULL);
	g_signal_connect (decoder, "pad-added", G_CALLBACK (audio_pad_added), convert);
	g_signal_connect (decoder, "autoplug-continue", G_CALLBACK (audio_autoplug_continue), NULL);
	g_signal_connect (decoder, "autoplug-select", G_CALLBACK (audio_autoplug_select), NULL);
	g_signal_connect (decoder, "no-more-pads", G_CALLBACK (audio_no_more_pads), convert);
	if (!gst_element_link (source, decoder) ||
	    !gst_element_link (convert, sink)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Unable to connect the loudness analysis pipeline."));
		goto out;
	}
	bus = gst_element_get_bus (pipeline);
	if (gst_element_set_state (pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Unable to decode audio for loudness analysis."));
		goto out;
	}
	gint64 last_progress = g_get_monotonic_time ();
	while (!g_cancellable_set_error_if_cancelled (cancel, error)) {
		GstSample *sample = gst_app_sink_try_pull_sample (GST_APP_SINK (sink), 100 * GST_MSECOND);
		if (sample != NULL) {
			gint sample_rate = 0;
			GstCaps *sample_caps = gst_sample_get_caps (sample);
			if (sample_caps == NULL ||
			    !gst_structure_get_int (gst_caps_get_structure (sample_caps, 0), "rate", &sample_rate) ||
			    sample_rate < 8000 || sample_rate > 192000 || (rate != 0 && rate != sample_rate)) {
				gst_sample_unref (sample);
				g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
					_("Unsupported or changing audio sample rate during loudness analysis."));
				break;
			}
			if (meter == NULL) {
				rate = sample_rate;
				meter = ebur128_init (2, rate,
					EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK | EBUR128_MODE_HISTOGRAM);
				if (meter == NULL) {
					gst_sample_unref (sample);
					g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
						_("Unable to allocate the loudness analyzer."));
					break;
				}
			}
			GstBuffer *buffer = gst_sample_get_buffer (sample);
			GstMapInfo map;
			gboolean valid = gst_buffer_map (buffer, &map, GST_MAP_READ);
			if (valid) {
				valid = map.size % (2 * sizeof (float)) == 0;
				if (valid) {
					const float *pcm = (const float *) map.data;
					for (gsize i = 0; i < map.size / sizeof (float); i++)
						if (!isfinite (pcm[i])) {
							valid = FALSE;
							break;
						}
					gsize count = map.size / (2 * sizeof (float));
					if (valid) {
						valid = ebur128_add_frames_float (meter, pcm, count) == EBUR128_SUCCESS;
						frames += count;
					}
				}
				gst_buffer_unmap (buffer, &map);
			}
			gst_sample_unref (sample);
			if (!valid) {
				g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					_("Invalid audio samples during loudness analysis."));
				break;
			}
			last_progress = g_get_monotonic_time ();
		}
		GstMessage *message = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
		if (message != NULL) {
			gst_message_parse_error (message, error, NULL);
			gst_message_unref (message);
			break;
		}
		if (sample == NULL && gst_app_sink_is_eos (GST_APP_SINK (sink))) {
			complete = TRUE;
			break;
		}
		if (g_get_monotonic_time () - last_progress > 30 * G_TIME_SPAN_SECOND) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
				_("Audio decoding stalled during loudness analysis."));
			break;
		}
	}
	if (complete && frames > 0) {
		result = g_new0 (NemoMediaLoudness, 1);
		double left, right;
		/* Read loudness before padding: trailing zeros must not create extra
		 * gating blocks. libebur128 1.2.6 uses a 49-tap FIR interpolator and
		 * true_peak() does not flush it; 64 input frames drain both 2x/4x
		 * delay lines, including peaks at the very end of a file. */
		const float tail[TRUE_PEAK_TAIL_FRAMES * 2] = { 0 };
		if (ebur128_loudness_global (meter, &result->lufs) != EBUR128_SUCCESS ||
		    ebur128_add_frames_float (meter, tail, TRUE_PEAK_TAIL_FRAMES) != EBUR128_SUCCESS ||
		    ebur128_true_peak (meter, 0, &left) != EBUR128_SUCCESS ||
		    ebur128_true_peak (meter, 1, &right) != EBUR128_SUCCESS) {
			g_clear_pointer (&result, g_free);
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
				_("Unable to measure integrated audio loudness."));
			goto out;
		}
		result->true_peak = MAX (left, right);
		if (isnan (result->lufs) || result->lufs == HUGE_VAL ||
		    !isfinite (result->true_peak) || result->true_peak < 0) {
			g_clear_pointer (&result, g_free);
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				_("Invalid integrated loudness or true-peak measurement."));
			goto out;
		}
		result->silent = !isfinite (result->lufs) || result->true_peak == 0;
		if (result->silent)
			result->lufs = 0;
		calculate_gain (result);
	} else if (complete) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			_("No decodable audio was found for loudness analysis."));
	}
out:
	/* Complete teardown before returning a result or releasing an app hold. */
	gst_element_set_state (pipeline, GST_STATE_NULL);
	gst_object_unref (pipeline);
	if (bus != NULL)
		gst_object_unref (bus);
	if (meter != NULL)
		ebur128_destroy (&meter);
	return result;
}

static void
request_run (GTask *task, gpointer unused, gpointer task_data, GCancellable *cancel)
{
	LoudnessRequest *request = task_data;
	g_autoptr (GError) error = NULL;
	g_autofree char *path = g_file_get_path (request->file);
	g_autofree char *identity = NULL;
	NemoMediaLoudness *result = NULL;
	if (g_task_return_error_if_cancelled (task))
		return;
	if (path == NULL) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"%s", _("Loudness analysis supports local regular files only."));
		return;
	}
	identity = file_identity (path, &error);
	if (identity == NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}
	g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
	g_autofree char *cache = g_build_filename (g_get_user_cache_dir (), "nemo", "loudness-v1", hash, NULL);
	result = cache_read (cache, identity);
	if (result != NULL) {
		g_autofree char *after = file_identity (path, &error);
		if (g_strcmp0 (after, identity) != 0)
			g_clear_pointer (&result, g_free);
		if (error != NULL) {
			g_task_return_error (task, g_steal_pointer (&error));
			return;
		}
	}
	if (result == NULL && request->analyze) {
		/* Pin the actual input inode, not just its pathname. O_NONBLOCK
		 * prevents a pathname replaced with a FIFO from hanging open(). */
		int fd = g_open (path, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
		if (fd < 0)
			g_set_error (&error, G_IO_ERROR, g_io_error_from_errno (errno),
				_("Cannot open loudness input: %s"), g_strerror (errno));
		g_autofree char *opened = fd >= 0 ? fd_identity (fd, &error) : NULL;
		if (opened != NULL && !g_str_equal (opened, identity))
			g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_BUSY,
				_("The file changed before loudness analysis. Play it again to retry."));
		if (error == NULL)
			result = scan_audio (fd, cancel, &error);
		if (result != NULL) {
			g_autofree char *read_identity = fd_identity (fd, &error);
			g_autofree char *after = error == NULL ? file_identity (path, &error) : NULL;
			if (error == NULL && (g_strcmp0 (after, identity) != 0 ||
			                     g_strcmp0 (read_identity, identity) != 0))
				g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_BUSY,
					_("The file changed during loudness analysis. Play it again to retry."));
			if (error == NULL && !g_cancellable_set_error_if_cancelled (cancel, &error))
				cache_write (cache, identity, result, &error);
			if (error != NULL)
				g_clear_pointer (&result, g_free);
			else
				result->cached = TRUE;
		}
		if (fd >= 0)
			close (fd);
	}
	if (error != NULL) {
		g_task_return_error (task, g_steal_pointer (&error));
		return;
	}
	if (result == NULL) {
		result = g_new0 (NemoMediaLoudness, 1);
		result->gain = 1;
	}
	g_task_return_pointer (task, result, g_free);
}

static gpointer
scan_worker (gpointer unused)
{
	nemo_thread_set_background_priority ();
	for (;;) {
		gpointer item = g_async_queue_pop (scan_queue);
		if (item == &stop_token)
			break;
		GTask *task = item;
		request_run (task, NULL, g_task_get_task_data (task), g_task_get_cancellable (task));
		g_object_unref (task);
	}
	return NULL;
}

static void
request_completed (GObject *source, GAsyncResult *result, gpointer data)
{
	/* GTask owns the actual caller callback separately, via task object data. */
	GTask *task = G_TASK (result);
	LoudnessRequest *request = g_task_get_task_data (task);
	GApplication *application = g_steal_pointer (&request->application);
	if (request->analyze)
		g_ptr_array_remove (scan_tasks, task);
	GAsyncReadyCallback callback = g_object_get_data (G_OBJECT (task), "loudness-callback");
	if (callback != NULL)
		callback (source, result, data);
	/* The worker can drop the final task reference after this callback.
	 * Keep all application ownership/release on the creating UI context. */
	if (application != NULL) {
		g_application_release (application);
		g_object_unref (application);
	}
}

static void
request_async (GFile *file, GCancellable *cancel, gboolean analyze,
               GAsyncReadyCallback callback, gpointer data)
{
	g_autoptr (GCancellable) own_cancel = cancel == NULL ? g_cancellable_new () : g_object_ref (cancel);
	GTask *task = g_task_new (NULL, own_cancel, request_completed, data);
	g_object_set_data (G_OBJECT (task), "loudness-callback", callback);
	LoudnessRequest *request = g_new0 (LoudnessRequest, 1);
	request->file = g_object_ref (file);
	request->analyze = analyze;
	GApplication *application = g_application_get_default ();
	if (application != NULL && g_application_get_is_registered (application)) {
		request->application = g_object_ref (application);
		g_application_hold (application);
		if (g_object_get_data (G_OBJECT (application), "nemo-loudness-shutdown") == NULL) {
			g_object_set_data (G_OBJECT (application), "nemo-loudness-shutdown", GINT_TO_POINTER (1));
			g_signal_connect (application, "shutdown", G_CALLBACK (loudness_application_shutdown), NULL);
		}
	}
	g_task_set_task_data (task, request, request_free);
	if (!analyze) {
		g_task_run_in_thread (task, request_run);
		g_object_unref (task);
		return;
	}
	if (scan_tasks == NULL)
		scan_tasks = g_ptr_array_new ();
	g_ptr_array_add (scan_tasks, task);
	if (scan_tasks->len > MAX_REQUESTS) {
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_BUSY,
			"%s", _("Loudness analysis is busy. Play this track again to retry."));
		g_object_unref (task);
		return;
	}
	if (scan_thread == NULL) {
		g_autoptr (GError) error = NULL;
		if (scan_queue == NULL)
			scan_queue = g_async_queue_new ();
		scan_thread = g_thread_try_new ("nemo-loudness", scan_worker, NULL, &error);
		if (scan_thread == NULL) {
			g_task_return_error (task, g_steal_pointer (&error));
			g_object_unref (task);
			return;
		}
	}
	g_async_queue_push (scan_queue, task);
}

void
nemo_media_loudness_lookup_async (GFile *file, GCancellable *cancel,
                                 GAsyncReadyCallback callback, gpointer data)
{
	request_async (file, cancel, FALSE, callback, data);
}

void
nemo_media_loudness_analyze_async (GFile *file, GCancellable *cancel,
                                  GAsyncReadyCallback callback, gpointer data)
{
	request_async (file, cancel, TRUE, callback, data);
}

NemoMediaLoudness *
nemo_media_loudness_finish (GAsyncResult *result, GError **error)
{
	return g_task_propagate_pointer (G_TASK (result), error);
}

void
nemo_media_loudness_shutdown (void)
{
	if (scan_thread == NULL)
		return;
	for (guint i = 0; i < scan_tasks->len; i++)
		g_cancellable_cancel (g_task_get_cancellable (g_ptr_array_index (scan_tasks, i)));
	g_async_queue_push (scan_queue, &stop_token);
	g_thread_join (scan_thread);
	scan_thread = NULL;
}
