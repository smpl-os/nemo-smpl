/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEMO_MEDIA_LOUDNESS_H
#define NEMO_MEDIA_LOUDNESS_H

#include <gio/gio.h>

typedef struct {
	gboolean cached;
	gboolean silent; /* Integrated loudness unavailable: silence or a short clip. */
	gboolean peak_limited;
	double lufs;
	double true_peak;
	double gain;
} NemoMediaLoudness;

/* Lookup never decodes. A cache miss returns an uncached unity-gain result.
 * Analysis only prepares subsequent plays; callers must not apply its gain to
 * the playback that requested it. Both callbacks run after worker cleanup. */
void nemo_media_loudness_lookup_async (GFile *file, GCancellable *cancel,
                                      GAsyncReadyCallback callback, gpointer data);
void nemo_media_loudness_analyze_async (GFile *file, GCancellable *cancel,
                                       GAsyncReadyCallback callback, gpointer data);
NemoMediaLoudness *nemo_media_loudness_finish (GAsyncResult *result, GError **error);
void nemo_media_loudness_shutdown (void);

#endif
