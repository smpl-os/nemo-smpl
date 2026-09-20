/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * nemo-preview-utils.c — Shared MIME-type helpers for preview widgets
 *
 * Copyright (C) 2026 smplOS contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#include <config.h>
#include "nemo-preview-utils.h"

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <string.h>

#ifdef NEMO_SMPL
typedef struct {
	GTask *task;
	GTaskThreadFunc worker;
} PreviewWork;

static GMutex preview_work_lock;
static GQueue preview_pending_work = G_QUEUE_INIT;
static guint preview_active_workers;

static void
preview_work_free (PreviewWork *work)
{
	g_object_unref (work->task);
	g_free (work);
}

static gpointer
preview_work_dispatch (gpointer unused)
{
	for (;;) {
		PreviewWork *work;
		g_mutex_lock (&preview_work_lock);
		work = g_queue_pop_head (&preview_pending_work);
		if (work == NULL) {
			preview_active_workers--;
			g_mutex_unlock (&preview_work_lock);
			return NULL;
		}
		g_mutex_unlock (&preview_work_lock);

		if (!g_task_return_error_if_cancelled (work->task))
			work->worker (work->task, g_task_get_source_object (work->task),
				      g_task_get_task_data (work->task),
				      g_task_get_cancellable (work->task));
		preview_work_free (work);
	}
}

void
nemo_preview_run_task (GTask *task, GTaskThreadFunc worker)
{
	GError *error = NULL;
	PreviewWork *work;
	GQueue cancelled = G_QUEUE_INIT;

	work = g_new0 (PreviewWork, 1);
	work->task = g_object_ref (task);
	work->worker = worker;

	g_mutex_lock (&preview_work_lock);
	/* Selection changes cancel their previous token before submitting new
	 * work. Keep only live demand instead of an unbounded GTask backlog. */
	for (GList *link = preview_pending_work.head, *next; link != NULL; link = next) {
		PreviewWork *pending = link->data;
		next = link->next;
		if (g_cancellable_is_cancelled (g_task_get_cancellable (pending->task))) {
			g_queue_delete_link (&preview_pending_work, link);
			g_queue_push_tail (&cancelled, pending);
		}
	}
	g_queue_push_tail (&preview_pending_work, work);
	if (preview_active_workers < 4) {
		GThread *thread;
		preview_active_workers++;
		thread = g_thread_try_new ("nemo-preview", preview_work_dispatch, NULL, &error);
		if (thread == NULL) {
			preview_active_workers--;
			g_queue_remove (&preview_pending_work, work);
		} else
			g_thread_unref (thread);
	}
	g_mutex_unlock (&preview_work_lock);

	PreviewWork *obsolete;
	while ((obsolete = g_queue_pop_head (&cancelled)) != NULL) {
		g_task_return_new_error (obsolete->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
					"Preview request superseded");
		preview_work_free (obsolete);
	}
	if (error != NULL) {
		g_task_return_error (task, error);
		preview_work_free (work);
	}
}
#endif

#if defined (NEMO_SMPL) && defined (HAVE_GSTREAMER)
#include <gst/app/gstappsink.h>

typedef struct {
	GMutex lock;
	GstElement *pipeline;
	GstState requested;
	gboolean busy;
	gboolean stopping;
	gboolean finished;
} MediaState;

/* At most two transitions can block in a backend, with two more pipelines
 * pending. Coalescing avoids a teardown thread/queue entry per keypress. */
static GThreadPool *media_pool;
static gint media_pipeline_count;

static void
media_state_free (gpointer data)
{
	MediaState *state = data;
	g_mutex_clear (&state->lock);
	g_free (state);
}

static void
media_state_worker (gpointer data, gpointer unused)
{
	MediaState *state = data;
	GstState requested;
	GstStateChangeReturn result;

	for (;;) {
		g_mutex_lock (&state->lock);
		requested = state->requested;
		g_mutex_unlock (&state->lock);

		result = gst_element_set_state (state->pipeline, requested);
		if (result == GST_STATE_CHANGE_FAILURE && requested != GST_STATE_NULL) {
			GError *error = g_error_new_literal (GST_CORE_ERROR,
				GST_CORE_ERROR_STATE_CHANGE, _("Unable to start media preview."));
			/* NULL flushes the bus. Let the UI consume this error and
			 * request shutdown, unless it has already closed the file. */
			gst_element_post_message (state->pipeline,
				gst_message_new_error (GST_OBJECT (state->pipeline), error, NULL));
			g_error_free (error);
		}

		g_mutex_lock (&state->lock);
		if (requested == GST_STATE_NULL) {
			state->finished = TRUE;
			state->busy = FALSE;
			g_mutex_unlock (&state->lock);
			g_atomic_int_add (&media_pipeline_count, -1);
			/* The worker owns the last potentially blocking unref. */
			gst_object_unref (state->pipeline);
			return;
		}
		if (state->requested == requested) {
			state->busy = FALSE;
			g_mutex_unlock (&state->lock);
			return;
		}
		g_mutex_unlock (&state->lock);
	}
}

gboolean
nemo_preview_media_set_state_async (GstElement *pipeline, GstState requested)
{
	static gsize initialized;
	MediaState *state;
	GError *error = NULL;

	g_return_val_if_fail (GST_IS_ELEMENT (pipeline), FALSE);
	if (g_once_init_enter (&initialized)) {
		media_pool = g_thread_pool_new (media_state_worker, NULL, 2, FALSE, &error);
		if (error != NULL) {
			g_warning ("Cannot create media preview workers: %s", error->message);
			g_clear_error (&error);
		}
		g_once_init_leave (&initialized, 1);
	}
	if (media_pool == NULL)
		return FALSE;
	/* A failed push is still queued by GLib. Retry starting its worker on
	 * subsequent requests instead of abandoning those retained pipelines. */
	if (g_thread_pool_get_num_threads (media_pool) == 0 &&
	    !g_thread_pool_set_max_threads (media_pool, 2, &error)) {
		g_debug ("Cannot start media preview worker: %s", error->message);
		g_clear_error (&error);
	}

	state = g_object_get_data (G_OBJECT (pipeline), "nemo-preview-media-state");
	if (state == NULL) {
		/* A never-started pipeline is already stopped and has no I/O
		 * resources to dispose. It needs neither a worker nor a slot. */
		if (requested == GST_STATE_NULL)
			return TRUE;
		if (g_atomic_int_get (&media_pipeline_count) >= 4)
			return FALSE;
		state = g_new0 (MediaState, 1);
		g_mutex_init (&state->lock);
		state->pipeline = gst_object_ref (pipeline);
		g_object_set_data_full (G_OBJECT (pipeline), "nemo-preview-media-state",
				       state, media_state_free);
		g_atomic_int_inc (&media_pipeline_count);
	}

	g_mutex_lock (&state->lock);
	if (state->finished || state->stopping) {
		g_mutex_unlock (&state->lock);
		return requested == GST_STATE_NULL;
	}
	state->requested = requested;
	state->stopping = requested == GST_STATE_NULL;
	if (!state->busy) {
		state->busy = TRUE;
		if (!g_thread_pool_push (media_pool, state, &error)) {
			g_mutex_unlock (&state->lock);
			g_debug ("Media preview transition queued without a worker: %s", error->message);
			g_error_free (error);
			return FALSE;
		}
	}
	g_mutex_unlock (&state->lock);
	return TRUE;
}

struct _NemoPreviewMediaFrames {
	gint refs;
	GMutex lock;
	GWeakRef widget;
	NemoPreviewSampleFunc consume;
	GstSample *sample;
	guint idle;
	gboolean stopped;
};

static NemoPreviewMediaFrames *
media_frames_ref (NemoPreviewMediaFrames *frames)
{
	g_atomic_int_inc (&frames->refs);
	return frames;
}

static void
media_frames_unref (gpointer data)
{
	NemoPreviewMediaFrames *frames = data;
	if (g_atomic_int_dec_and_test (&frames->refs)) {
		g_weak_ref_clear (&frames->widget);
		g_mutex_clear (&frames->lock);
		g_free (frames);
	}
}

static gboolean
media_frame_ready (gpointer data)
{
	NemoPreviewMediaFrames *frames = data;
	GstSample *sample;
	GObject *widget;

	g_mutex_lock (&frames->lock);
	frames->idle = 0;
	sample = g_steal_pointer (&frames->sample);
	g_mutex_unlock (&frames->lock);
	widget = g_weak_ref_get (&frames->widget);
	if (widget != NULL && sample != NULL && !frames->stopped)
		frames->consume (widget, sample);
	else if (sample != NULL)
		gst_sample_unref (sample);
	g_clear_object (&widget);
	return G_SOURCE_REMOVE;
}

static GstFlowReturn
media_queue_sample (NemoPreviewMediaFrames *frames, GstSample *sample)
{
	if (sample == NULL)
		return GST_FLOW_OK;
	g_mutex_lock (&frames->lock);
	if (!frames->stopped) {
		g_clear_pointer (&frames->sample, gst_sample_unref);
		frames->sample = g_steal_pointer (&sample);
		if (frames->idle == 0)
			frames->idle = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
				media_frame_ready, media_frames_ref (frames), media_frames_unref);
	}
	g_mutex_unlock (&frames->lock);
	if (sample != NULL)
		gst_sample_unref (sample);
	return GST_FLOW_OK;
}

static GstFlowReturn
media_new_sample (GstAppSink *sink, gpointer data)
{
	return media_queue_sample (data, gst_app_sink_pull_sample (sink));
}

static GstFlowReturn
media_new_preroll (GstAppSink *sink, gpointer data)
{
	return media_queue_sample (data, gst_app_sink_pull_preroll (sink));
}

NemoPreviewMediaFrames *
nemo_preview_media_connect_sink (GstElement *sink, GObject *widget,
				 NemoPreviewSampleFunc consume)
{
	GstAppSinkCallbacks callbacks = { 0 };
	NemoPreviewMediaFrames *frames = g_new0 (NemoPreviewMediaFrames, 1);
	frames->refs = 1;
	g_mutex_init (&frames->lock);
	g_weak_ref_init (&frames->widget, widget);
	frames->consume = consume;
	callbacks.new_sample = media_new_sample;
	callbacks.new_preroll = media_new_preroll;
	gst_app_sink_set_callbacks (GST_APP_SINK (sink), &callbacks,
				   media_frames_ref (frames), media_frames_unref);
	return frames;
}

void
nemo_preview_media_frames_stop (NemoPreviewMediaFrames *frames)
{
	guint idle;
	if (frames == NULL)
		return;
	g_mutex_lock (&frames->lock);
	frames->stopped = TRUE;
	idle = frames->idle;
	frames->idle = 0;
	g_clear_pointer (&frames->sample, gst_sample_unref);
	g_mutex_unlock (&frames->lock);
	if (idle != 0)
		g_source_remove (idle);
	media_frames_unref (frames);
}
#endif

gboolean
nemo_preview_mime_is_image (const gchar *mime)
{
	if (mime == NULL)
		return FALSE;

	return g_str_has_prefix (mime, "image/") ||
	       g_content_type_is_a (mime, "image/*");
}

gboolean
nemo_preview_mime_is_raw_image (const gchar *mime)
{
	if (mime == NULL)
		return FALSE;

	/* Camera RAW formats that gdk-pixbuf cannot decode natively */
	static const char *raw_types[] = {
		"image/x-sony-arw",
		"image/x-adobe-dng",
		"image/x-canon-cr2",
		"image/x-canon-cr3",
		"image/x-canon-crw",
		"image/x-nikon-nef",
		"image/x-nikon-nrw",
		"image/x-olympus-orf",
		"image/x-pentax-pef",
		"image/x-panasonic-rw2",
		"image/x-panasonic-raw",
		"image/x-fuji-raf",
		"image/x-samsung-srw",
		"image/x-sigma-x3f",
		"image/x-minolta-mrw",
		"image/x-kodak-dcr",
		"image/x-kodak-kdc",
		"image/x-raw",
		"image/x-dcraw",
		NULL
	};

	for (int i = 0; raw_types[i] != NULL; i++) {
		if (g_strcmp0 (mime, raw_types[i]) == 0)
			return TRUE;
	}

	return FALSE;
}

gboolean
nemo_preview_mime_is_text (const gchar *mime)
{
	if (mime == NULL)
		return FALSE;

	if (g_str_has_prefix (mime, "text/"))
		return TRUE;

	if (g_content_type_is_a (mime, "text/plain"))
		return TRUE;

	/* Common source code and configuration types that are really text */
	static const char *text_types[] = {
		"application/json",
		"application/xml",
		"application/javascript",
		"application/x-shellscript",
		"application/x-perl",
		"application/x-ruby",
		"application/x-python",
		"application/x-desktop",
		"application/toml",
		"application/yaml",
		"application/x-yaml",
		"application/sql",
		"application/x-awk",
		"application/x-m4",
		"application/xslt+xml",
		NULL
	};

	for (int i = 0; text_types[i] != NULL; i++) {
		if (g_strcmp0 (mime, text_types[i]) == 0 ||
		    g_content_type_is_a (mime, text_types[i]))
			return TRUE;
	}

	return FALSE;
}

gboolean
nemo_preview_mime_is_video (const gchar *mime)
{
	if (mime == NULL)
		return FALSE;

	return g_str_has_prefix (mime, "video/") ||
	       g_content_type_is_a (mime, "video/*");
}

gboolean
nemo_preview_mime_is_audio (const gchar *mime)
{
	if (mime == NULL)
		return FALSE;

	return g_str_has_prefix (mime, "audio/") ||
	       g_content_type_is_a (mime, "audio/*");
}

gboolean
nemo_preview_mime_is_media (const gchar *mime)
{
	return nemo_preview_mime_is_video (mime) ||
	       nemo_preview_mime_is_audio (mime);
}
