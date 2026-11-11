#ifndef NEMO_MEDIA_EFFECTS_H
#define NEMO_MEDIA_EFFECTS_H

#include <gtk/gtk.h>
#include <gst/gst.h>

G_BEGIN_DECLS

#define NEMO_TYPE_MEDIA_VISUALIZER (nemo_media_visualizer_get_type ())
G_DECLARE_FINAL_TYPE (NemoMediaVisualizer, nemo_media_visualizer, NEMO, MEDIA_VISUALIZER, GtkDrawingArea)

GtkWidget *nemo_media_visualizer_new (void);
void nemo_media_visualizer_set_playing (NemoMediaVisualizer *self, gboolean playing);
void nemo_media_visualizer_handle_message (NemoMediaVisualizer *self, GstMessage *message);
GstElement *nemo_media_audio_filter_new (GError **error);
void nemo_media_audio_set_binaural (GstElement *filter, gboolean enabled);

G_END_DECLS

#endif
