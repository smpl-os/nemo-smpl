#include <gst/pbutils/missing-plugins.h>
#include <glib/gstdio.h>
#include <unistd.h>

static GstElement *
preview_media_factory_make (const char *factory, const char *name)
{
	GstElement *element = gst_element_factory_make (factory, name);
	if (element != NULL && g_str_equal (factory, "playbin")) {
		GstElement *sink = gst_element_factory_make ("fakesink", NULL);
		g_assert_nonnull (sink);
		gst_object_ref_sink (sink);
		g_object_set (sink, "sync", TRUE, NULL);
		g_object_set (element, "audio-sink", sink, NULL);
		gst_object_unref (sink);
	}
	return element;
}

static char *
preview_media_fixture (int width, int height)
{
	GError *error = NULL;
	char *path = NULL;
	int fd = g_file_open_tmp ("nemo-preview-XXXXXX.webm", &path, &error);
	g_assert_no_error (error);
	g_assert_cmpint (fd, >=, 0);
	g_assert_cmpint (close (fd), ==, 0);
	g_autofree char *description = width == 0 ? g_strdup (
		"audiotestsrc num-buffers=3 ! audioconvert ! opusenc ! "
		"webmmux ! filesink name=output") : g_strdup_printf (
		"videotestsrc num-buffers=3 pattern=red ! "
		"video/x-raw,width=%d,height=%d,framerate=1/1800 ! "
		"vp8enc deadline=1 threads=2 keyframe-max-dist=1 ! queue ! mux. "
		"audiotestsrc num-buffers=3 ! audioconvert ! opusenc ! queue ! mux. "
		"webmmux name=mux ! filesink name=output", width, height);
	GstElement *pipeline = gst_parse_launch (description, &error);
	g_assert_no_error (error);
	g_assert_nonnull (pipeline);
	GstElement *output = gst_bin_get_by_name (GST_BIN (pipeline), "output");
	g_object_set (output, "location", path, NULL);
	gst_object_unref (output);
	GstBus *bus = gst_element_get_bus (pipeline);
	g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PLAYING),
			 !=, GST_STATE_CHANGE_FAILURE);
	GstMessage *message = gst_bus_timed_pop_filtered (bus, 20 * GST_SECOND,
							 GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
	g_assert_nonnull (message);
	if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
		gst_message_parse_error (message, &error, NULL);
		g_error ("WebM fixture failed: %s", error->message);
	}
	g_assert_cmpint (GST_MESSAGE_TYPE (message), ==, GST_MESSAGE_EOS);
	gst_message_unref (message);
	gst_element_set_state (pipeline, GST_STATE_NULL);
	gst_object_unref (bus);
	gst_object_unref (pipeline);
	return path;
}

typedef struct {
	GstPluginFeature *feature;
	guint rank;
} PreviewDecoderRank;

static void
preview_decoder_restore (gpointer data)
{
	PreviewDecoderRank *saved = data;
	gst_plugin_feature_set_rank (saved->feature, saved->rank);
	gst_object_unref (saved->feature);
	g_free (saved);
}

static GPtrArray *
preview_disable_video_decoders (void)
{
	GList *factories = gst_element_factory_list_get_elements (
		GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
	GstCaps *caps = gst_caps_from_string ("video/x-vp8");
	GList *matching = gst_element_factory_list_filter (factories, caps, GST_PAD_SINK, FALSE);
	GPtrArray *saved = g_ptr_array_new_with_free_func (preview_decoder_restore);

	for (GList *item = matching; item != NULL; item = item->next) {
		PreviewDecoderRank *rank = g_new0 (PreviewDecoderRank, 1);
		rank->feature = gst_object_ref (item->data);
		rank->rank = gst_plugin_feature_get_rank (rank->feature);
		gst_plugin_feature_set_rank (rank->feature, GST_RANK_NONE);
		g_ptr_array_add (saved, rank);
	}
	gst_plugin_feature_list_free (matching);
	gst_plugin_feature_list_free (factories);
	gst_caps_unref (caps);
	g_assert_cmpuint (saved->len, >, 0);
	return saved;
}
