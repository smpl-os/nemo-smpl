/* Included by the isolated player regression, exercising the production scanner,
 * cache, native filter, playbin volume, and player lifecycle together. */
#include <ebur128.h>
#include <gst/app/gstappsrc.h>
#include <fcntl.h>
#include <sys/stat.h>

typedef struct {
	gboolean done;
	NemoMediaLoudness *measurement;
	GError *error;
} LoudnessCompletion;

static void
loudness_completed (GObject *source, GAsyncResult *result, gpointer data)
{
	LoudnessCompletion *completion = data;
	completion->measurement = nemo_media_loudness_finish (result, &completion->error);
	completion->done = TRUE;
}

static NemoMediaLoudness *
loudness_measure (GFile *file, gboolean analyze)
{
	LoudnessCompletion completion = { 0 };
	if (analyze)
		nemo_media_loudness_analyze_async (file, NULL, loudness_completed, &completion);
	else
		nemo_media_loudness_lookup_async (file, NULL, loudness_completed, &completion);
	WAIT_UNTIL (completion.done);
	g_assert_no_error (completion.error);
	g_assert_nonnull (completion.measurement);
	return completion.measurement;
}

static GFile *
loudness_fixture_full (const char *directory, const char *name, double amplitude,
                       gboolean high_crest, guint blocks, gboolean tail_peak)
{
	g_autofree char *path = g_build_filename (directory, name, NULL);
	g_autoptr (GError) error = NULL;
	GstElement *pipeline = gst_parse_launch (
		"appsrc name=input format=time ! wavenc ! filesink name=output", &error);
	g_assert_no_error (error);
	GstElement *source = gst_bin_get_by_name (GST_BIN (pipeline), "input");
	GstElement *output = gst_bin_get_by_name (GST_BIN (pipeline), "output");
	g_object_set (output, "location", path, NULL);
	gst_object_unref (output);
	GstCaps *caps = gst_caps_from_string (
		"audio/x-raw,format=" GST_AUDIO_NE (F32) ",channels=2,rate=48000,layout=interleaved");
	g_object_set (source, "caps", caps, NULL);
	gst_caps_unref (caps);
	g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
	for (guint block = 0; block < blocks; block++) {
		float pcm[480 * 2];
		for (guint frame = 0; frame < 480; frame++) {
			guint position = block * 480 + frame;
			float value = amplitude * sin (2 * G_PI * 1000 * position / 48000);
			if ((high_crest && position == 48000) ||
			    (tail_peak && position >= blocks * 480 - 2))
				value = 0.95;
			pcm[frame * 2] = pcm[frame * 2 + 1] = value;
		}
		GstBuffer *buffer = gst_buffer_new_allocate (NULL, sizeof pcm, NULL);
		gst_buffer_fill (buffer, 0, pcm, sizeof pcm);
		GST_BUFFER_PTS (buffer) = block * 10 * GST_MSECOND;
		GST_BUFFER_DURATION (buffer) = 10 * GST_MSECOND;
		g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (source), buffer), ==, GST_FLOW_OK);
	}
	g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (source)), ==, GST_FLOW_OK);
	GstBus *bus = gst_element_get_bus (pipeline);
	GstMessage *message = gst_bus_timed_pop_filtered (bus, 5 * GST_SECOND,
		GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
	g_assert_nonnull (message);
	if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
		gst_message_parse_error (message, &error, NULL);
		g_error ("Loudness fixture failed: %s", error->message);
	}
	gst_message_unref (message);
	gst_element_set_state (pipeline, GST_STATE_NULL);
	gst_object_unref (source);
	gst_object_unref (pipeline);
	gst_object_unref (bus);
	return g_file_new_for_path (path);
}

static GFile *
loudness_fixture (const char *directory, const char *name, double amplitude, gboolean high_crest)
{
	return loudness_fixture_full (directory, name, amplitude, high_crest, 300, FALSE);
}

static GArray *
loudness_playbin_output (GFile *file, double gain, double volume, double *lufs, double *peak)
{
	g_autoptr (GError) error = NULL;
	GstElement *pipeline = gst_element_factory_make ("playbin", NULL);
	GstElement *filter = nemo_media_audio_filter_new (&error);
	g_assert_no_error (error);
	nemo_media_audio_set_normalization_gain (filter, gain);
	GstElement *sink = gst_element_factory_make ("appsink", NULL);
	gst_object_ref_sink (sink);
	GstCaps *caps = gst_caps_from_string (
		"audio/x-raw,format=" GST_AUDIO_NE (F32) ",channels=2,rate=48000,layout=interleaved");
	g_object_set (sink, "caps", caps, "sync", FALSE, "max-buffers", 2,
		"enable-last-sample", FALSE, NULL);
	gst_caps_unref (caps);
	g_autofree char *uri = g_file_get_uri (file);
	g_object_set (pipeline, "uri", uri, "audio-filter", filter, "audio-sink", sink,
		"volume", volume, NULL);
	GArray *samples = g_array_new (FALSE, FALSE, sizeof (float));
	g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
	for (;;) {
		GstSample *sample = gst_app_sink_try_pull_sample (GST_APP_SINK (sink), 5 * GST_SECOND);
		if (sample == NULL) {
			g_assert_true (gst_app_sink_is_eos (GST_APP_SINK (sink)));
			break;
		}
		GstMapInfo map;
		g_assert_true (gst_buffer_map (gst_sample_get_buffer (sample), &map, GST_MAP_READ));
		g_array_append_vals (samples, map.data, map.size / sizeof (float));
		gst_buffer_unmap (gst_sample_get_buffer (sample), &map);
		gst_sample_unref (sample);
	}
	ebur128_state *meter = ebur128_init (2, 48000, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK |
		EBUR128_MODE_HISTOGRAM);
	g_assert_cmpint (ebur128_add_frames_float (meter, (const float *) samples->data,
		samples->len / 2), ==, EBUR128_SUCCESS);
	g_assert_cmpint (ebur128_loudness_global (meter, lufs), ==, EBUR128_SUCCESS);
	const float tail[64 * 2] = { 0 };
	g_assert_cmpint (ebur128_add_frames_float (meter, tail, 64), ==, EBUR128_SUCCESS);
	double right;
	g_assert_cmpint (ebur128_true_peak (meter, 0, peak), ==, EBUR128_SUCCESS);
	g_assert_cmpint (ebur128_true_peak (meter, 1, &right), ==, EBUR128_SUCCESS);
	*peak = MAX (*peak, right);
	ebur128_destroy (&meter);
	gst_element_set_state (pipeline, GST_STATE_NULL);
	gst_object_unref (pipeline);
	gst_object_unref (filter);
	gst_object_unref (sink);
	return samples;
}

static double
player_normalization_gain (NemoQuickPreview *player)
{
	GstElement *tones = gst_bin_get_by_name (GST_BIN (player->audio_filter), "binaural-tones");
	GST_OBJECT_LOCK (tones);
	double gain = ((NemoBinauralFilter *) tones)->normalization_gain;
	GST_OBJECT_UNLOCK (tones);
	gst_object_unref (tones);
	return gain;
}

static void
test_player_loudness_cache_and_playback (void)
{
	nemo_global_preferences_init ();
	if (g_test_subprocess ()) {
		GFile *persisted = g_file_new_for_path (g_getenv ("NEMO_TEST_LOUDNESS_CACHE_FILE"));
		g_autofree NemoMediaLoudness *cached = loudness_measure (persisted, FALSE);
		g_assert_true (cached->cached);
		g_assert_cmpfloat (cached->gain, >, 2);
		g_object_unref (persisted);
		return;
	}
	g_autofree char *directory = g_dir_make_tmp ("nemo-loudness-XXXXXX", NULL);
	GFile *file = loudness_fixture (directory, "quiet.wav", 0.02, FALSE);
	g_autofree char *path = g_file_get_path (file);
	g_autofree char *before = NULL;
	gsize length;
	g_assert_true (g_file_get_contents (path, &before, &length, NULL));
	g_autofree NemoMediaLoudness *miss = loudness_measure (file, FALSE);
	g_assert_false (miss->cached);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_MEDIA_PLAYER_NORMALIZE, TRUE);
	NemoQuickPreview *first = g_object_ref_sink (nemo_quick_preview_new_player ());
	nemo_quick_preview_show_file (first, file, NULL);
	WAIT_UNTIL (first->media_ready && first->video_playing);
	WAIT_UNTIL (g_str_has_prefix (gtk_label_get_text (GTK_LABEL (first->normalization_status)), "Ready for next play"));
	g_assert_cmpfloat (player_normalization_gain (first), ==, 1);
	g_autofree NemoMediaLoudness *hit = loudness_measure (file, FALSE);
	g_assert_true (hit->cached);
	g_assert_false (hit->silent);
	g_assert_false (hit->peak_limited);
	g_assert_cmpfloat (hit->gain, >, 2);
	g_setenv ("NEMO_TEST_LOUDNESS_CACHE_FILE", path, TRUE);
	g_test_trap_subprocess (NULL, 10 * G_TIME_SPAN_SECOND, G_TEST_SUBPROCESS_DEFAULT);
	g_test_trap_assert_passed ();
	g_unsetenv ("NEMO_TEST_LOUDNESS_CACHE_FILE");

	NemoQuickPreview *second = g_object_ref_sink (nemo_quick_preview_new_player ());
	nemo_quick_preview_show_file (second, file, NULL);
	WAIT_UNTIL (second->media_ready && second->video_playing);
	g_assert_cmpfloat_with_epsilon (player_normalization_gain (second), hit->gain, 0.000001);
	g_assert_true (g_str_has_prefix (gtk_label_get_text (GTK_LABEL (second->normalization_status)), "Normalized:"));
	g_assert_cmpfloat (player_normalization_gain (first), ==, 1);
	gtk_range_set_value (GTK_RANGE (second->volume_scale), 37);
	double volume;
	g_object_get (second->pipeline, "volume", &volume, NULL);
	g_assert_cmpfloat_with_epsilon (volume, 0.37, 0.00001);
	g_assert_cmpfloat_with_epsilon (player_normalization_gain (second), hit->gain, 0.000001);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (first->normalization_button), FALSE);
	g_assert_cmpfloat_with_epsilon (player_normalization_gain (second), hit->gain, 0.000001);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (first->normalization_button), TRUE);
	WAIT_UNTIL (g_str_has_prefix (gtk_label_get_text (GTK_LABEL (first->normalization_status)), "Ready for next play"));
	g_assert_cmpfloat (player_normalization_gain (first), ==, 1);

	double lufs, peak;
	GArray *normalized = loudness_playbin_output (file, hit->gain, 1, &lufs, &peak);
	g_assert_cmpfloat_with_epsilon (lufs, -16, 0.15);
	g_assert_cmpfloat (20 * log10 (peak), <=, -0.9);
	GArray *quieter = loudness_playbin_output (file, hit->gain, 0.5, &lufs, &peak);
	g_assert_cmpfloat_with_epsilon (lufs, -16 + 20 * log10 (0.5), 0.15);
	g_assert_cmpuint (quieter->len, ==, normalized->len);
	for (guint i = 0; i < quieter->len; i++)
		g_assert_cmpfloat_with_epsilon (g_array_index (quieter, float, i),
			g_array_index (normalized, float, i) * 0.5, 0.000001);
	g_array_unref (normalized);
	g_array_unref (quieter);
	g_autofree char *after = NULL;
	gsize after_length;
	g_assert_true (g_file_get_contents (path, &after, &after_length, NULL));
	g_assert_cmpmem (before, length, after, after_length);
	WAIT_UNTIL (first->player_finished);
	play_pause_clicked_cb (GTK_BUTTON (first->play_btn), first);
	WAIT_UNTIL (first->media_ready && first->video_playing);
	g_assert_cmpfloat_with_epsilon (player_normalization_gain (first), hit->gain, 0.000001);
	gtk_widget_destroy (GTK_WIDGET (first));
	gtk_widget_destroy (GTK_WIDGET (second));
	g_object_unref (first);
	g_object_unref (second);
	WAIT_UNTIL (g_atomic_int_get (&media_pipeline_count) == 0);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_MEDIA_PLAYER_NORMALIZE, FALSE);
	remove_fixture (file);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
test_player_loudness_peak_silence_invalidation (void)
{
	g_autofree char *directory = g_dir_make_tmp ("nemo-loudness-XXXXXX", NULL);
	GFile *crest = loudness_fixture (directory, "crest.wav", 0.02, TRUE);
	g_autofree NemoMediaLoudness *measurement = loudness_measure (crest, TRUE);
	g_assert_true (measurement->peak_limited);
	double lufs, peak;
	GArray *output = loudness_playbin_output (crest, measurement->gain, 1, &lufs, &peak);
	g_assert_cmpfloat (lufs, <, -20);
	g_assert_cmpfloat_with_epsilon (20 * log10 (peak), -1, 0.05);
	g_array_unref (output);
	g_autofree char *path = g_file_get_path (crest);
	struct stat before;
	g_assert_cmpint (g_stat (path, &before), ==, 0);
	int fd = g_open (path, O_WRONLY, 0);
	g_assert_cmpint (fd, >=, 0);
	float replacement[2] = { 0.3, 0.3 };
	g_usleep (2000);
	g_assert_cmpint (pwrite (fd, replacement, sizeof replacement, before.st_size - sizeof replacement),
		==, sizeof replacement);
	g_assert_cmpint (close (fd), ==, 0);
	struct timespec times[2] = { before.st_atim, before.st_mtim };
	g_assert_cmpint (utimensat (AT_FDCWD, path, times, 0), ==, 0);
	g_autofree NemoMediaLoudness *changed = loudness_measure (crest, FALSE);
	g_assert_false (changed->cached);
	GFile *silence = loudness_fixture (directory, "silence.wav", 0, FALSE);
	g_autofree NemoMediaLoudness *silent = loudness_measure (silence, TRUE);
	g_assert_true (silent->silent);
	g_assert_cmpfloat (silent->gain, ==, 1);
	g_autofree NemoMediaLoudness *silent_hit = loudness_measure (silence, FALSE);
	g_assert_true (silent_hit->cached);
	g_assert_true (silent_hit->silent);
	g_autofree char *video_path = preview_webm_codec_fixture ("vp8");
	GFile *video = g_file_new_for_path (video_path);
	GPtrArray *disabled = preview_disable_video_decoders ();
	g_autofree NemoMediaLoudness *video_audio = loudness_measure (video, TRUE);
	g_assert_true (video_audio->cached);
	g_assert_false (video_audio->silent);
	g_ptr_array_unref (disabled);
	remove_fixture (video);
	remove_fixture (crest);
	remove_fixture (silence);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
test_player_loudness_short_peak_and_corrupt_cache (void)
{
	nemo_global_preferences_init ();
	g_autofree char *directory = g_dir_make_tmp ("nemo-loudness-XXXXXX", NULL);
	/* Only the final two samples are loud: their intersample FIR peak is
	 * after EOF, and 100 ms is too short for an integrated LUFS reading. */
	GFile *file = loudness_fixture_full (directory, "short.wav", 0, FALSE, 10, TRUE);
	g_autofree NemoMediaLoudness *measurement = loudness_measure (file, TRUE);
	g_assert_true (measurement->silent);
	g_assert_true (measurement->peak_limited);
	g_assert_cmpfloat (measurement->true_peak, >, 1.1);
	g_assert_cmpfloat (measurement->gain, <, 1);
	double lufs, peak;
	GArray *output = loudness_playbin_output (file, measurement->gain, 1, &lufs, &peak);
	g_assert_true (isinf (lufs) && lufs < 0);
	g_assert_cmpfloat_with_epsilon (20 * log10 (peak), -1, 0.05);
	g_array_unref (output);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_MEDIA_PLAYER_NORMALIZE, TRUE);
	NemoQuickPreview *player = g_object_ref_sink (nemo_quick_preview_new_player ());
	nemo_quick_preview_show_file (player, file, NULL);
	WAIT_UNTIL (player->media_ready);
	const char *status = gtk_label_get_text (GTK_LABEL (player->normalization_status));
	g_assert_true (g_str_has_prefix (status, "Peak-limited:"));
	g_assert_nonnull (strstr (status, "LUFS unavailable"));
	g_assert_cmpfloat_with_epsilon (player_normalization_gain (player), measurement->gain, 0.000001);
	gtk_widget_destroy (GTK_WIDGET (player));
	g_object_unref (player);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_MEDIA_PLAYER_NORMALIZE, FALSE);

	g_autofree char *path = g_file_get_path (file);
	g_autofree char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
	g_autofree char *cache = g_build_filename (g_get_user_cache_dir (), "nemo", "loudness-v1", hash, NULL);
	g_autoptr (GKeyFile) key = g_key_file_new ();
	g_assert_true (g_key_file_load_from_file (key, cache, G_KEY_FILE_NONE, NULL));
	g_key_file_set_string (key, "r128", "peak", "not-a-number");
	gsize length;
	g_autofree char *corrupt = g_key_file_to_data (key, &length, NULL);
	g_assert_true (g_file_set_contents (cache, corrupt, length, NULL));
	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "*Ignoring corrupt loudness cache*");
	g_autofree NemoMediaLoudness *miss = loudness_measure (file, FALSE);
	g_test_assert_expected_messages ();
	g_assert_false (miss->cached);
	g_test_expect_message (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, "*Ignoring corrupt loudness cache*");
	g_autofree NemoMediaLoudness *repaired = loudness_measure (file, TRUE);
	g_test_assert_expected_messages ();
	g_assert_true (repaired->cached);
	g_assert_cmpfloat_with_epsilon (repaired->gain, measurement->gain, 0.000001);
	remove_fixture (file);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

typedef struct {
	GtkApplication *application;
	GFile *file;
	GThread *task_releaser;
	GMutex lock;
	GCond condition;
	gboolean release_task;
	gboolean completed;
	gboolean finalized;
	gboolean explicit_quit;
	GTask *retained_task;
} LoudnessApplicationTest;

static gpointer
loudness_release_task_on_worker (gpointer data)
{
	LoudnessApplicationTest *test = data;
	g_mutex_lock (&test->lock);
	while (!test->release_task)
		g_cond_wait (&test->condition, &test->lock);
	g_mutex_unlock (&test->lock);
	g_object_unref (test->retained_task);
	return NULL;
}

static void
loudness_application_completed (GObject *source, GAsyncResult *result, gpointer data)
{
	LoudnessApplicationTest *test = data;
	g_autoptr (GError) error = NULL;
	g_autofree NemoMediaLoudness *measurement = nemo_media_loudness_finish (result, &error);
	g_assert_no_error (error);
	g_assert_true (measurement->cached);
	g_assert_true (g_thread_self () == main_thread);
	/* Force the last task reference to disappear on a worker, after its UI
	 * callback. Application lifetime must not depend on task destruction. */
	test->retained_task = g_object_ref (G_TASK (result));
	test->task_releaser = g_thread_new ("loudness-task-release", loudness_release_task_on_worker, test);
	test->completed = TRUE;
	if (test->explicit_quit)
		g_application_quit (G_APPLICATION (test->application));
}

static void
loudness_application_activate (GApplication *application, gpointer data)
{
	LoudnessApplicationTest *test = data;
	nemo_media_loudness_analyze_async (test->file, NULL, loudness_application_completed, test);
	/* No window or other hold: only the analysis keeps this app running. */
}

static void
loudness_application_finalized (gpointer data, GObject *application)
{
	LoudnessApplicationTest *test = data;
	g_assert_true (g_thread_self () == main_thread);
	test->finalized = TRUE;
}

static void
test_player_loudness_application_lifetime (gconstpointer data)
{
	if (!g_test_subprocess ()) {
		g_test_trap_subprocess (NULL, 10 * G_TIME_SPAN_SECOND, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed ();
		return;
	}
	g_autofree char *directory = g_dir_make_tmp ("nemo-loudness-XXXXXX", NULL);
	LoudnessApplicationTest test = { .explicit_quit = GPOINTER_TO_INT (data) };
	g_mutex_init (&test.lock);
	g_cond_init (&test.condition);
	test.file = loudness_fixture (directory, "application.wav", 0.02, FALSE);
	test.application = gtk_application_new ("org.Nemo.LoudnessLifetimeTest", G_APPLICATION_NON_UNIQUE);
	g_object_weak_ref (G_OBJECT (test.application), loudness_application_finalized, &test);
	g_signal_connect (test.application, "activate", G_CALLBACK (loudness_application_activate), &test);
	char *argv[] = { "nemo-loudness-lifetime", NULL };
	g_assert_cmpint (g_application_run (G_APPLICATION (test.application), 1, argv), ==, 0);
	g_assert_true (test.completed);
	g_clear_object (&test.application);
	g_assert_true (test.finalized);
	g_mutex_lock (&test.lock);
	test.release_task = TRUE;
	g_cond_signal (&test.condition);
	g_mutex_unlock (&test.lock);
	g_thread_join (test.task_releaser);
	g_cond_clear (&test.condition);
	g_mutex_clear (&test.lock);
	remove_fixture (test.file);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}

static void
test_player_loudness_errors_and_close (void)
{
	nemo_global_preferences_init ();
	g_autofree char *directory = g_dir_make_tmp ("nemo-loudness-XXXXXX", NULL);
	g_autofree char *bad_path = g_build_filename (directory, "bad.wav", NULL);
	g_assert_true (g_file_set_contents (bad_path, "not audio", -1, NULL));
	GFile *bad = g_file_new_for_path (bad_path);
	LoudnessCompletion invalid = { 0 };
	nemo_media_loudness_analyze_async (bad, NULL, loudness_completed, &invalid);
	WAIT_UNTIL (invalid.done);
	g_assert_nonnull (invalid.error);
	g_assert_null (invalid.measurement);
	g_clear_error (&invalid.error);
	GFile *remote = g_file_new_for_uri ("https://example.invalid/track.wav");
	LoudnessCompletion unsupported = { 0 };
	nemo_media_loudness_lookup_async (remote, NULL, loudness_completed, &unsupported);
	WAIT_UNTIL (unsupported.done);
	g_assert_error (unsupported.error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
	g_clear_error (&unsupported.error);
	g_object_unref (remote);
	GFile *file = loudness_fixture (directory, "cancel.wav", 0.02, FALSE);
	g_autoptr (GCancellable) cancel = g_cancellable_new ();
	LoudnessCompletion cancelled = { 0 };
	nemo_media_loudness_analyze_async (file, cancel, loudness_completed, &cancelled);
	g_cancellable_cancel (cancel);
	WAIT_UNTIL (cancelled.done);
	g_assert_error (cancelled.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error (&cancelled.error);
	g_autofree NemoMediaLoudness *miss = loudness_measure (file, FALSE);
	g_assert_false (miss->cached);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_MEDIA_PLAYER_NORMALIZE, TRUE);
	NemoQuickPreview *player = g_object_ref_sink (nemo_quick_preview_new_player ());
	nemo_quick_preview_show_file (player, file, NULL);
	gpointer weak = player;
	g_object_add_weak_pointer (G_OBJECT (player), &weak);
	gtk_widget_destroy (GTK_WIDGET (player));
	g_object_unref (player);
	WAIT_UNTIL (weak == NULL);
	/* A cancelled player must not strand the sole scanning worker. */
	g_autofree NemoMediaLoudness *next = loudness_measure (file, TRUE);
	g_assert_true (next->cached);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_MEDIA_PLAYER_NORMALIZE, FALSE);
	remove_fixture (bad);
	remove_fixture (file);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
}
