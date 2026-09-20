/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * nemo-preview-utils.h — Shared MIME-type helpers for preview widgets
 *
 * Used by both the sidebar preview pane (NemoPreviewPane) and the F3
 * quick-preview window (NemoQuickPreview) so the classification logic
 * lives in one place.
 *
 * Copyright (C) 2026 smplOS contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#ifndef NEMO_PREVIEW_UTILS_H
#define NEMO_PREVIEW_UTILS_H

#include <gio/gio.h>
#if defined (NEMO_SMPL) && defined (HAVE_GSTREAMER)
#include <gst/gst.h>
#endif

G_BEGIN_DECLS

gboolean nemo_preview_mime_is_image (const gchar *mime_type);
gboolean nemo_preview_mime_is_raw_image (const gchar *mime_type);
gboolean nemo_preview_mime_is_text  (const gchar *mime_type);
gboolean nemo_preview_mime_is_video (const gchar *mime_type);
gboolean nemo_preview_mime_is_audio (const gchar *mime_type);
gboolean nemo_preview_mime_is_media (const gchar *mime_type);

#ifdef NEMO_SMPL
/* Workers must not own widgets or acquire I/O resources before dispatch.
 * Cancellation prunes superseded pending work; an actual worker slot remains
 * occupied until its function returns, even if native I/O ignores cancellation. */
void nemo_preview_run_task (GTask *task, GTaskThreadFunc worker);
#endif

#if defined (NEMO_SMPL) && defined (HAVE_GSTREAMER)
/* State changes are serialized per pipeline. FALSE means the bounded
 * preview queue is full; the caller should show a retryable error. */
gboolean nemo_preview_media_set_state_async (GstElement *pipeline, GstState state);

typedef struct _NemoPreviewMediaFrames NemoPreviewMediaFrames;
typedef void (*NemoPreviewSampleFunc) (GObject *widget, GstSample *sample);

/* Delivers at most one pending frame on the main thread. The callback owns
 * the sample. stop() discards pending frames and releases the handle. */
NemoPreviewMediaFrames *nemo_preview_media_connect_sink (GstElement *sink,
							GObject *widget,
							NemoPreviewSampleFunc consume);
void nemo_preview_media_frames_stop (NemoPreviewMediaFrames *frames);
#endif

G_END_DECLS

#endif /* NEMO_PREVIEW_UTILS_H */
