/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * nemo-paged-viewer.c — Asynchronous, bounded-memory large-file viewer
 *
 * Copyright (C) 2026 smplOS contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#include <config.h>

#ifdef NEMO_SMPL

#include "nemo-paged-viewer.h"
#include "nemo-preview-utils.h"

#include <string.h>
#include <glib/gi18n.h>

#define PAGE_SIZE       (64 * 1024)
#define MAX_PAGES       8
#define HEX_BPL         16
#define MARGIN_H        8
#define MARGIN_V        4
#define SCAN_BUF        4096
#define READ_CHUNK      (256 * 1024)
#define SCROLL_LINES    3

typedef struct {
	gint64 offset;
	GBytes *bytes;
	guint64 tick;
} PageSlot;

typedef struct {
	PageSlot slots[MAX_PAGES];
	guint64 tick;
} PageCache;

typedef struct {
	gchar *text;
	gint64 offset;
	gsize length;
	guint highlight_start;
	guint highlight_end;
} DisplayLine;

typedef struct {
	GPtrArray *lines;
	gint64 offset;
	gint64 match;
	NemoViewerMode mode;
} DisplayPage;

/* Streams never leave the worker that opened them, even on cancellation.
 * Cache snapshots contain only immutable bytes; no widget, lock or open stream
 * is shared with a worker. A blocked old device cannot retain a live widget or
 * prevent a new location/page request from running. */
typedef struct {
	GFile *location;
	GFileInputStream *stream;
	gint64 stream_offset;
	gint64 size;
	PageCache cache;
	GCancellable *cancellable;
	GError *error;
} Reader;

typedef enum { REQUEST_OPEN, REQUEST_PAGE, REQUEST_SEARCH } RequestKind;
typedef enum { NAV_OFFSET, NAV_LINES, NAV_END } Navigation;

typedef struct {
	GWeakRef viewer;
	Reader reader;
	RequestKind kind;
	guint64 file_generation;
	guint64 page_generation;
	guint64 search_generation;
	NemoViewerMode mode;
	Navigation navigation;
	gint64 offset;
	int delta;
	int lines;
	int cols;
	gchar *needle;
	gint64 match;
	gboolean backwards;
} Request;

typedef struct {
	PageCache cache;
	DisplayPage *page;
	gint64 size;
	gdouble avg_line_len;
	gint64 match;
	gint64 match_top;
} Result;

struct _NemoPagedViewer {
	GtkBox parent;
	GtkWidget *drawing_area;
	GtkWidget *scrollbar;
	GtkAdjustment *vadjust;
	gboolean in_adj_update;
	gboolean destroyed;

	GFile *location;
	gboolean opening;
	gboolean opened;
	gint64 file_size;
	guint64 file_generation;
	guint64 page_generation;
	guint64 search_generation;
	GCancellable *open_cancel;
	GCancellable *page_cancel;
	GCancellable *search_cancel;
	gboolean page_for_search;
	PageCache cache;
	DisplayPage *page;
	gchar *message;

	NemoViewerMode mode;
	PangoFontDescription *font_desc;
	int char_w;
	int char_h;
	gboolean font_measured;
	int vis_lines;
	int vis_cols;
	gint64 top_offset;
	gdouble avg_line_len;
	gdouble scroll_accum;
	Navigation pending_navigation;
	gint64 pending_offset;
	int pending_delta;

	gchar *search_needle;
	GError *search_error;
	gint64 search_match_offset;
	gboolean search_pending;
	gboolean deferred_search;
	gboolean deferred_backwards;
};

G_DEFINE_TYPE (NemoPagedViewer, nemo_paged_viewer, GTK_TYPE_BOX)

enum { LOAD_FINISHED, SEARCH_CHANGED, LAST_SIGNAL };
static guint signals[LAST_SIGNAL];

static void update_scrollbar (NemoPagedViewer *self);
static void request_page (NemoPagedViewer *self, Navigation navigation,
                         gint64 offset, int delta);
static gboolean start_search (NemoPagedViewer *self, gboolean backwards);

static void
cache_clear (PageCache *cache)
{
	for (int i = 0; i < MAX_PAGES; i++)
		g_clear_pointer (&cache->slots[i].bytes, g_bytes_unref);
	memset (cache, 0, sizeof (*cache));
}

static void
cache_copy (PageCache *dest, const PageCache *source)
{
	*dest = *source;
	for (int i = 0; i < MAX_PAGES; i++)
		if (dest->slots[i].bytes)
			g_bytes_ref (dest->slots[i].bytes);
}

static gboolean
reader_cancelled (Reader *reader)
{
	if (reader->error)
		return TRUE;
	return g_cancellable_set_error_if_cancelled (reader->cancellable, &reader->error);
}

static void
reader_close (Reader *reader)
{
	if (reader->stream) {
		/* Never close a device stream on the main context, even when the
		 * request was cancelled or its widget has already been destroyed. */
		g_input_stream_close (G_INPUT_STREAM (reader->stream), NULL, NULL);
		g_clear_object (&reader->stream);
	}
}

static gboolean
reader_open (Reader *reader)
{
	if (reader_cancelled (reader))
		return FALSE;
	if (!reader->stream) {
		reader->stream = g_file_read (reader->location, reader->cancellable,
		                             &reader->error);
		reader->stream_offset = 0;
	}
	return reader->stream != NULL;
}

static gboolean
reader_position (Reader *reader, gint64 offset)
{
	if (!reader_open (reader))
		return FALSE;
	if (offset == reader->stream_offset)
		return TRUE;
	if (G_IS_SEEKABLE (reader->stream) &&
	    g_seekable_can_seek (G_SEEKABLE (reader->stream))) {
		if (!g_seekable_seek (G_SEEKABLE (reader->stream), offset, G_SEEK_SET,
		                      reader->cancellable, &reader->error))
			return FALSE;
		reader->stream_offset = offset;
		return TRUE;
	}

	/* Some GVfs/MTP streams cannot seek. Reopen and discard bounded chunks
	 * instead of downloading the file into RAM or limiting its visible size. */
	if (offset < reader->stream_offset) {
		reader_close (reader);
		if (!reader_open (reader))
			return FALSE;
	}
	while (reader->stream_offset < offset && !reader_cancelled (reader)) {
		gssize n = g_input_stream_skip (G_INPUT_STREAM (reader->stream),
		                               MIN ((gint64) PAGE_SIZE,
		                                    offset - reader->stream_offset),
		                               reader->cancellable, &reader->error);
		if (n <= 0)
			return FALSE;
		reader->stream_offset += n;
	}
	return !reader_cancelled (reader);
}

static PageSlot *
cache_get_page (Reader *reader, gint64 base)
{
	PageSlot *slot = &reader->cache.slots[0];
	if (reader_cancelled (reader))
		return NULL;
	for (int i = 0; i < MAX_PAGES; i++) {
		PageSlot *candidate = &reader->cache.slots[i];
		if (candidate->bytes && candidate->offset == base) {
			candidate->tick = ++reader->cache.tick;
			return candidate;
		}
		if (!candidate->bytes || candidate->tick < slot->tick)
			slot = candidate;
	}
	if (!reader_position (reader, base))
		return NULL;

	guint8 *bytes = g_malloc (PAGE_SIZE);
	gsize count = MIN ((gint64) PAGE_SIZE, reader->size - base);
	gsize length = 0;
	while (length < count && !reader_cancelled (reader)) {
		gssize n = g_input_stream_read (G_INPUT_STREAM (reader->stream),
		                               bytes + length, count - length,
		                               reader->cancellable, &reader->error);
		if (n <= 0)
			break;
		length += n;
		reader->stream_offset += n;
	}
	if (reader->error) {
		g_free (bytes);
		return NULL;
	}
	g_clear_pointer (&slot->bytes, g_bytes_unref);
	slot->bytes = g_bytes_new_take (bytes, length);
	slot->offset = base;
	slot->tick = ++reader->cache.tick;
	return slot;
}

static gssize
cache_read (Reader *reader, gint64 offset, void *buffer, gsize count)
{
	if (offset < 0 || reader_cancelled (reader))
		return -1;
	if (offset >= reader->size)
		return 0;
	count = MIN ((guint64) count, (guint64) (reader->size - offset));
	gsize copied = 0;
	while (copied < count) {
		gint64 base = offset & ~((gint64) PAGE_SIZE - 1);
		gsize in_page = offset - base;
		PageSlot *slot = cache_get_page (reader, base);
		if (!slot)
			return reader->error ? -1 : (gssize) copied;
		gsize length;
		const guint8 *bytes = g_bytes_get_data (slot->bytes, &length);
		if (length <= in_page)
			break;
		gsize n = MIN (length - in_page, count - copied);
		memcpy ((guint8 *) buffer + copied, bytes + in_page, n);
		offset += n;
		copied += n;
	}
	return copied;
}

static gint64
find_line_start (Reader *reader, gint64 offset)
{
	guint8 buffer[SCAN_BUF];
	while (offset > 0 && !reader_cancelled (reader)) {
		gint64 start = MAX (0, offset - SCAN_BUF);
		gssize n = cache_read (reader, start, buffer, offset - start);
		if (n <= 0)
			break;
		for (gssize i = n - 1; i >= 0; i--)
			if (buffer[i] == '\n')
				return start + i + 1;
		offset = start;
	}
	return 0;
}

static gint64
skip_lines_forward (Reader *reader, gint64 offset, int count)
{
	guint8 buffer[SCAN_BUF];
	while (count > 0 && offset < reader->size && !reader_cancelled (reader)) {
		gssize n = cache_read (reader, offset, buffer, sizeof (buffer));
		if (n <= 0)
			break;
		for (gssize i = 0; i < n; i++)
			if (buffer[i] == '\n' && --count == 0)
				return offset + i + 1;
		offset += n;
	}
	return offset;
}

static gint64
skip_lines_backward (Reader *reader, gint64 offset, int count)
{
	while (count-- > 0 && offset > 0 && !reader_cancelled (reader))
		offset = find_line_start (reader, offset - 1);
	return offset;
}

static gint64
navigate (Request *request)
{
	Reader *reader = &request->reader;
	gint64 offset = CLAMP (request->offset, 0, reader->size);
	if (request->mode == NEMO_VIEWER_MODE_HEX) {
		gint64 total = reader->size / HEX_BPL + (reader->size % HEX_BPL != 0);
		gint64 last = MAX (0, total - request->lines);
		if (request->navigation == NAV_END)
			return last * HEX_BPL;
		if (request->navigation == NAV_LINES)
			return CLAMP (offset / HEX_BPL + request->delta, 0, last) * HEX_BPL;
		return (offset / HEX_BPL) * HEX_BPL;
	}
	if (request->navigation == NAV_END)
		return skip_lines_backward (reader, reader->size, MAX (1, request->lines - 1));
	offset = find_line_start (reader, offset);
	if (request->navigation == NAV_LINES) {
		if (request->delta > 0)
			offset = skip_lines_forward (reader, offset, request->delta);
		else
			offset = skip_lines_backward (reader, offset, -request->delta);
	}
	return offset;
}

static void
display_line_free (gpointer data)
{
	DisplayLine *line = data;
	g_free (line->text);
	g_free (line);
}

static void
display_page_free (DisplayPage *page)
{
	if (page) {
		g_ptr_array_unref (page->lines);
		g_free (page);
	}
}

static DisplayPage *
render_page (Request *request, gint64 offset)
{
	DisplayPage *page = g_new0 (DisplayPage, 1);
	page->lines = g_ptr_array_new_with_free_func (display_line_free);
	page->offset = offset;
	page->match = request->match;
	page->mode = request->mode;
	gsize needed = request->mode == NEMO_VIEWER_MODE_HEX
		? MIN ((gint64) READ_CHUNK, (gint64) request->lines * HEX_BPL) : READ_CHUNK;
	guint8 *bytes = g_malloc (needed);
	gssize n = cache_read (&request->reader, offset, bytes, needed);
	gsize pos = 0;
	while (n > 0 && pos < (gsize) n &&
	       page->lines->len < (guint) request->lines &&
	       !reader_cancelled (&request->reader)) {
		DisplayLine *line = g_new0 (DisplayLine, 1);
		line->offset = offset + pos;
		if (request->mode == NEMO_VIEWER_MODE_HEX) {
			line->length = MIN (HEX_BPL, (gsize) n - pos);
			GString *text = g_string_new (NULL);
			g_string_append_printf (text, "%08" G_GINT64_MODIFIER "x  ",
			                        (guint64) line->offset);
			for (int i = 0; i < HEX_BPL; i++) {
				if ((gsize) i < line->length)
					g_string_append_printf (text, "%02x ", bytes[pos + i]);
				else
					g_string_append (text, "   ");
				if (i == 7)
					g_string_append_c (text, ' ');
			}
			g_string_append (text, " |");
			for (gsize i = 0; i < line->length; i++) {
				guint8 c = bytes[pos + i];
				g_string_append_c (text, c >= 0x20 && c <= 0x7e ? c : '.');
			}
			g_string_append_c (text, '|');
			line->text = g_string_free (text, FALSE);
			pos += line->length;
		} else {
			gsize end = pos;
			while (end < (gsize) n && bytes[end] != '\n')
				end++;
			line->length = end - pos;
			/* Only a screen-width prefix will ever be drawn. Bounding the
			 * layout input avoids expensive GTK/Pango shaping of huge lines. */
			gsize visible = MIN (line->length,
			                     (gsize) CLAMP (request->cols, 1, 4096) * 6);
			line->text = g_utf8_make_valid ((const gchar *) bytes + pos, visible);
			if (request->needle && request->match >= line->offset &&
			    request->match - line->offset < (gint64) visible) {
				gsize start = request->match - line->offset;
				gsize finish = start + MIN (strlen (request->needle), visible - start);
				gchar *prefix = g_utf8_make_valid ((const gchar *) bytes + pos, start);
				gchar *through = g_utf8_make_valid ((const gchar *) bytes + pos, finish);
				line->highlight_start = strlen (prefix);
				line->highlight_end = strlen (through);
				g_free (prefix);
				g_free (through);
			}
			pos = end + 1;
		}
		g_ptr_array_add (page->lines, line);
	}
	g_free (bytes);
	return page;
}

/* Streaming KMP keeps both cancellation latency and memory bounded for
 * cross-page matches, including needles larger than a page/read chunk. */
static gint64
search_range (Reader *reader, const gchar *needle, gsize length,
              const gsize *failure, gint64 start, gint64 before, gboolean last)
{
	guint8 *buffer = g_malloc (READ_CHUNK);
	gsize matched = 0;
	gint64 found = -1;
	gint64 end = before + MIN ((guint64) (length - 1),
	                          (guint64) (reader->size - before));
	for (gint64 pos = start; pos < end && !reader_cancelled (reader);) {
		gssize n = cache_read (reader, pos, buffer,
		                      MIN ((gint64) READ_CHUNK, end - pos));
		if (n <= 0)
			break;
		for (gssize i = 0; i < n; i++) {
			if ((i % SCAN_BUF) == 0 && reader_cancelled (reader))
				goto out;
			gchar c = g_ascii_tolower (buffer[i]);
			while (matched && c != needle[matched])
				matched = failure[matched - 1];
			if (c == needle[matched])
				matched++;
			if (matched == length) {
				gint64 at = pos + i + 1 - length;
				if (at < before) {
					found = at;
					if (!last)
						goto out;
				}
				matched = failure[matched - 1];
			}
		}
		pos += n;
	}
out:
	g_free (buffer);
	return found;
}

static void
search_worker (Request *request, Result *result)
{
	Reader *reader = &request->reader;
	gchar *needle = g_ascii_strdown (request->needle, -1);
	gsize length = strlen (needle);
	result->match = -1;
	if (length == 0 || (guint64) reader->size < length)
		goto out;
	gsize *failure = g_new0 (gsize, length);
	for (gsize i = 1, matched = 0; i < length; i++) {
		if ((i % SCAN_BUF) == 0 && reader_cancelled (reader))
			goto free_failure;
		while (matched && needle[i] != needle[matched])
			matched = failure[matched - 1];
		if (needle[i] == needle[matched])
			matched++;
		failure[i] = matched;
	}
	gint64 from = request->match >= 0
		? request->match + (request->backwards ? 0 : 1) : request->offset;
	from = CLAMP (from, 0, reader->size);
	if (request->backwards) {
		result->match = search_range (reader, needle, length, failure, 0, from, TRUE);
		if (result->match < 0 && !reader_cancelled (reader))
			result->match = search_range (reader, needle, length, failure,
			                             0, reader->size, TRUE);
	} else {
		result->match = search_range (reader, needle, length, failure,
		                             from, reader->size, FALSE);
		if (result->match < 0 && from > 0 && !reader_cancelled (reader))
			result->match = search_range (reader, needle, length, failure, 0, from, FALSE);
	}
	if (result->match >= 0 && !reader_cancelled (reader)) {
		if (request->mode == NEMO_VIEWER_MODE_HEX)
			result->match_top = MAX (0, result->match / HEX_BPL - 2) * HEX_BPL;
		else
			result->match_top = skip_lines_backward (reader,
				find_line_start (reader, result->match), 2);
	}
free_failure:
	g_free (failure);
out:
	g_free (needle);
}

static void
result_free (gpointer data)
{
	Result *result = data;
	cache_clear (&result->cache);
	display_page_free (result->page);
	g_free (result);
}

static void
request_free (gpointer data)
{
	Request *request = data;
	g_assert (request->reader.stream == NULL);
	g_weak_ref_clear (&request->viewer);
	g_clear_object (&request->reader.location);
	g_clear_error (&request->reader.error);
	cache_clear (&request->reader.cache);
	g_free (request->needle);
	g_free (request);
}

static void
request_worker (GTask *task, gpointer source, gpointer task_data,
                GCancellable *cancellable)
{
	Request *request = task_data;
	Reader *reader = &request->reader;
	Result *result = g_new0 (Result, 1);
	reader->cancellable = cancellable;
	if (reader_cancelled (reader))
		goto done;

	if (request->kind == REQUEST_OPEN) {
		if (!reader_open (reader))
			goto done;
		GFileInfo *info = g_file_input_stream_query_info (reader->stream,
			G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
			cancellable, &reader->error);
		if (!info || !g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE)) {
			g_clear_object (&info);
			g_clear_error (&reader->error);
			if (reader_cancelled (reader))
				goto done;
			info = g_file_query_info (reader->location,
				G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
				G_FILE_QUERY_INFO_NONE, cancellable, &reader->error);
		}
		if (!info)
			goto done;
		if (!g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE) ||
		    g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
			g_set_error_literal (&reader->error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			                     _("This location cannot be paged."));
			g_object_unref (info);
			goto done;
		}
		reader->size = MAX (0, g_file_info_get_size (info));
		g_object_unref (info);
		guint8 buffer[PAGE_SIZE];
		gssize n = cache_read (reader, 0, buffer, sizeof (buffer));
		int newlines = 0;
		for (gssize i = 0; i < n; i++)
			if (buffer[i] == '\n')
				newlines++;
		result->avg_line_len = n > 0 ? (gdouble) n / MAX (1, newlines) : 80.0;
	}
	if (!reader_cancelled (reader)) {
		if (request->kind == REQUEST_SEARCH)
			search_worker (request, result);
		else
			result->page = render_page (request, navigate (request));
	}
done:
	reader_close (reader);
	if (reader_cancelled (reader)) {
		g_task_return_error (task, g_steal_pointer (&reader->error));
		result_free (result);
	} else {
		result->size = reader->size;
		if (request->kind != REQUEST_SEARCH) {
			result->cache = reader->cache;
			memset (&reader->cache, 0, sizeof (reader->cache));
		}
		g_task_return_pointer (task, result, result_free);
	}
}

static void
cancel_request (GCancellable **cancellable)
{
	if (*cancellable)
		g_cancellable_cancel (*cancellable);
	g_clear_object (cancellable);
}

static void
cancel_search (NemoPagedViewer *self)
{
	cancel_request (&self->search_cancel);
	if (self->search_error &&
	    g_strcmp0 (self->message, self->search_error->message) == 0)
		g_clear_pointer (&self->message, g_free);
	g_clear_error (&self->search_error);
	if (self->page_for_search) {
		cancel_request (&self->page_cancel);
		self->page_generation++;
		self->page_for_search = FALSE;
	}
	self->search_generation++;
	self->search_pending = FALSE;
	self->deferred_search = FALSE;
}

static void
adopt_page (NemoPagedViewer *self, Result *result)
{
	cache_clear (&self->cache);
	self->cache = result->cache;
	memset (&result->cache, 0, sizeof (result->cache));
	g_clear_pointer (&self->page, display_page_free);
	self->page = g_steal_pointer (&result->page);
	self->top_offset = self->page->offset;
	g_clear_pointer (&self->message, g_free);
	update_scrollbar (self);
	gtk_widget_queue_draw (self->drawing_area);
}

static void
request_done (GObject *source, GAsyncResult *async_result, gpointer user_data)
{
	GTask *task = G_TASK (async_result);
	Request *request = g_task_get_task_data (task);
	GError *error = NULL;
	Result *result = g_task_propagate_pointer (task, &error);
	NemoPagedViewer *self = g_weak_ref_get (&request->viewer);
	if (!self || self->destroyed || request->file_generation != self->file_generation)
		goto out;
	if (request->kind == REQUEST_PAGE &&
	    request->page_generation != self->page_generation)
		goto out;
	if (request->kind == REQUEST_SEARCH &&
	    request->search_generation != self->search_generation)
		goto out;

	if (request->kind == REQUEST_SEARCH) {
		g_clear_object (&self->search_cancel);
		self->search_pending = FALSE;
		self->search_match_offset = result ? result->match : -1;
		if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_clear_error (&self->search_error);
			self->search_error = g_error_copy (error);
			g_clear_pointer (&self->message, g_free);
			self->message = g_strdup (error->message);
		}
		if (result && result->match >= 0 &&
		    request->page_generation == self->page_generation) {
			request_page (self, NAV_OFFSET, result->match_top, 0);
			self->page_for_search = TRUE;
		}
		gtk_widget_queue_draw (self->drawing_area);
		g_signal_emit (self, signals[SEARCH_CHANGED], 0);
	} else {
		if (request->kind == REQUEST_OPEN) {
			g_clear_object (&self->open_cancel);
			self->opening = FALSE;
			self->opened = result != NULL;
		} else {
			g_clear_object (&self->page_cancel);
			self->page_for_search = FALSE;
		}
		if (result) {
			self->file_size = result->size;
			if (request->kind == REQUEST_OPEN)
				self->avg_line_len = result->avg_line_len;
			adopt_page (self, result);
			if (request->mode != self->mode || request->lines != self->vis_lines ||
			    request->cols != self->vis_cols)
				request_page (self, NAV_OFFSET, self->top_offset, 0);
		} else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			g_clear_pointer (&self->message, g_free);
			self->message = g_strdup (error ? error->message : _("Unable to read this file."));
			gtk_widget_queue_draw (self->drawing_area);
		}
		if (request->kind == REQUEST_OPEN) {
			g_signal_emit (self, signals[LOAD_FINISHED], 0, error);
			/* Signal handlers may close, replace, or destroy the viewer. */
			if (!self->destroyed && request->file_generation == self->file_generation &&
			    self->deferred_search) {
				gboolean backwards = self->deferred_backwards;
				self->deferred_search = FALSE;
				if (self->opened)
					start_search (self, backwards);
				else {
					self->search_pending = FALSE;
					g_signal_emit (self, signals[SEARCH_CHANGED], 0);
				}
			}
		}
	}
out:
	g_clear_object (&self);
	g_clear_error (&error);
	if (result)
		result_free (result);
}

static Request *
request_new (NemoPagedViewer *self, RequestKind kind)
{
	Request *request = g_new0 (Request, 1);
	g_weak_ref_init (&request->viewer, self);
	request->kind = kind;
	request->reader.location = g_object_ref (self->location);
	request->reader.size = self->file_size;
	cache_copy (&request->reader.cache, &self->cache);
	request->file_generation = self->file_generation;
	request->page_generation = self->page_generation;
	request->search_generation = self->search_generation;
	request->mode = self->mode;
	request->lines = self->vis_lines;
	request->cols = self->vis_cols;
	request->offset = self->top_offset;
	request->needle = g_strdup (self->search_needle);
	request->match = self->search_match_offset;
	return request;
}

static void
request_start (Request *request, GCancellable **cancellable)
{
	*cancellable = g_cancellable_new ();
	/* A NULL source object is intentional: a blocked task must not keep the
	 * widget alive. Completion consults the weak reference and generations. */
	GTask *task = g_task_new (NULL, *cancellable, request_done, NULL);
	g_task_set_task_data (task, request, request_free);
	nemo_preview_run_task (task, request_worker);
	g_object_unref (task);
}

static void
request_page (NemoPagedViewer *self, Navigation navigation, gint64 offset, int delta)
{
	if (!self->opened || self->destroyed)
		return;
	cancel_request (&self->page_cancel);
	self->page_generation++;
	self->page_for_search = FALSE;
	self->pending_navigation = navigation;
	self->pending_offset = offset;
	self->pending_delta = delta;
	Request *request = request_new (self, REQUEST_PAGE);
	request->navigation = navigation;
	request->offset = offset;
	request->delta = delta;
	request_start (request, &self->page_cancel);
}

static void
measure_font (NemoPagedViewer *self)
{
	if (self->font_measured)
		return;
	PangoContext *context = gtk_widget_get_pango_context (self->drawing_area);
	PangoFontMetrics *metrics = pango_context_get_metrics (context, self->font_desc, NULL);
	self->char_w = MAX (1, pango_font_metrics_get_approximate_char_width (metrics) / PANGO_SCALE);
	self->char_h = MAX (1, (pango_font_metrics_get_ascent (metrics) +
	                       pango_font_metrics_get_descent (metrics)) / PANGO_SCALE + 2);
	pango_font_metrics_unref (metrics);
	self->font_measured = TRUE;
}

static gboolean
on_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
	NemoPagedViewer *self = user_data;
	GtkAllocation allocation;
	GdkRGBA foreground;
	measure_font (self);
	gtk_widget_get_allocation (widget, &allocation);
	GtkStyleContext *style = gtk_widget_get_style_context (widget);
	gtk_render_background (style, cr, 0, 0, allocation.width, allocation.height);
	gtk_style_context_get_color (style, GTK_STATE_FLAG_NORMAL, &foreground);
	gdk_cairo_set_source_rgba (cr, &foreground);
	PangoLayout *layout = pango_cairo_create_layout (cr);
	pango_layout_set_font_description (layout, self->font_desc);
	pango_layout_set_width (layout, MAX (1, allocation.width - 2 * MARGIN_H) * PANGO_SCALE);
	pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);

	if (self->message || self->opening) {
		pango_layout_set_text (layout, self->message ? self->message : _("Loading…"), -1);
		cairo_move_to (cr, MARGIN_H, MARGIN_V);
		pango_cairo_show_layout (cr, layout);
	} else if (self->page && self->page->mode == self->mode) {
		for (guint i = 0; i < self->page->lines->len && i < (guint) self->vis_lines; i++) {
			DisplayLine *line = g_ptr_array_index (self->page->lines, i);
			int y = MARGIN_V + i * self->char_h;
			pango_layout_set_text (layout, line->text, -1);
			if (self->search_needle && self->search_match_offset >= 0 &&
			    self->search_match_offset == self->page->match) {
				if (self->mode == NEMO_VIEWER_MODE_HEX &&
				    self->search_match_offset >= line->offset &&
				    self->search_match_offset - line->offset < (gint64) line->length) {
					cairo_save (cr);
					cairo_set_source_rgba (cr, 1.0, 0.85, 0.0, 0.35);
					cairo_rectangle (cr, 0, y, allocation.width, self->char_h);
					cairo_fill (cr);
					cairo_restore (cr);
				} else if (line->highlight_end > line->highlight_start) {
					PangoAttrList *attrs = pango_attr_list_new ();
					PangoAttribute *background = pango_attr_background_new (0xFFFF, 0xD700, 0);
					PangoAttribute *fg = pango_attr_foreground_new (0, 0, 0);
					background->start_index = fg->start_index = line->highlight_start;
					background->end_index = fg->end_index = line->highlight_end;
					pango_attr_list_insert (attrs, background);
					pango_attr_list_insert (attrs, fg);
					pango_layout_set_attributes (layout, attrs);
					pango_attr_list_unref (attrs);
				}
			}
			cairo_move_to (cr, MARGIN_H, y);
			pango_cairo_show_layout (cr, layout);
			pango_layout_set_attributes (layout, NULL);
		}
	}
	g_object_unref (layout);
	return TRUE;
}

static void
update_scrollbar (NemoPagedViewer *self)
{
	self->in_adj_update = TRUE;
	if (!self->opened || self->file_size == 0) {
		gtk_adjustment_configure (self->vadjust, 0, 0, 0, 0, 0, 0);
	} else if (self->mode == NEMO_VIEWER_MODE_HEX) {
		gint64 total = self->file_size / HEX_BPL + (self->file_size % HEX_BPL != 0);
		gtk_adjustment_configure (self->vadjust, self->top_offset / HEX_BPL,
		                          0, total, 1, self->vis_lines, self->vis_lines);
	} else {
		gdouble page = MAX (1.0, self->vis_lines * self->avg_line_len);
		gtk_adjustment_configure (self->vadjust, self->top_offset, 0,
		                          self->file_size, self->avg_line_len, page, page);
	}
	self->in_adj_update = FALSE;
}

static void
on_vadjust_value_changed (GtkAdjustment *adjustment, gpointer user_data)
{
	NemoPagedViewer *self = user_data;
	if (self->in_adj_update)
		return;
	gdouble value = gtk_adjustment_get_value (adjustment);
	if (self->mode == NEMO_VIEWER_MODE_HEX)
		value *= HEX_BPL;
	gint64 offset = value >= (gdouble) self->file_size ? self->file_size : (gint64) value;
	request_page (self, NAV_OFFSET, MAX (0, offset), 0);
}

static void
scroll_by_lines (NemoPagedViewer *self, int delta)
{
	gint64 offset = self->top_offset;
	if (self->page_cancel && self->pending_navigation == NAV_LINES) {
		offset = self->pending_offset;
		delta = CLAMP ((gint64) delta + self->pending_delta, -G_MAXINT, G_MAXINT);
	}
	request_page (self, NAV_LINES, offset, delta);
}

static gboolean
on_scroll_event (GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
	NemoPagedViewer *self = user_data;
	int lines = 0;
	switch (event->direction) {
	case GDK_SCROLL_UP: lines = -SCROLL_LINES; break;
	case GDK_SCROLL_DOWN: lines = SCROLL_LINES; break;
	case GDK_SCROLL_SMOOTH:
		self->scroll_accum += event->delta_y;
		lines = (int) self->scroll_accum;
		self->scroll_accum -= lines;
		lines *= SCROLL_LINES;
		break;
	default: return FALSE;
	}
	if (lines)
		scroll_by_lines (self, lines);
	return TRUE;
}

static gboolean
on_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	NemoPagedViewer *self = user_data;
	int page = MAX (1, self->vis_lines - 2);
	switch (event->keyval) {
	case GDK_KEY_Up:
	case GDK_KEY_k: scroll_by_lines (self, -1); return TRUE;
	case GDK_KEY_Down:
	case GDK_KEY_j: scroll_by_lines (self, 1); return TRUE;
	case GDK_KEY_Page_Up: scroll_by_lines (self, -page); return TRUE;
	case GDK_KEY_Page_Down:
	case GDK_KEY_space: scroll_by_lines (self, page); return TRUE;
	case GDK_KEY_Home: request_page (self, NAV_OFFSET, 0, 0); return TRUE;
	case GDK_KEY_End: request_page (self, NAV_END, 0, 0); return TRUE;
	default: return FALSE;
	}
}

static void
on_size_allocate (GtkWidget *widget, GtkAllocation *allocation, gpointer user_data)
{
	NemoPagedViewer *self = user_data;
	measure_font (self);
	int lines = MAX (1, (allocation->height - 2 * MARGIN_V) / self->char_h);
	int cols = MAX (1, (allocation->width - 2 * MARGIN_H) / self->char_w);
	if (lines == self->vis_lines && cols == self->vis_cols)
		return;
	self->vis_lines = lines;
	self->vis_cols = cols;
	update_scrollbar (self);
	if (self->page_cancel)
		request_page (self, self->pending_navigation, self->pending_offset, self->pending_delta);
	else
		request_page (self, NAV_OFFSET, self->top_offset, 0);
}

static void
reset_file (NemoPagedViewer *self)
{
	self->file_generation++;
	self->page_generation++;
	cancel_request (&self->open_cancel);
	cancel_request (&self->page_cancel);
	cancel_search (self);
	g_clear_object (&self->location);
	cache_clear (&self->cache);
	g_clear_pointer (&self->page, display_page_free);
	g_clear_pointer (&self->message, g_free);
	self->opening = self->opened = FALSE;
	self->file_size = self->top_offset = 0;
	self->search_match_offset = -1;
	self->scroll_accum = 0;
}

static void
nemo_paged_viewer_destroy (GtkWidget *widget)
{
	NemoPagedViewer *self = NEMO_PAGED_VIEWER (widget);
	self->destroyed = TRUE;
	reset_file (self);
	GTK_WIDGET_CLASS (nemo_paged_viewer_parent_class)->destroy (widget);
}

static void
nemo_paged_viewer_finalize (GObject *object)
{
	NemoPagedViewer *self = NEMO_PAGED_VIEWER (object);
	reset_file (self);
	g_free (self->search_needle);
	pango_font_description_free (self->font_desc);
	G_OBJECT_CLASS (nemo_paged_viewer_parent_class)->finalize (object);
}

static void
nemo_paged_viewer_class_init (NemoPagedViewerClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = nemo_paged_viewer_finalize;
	GTK_WIDGET_CLASS (klass)->destroy = nemo_paged_viewer_destroy;
	signals[LOAD_FINISHED] = g_signal_new ("load-finished", G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);
	signals[SEARCH_CHANGED] = g_signal_new ("search-changed", G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
nemo_paged_viewer_init (NemoPagedViewer *self)
{
	self->mode = NEMO_VIEWER_MODE_TEXT;
	self->char_w = 8;
	self->char_h = 16;
	self->vis_lines = 40;
	self->vis_cols = 80;
	self->avg_line_len = 80.0;
	self->search_match_offset = -1;
	self->font_desc = pango_font_description_from_string ("Monospace 10");
	gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_HORIZONTAL);
	self->drawing_area = gtk_drawing_area_new ();
	gtk_widget_set_can_focus (self->drawing_area, TRUE);
	gtk_widget_add_events (self->drawing_area,
		GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK);
	gtk_style_context_add_class (gtk_widget_get_style_context (self->drawing_area),
	                             GTK_STYLE_CLASS_VIEW);
	gtk_box_pack_start (GTK_BOX (self), self->drawing_area, TRUE, TRUE, 0);
	g_signal_connect (self->drawing_area, "draw", G_CALLBACK (on_draw), self);
	g_signal_connect (self->drawing_area, "scroll-event", G_CALLBACK (on_scroll_event), self);
	g_signal_connect (self->drawing_area, "key-press-event", G_CALLBACK (on_key_press), self);
	g_signal_connect (self->drawing_area, "size-allocate", G_CALLBACK (on_size_allocate), self);
	self->vadjust = gtk_adjustment_new (0, 0, 0, 1, 10, 10);
	self->scrollbar = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL, self->vadjust);
	gtk_box_pack_end (GTK_BOX (self), self->scrollbar, FALSE, FALSE, 0);
	g_signal_connect (self->vadjust, "value-changed",
	                  G_CALLBACK (on_vadjust_value_changed), self);
	gtk_widget_show_all (GTK_WIDGET (self));
}

NemoPagedViewer *
nemo_paged_viewer_new (void)
{
	return g_object_new (NEMO_TYPE_PAGED_VIEWER, NULL);
}

void
nemo_paged_viewer_open_location (NemoPagedViewer *self, GFile *location)
{
	g_return_if_fail (NEMO_IS_PAGED_VIEWER (self));
	g_return_if_fail (G_IS_FILE (location));
	if (self->destroyed)
		return;
	/* Keep the argument alive even if it aliases the previous location. */
	g_object_ref (location);
	reset_file (self);
	self->location = location;
	self->opening = TRUE;
	update_scrollbar (self);
	gtk_widget_queue_draw (self->drawing_area);
	gtk_widget_grab_focus (self->drawing_area);
	request_start (request_new (self, REQUEST_OPEN), &self->open_cancel);
	g_signal_emit (self, signals[SEARCH_CHANGED], 0);
}

gboolean
nemo_paged_viewer_open_file (NemoPagedViewer *self, const gchar *path, GError **error)
{
	g_return_val_if_fail (NEMO_IS_PAGED_VIEWER (self), FALSE);
	g_return_val_if_fail (path != NULL, FALSE);
	if (self->destroyed)
		return FALSE;
	GFile *location = g_file_new_for_path (path);
	nemo_paged_viewer_open_location (self, location);
	g_object_unref (location);
	return TRUE;
}

void
nemo_paged_viewer_close_file (NemoPagedViewer *self)
{
	g_return_if_fail (NEMO_IS_PAGED_VIEWER (self));
	reset_file (self);
	if (!self->destroyed) {
		update_scrollbar (self);
		gtk_widget_queue_draw (self->drawing_area);
		g_signal_emit (self, signals[SEARCH_CHANGED], 0);
	}
}

void
nemo_paged_viewer_set_mode (NemoPagedViewer *self, NemoViewerMode mode)
{
	g_return_if_fail (NEMO_IS_PAGED_VIEWER (self));
	if (self->mode == mode || self->destroyed)
		return;
	self->mode = mode;
	self->top_offset = 0;
	self->scroll_accum = 0;
	request_page (self, NAV_OFFSET, 0, 0);
	update_scrollbar (self);
	gtk_widget_queue_draw (self->drawing_area);
}

void
nemo_paged_viewer_search_set_needle (NemoPagedViewer *self, const gchar *needle)
{
	g_return_if_fail (NEMO_IS_PAGED_VIEWER (self));
	cancel_search (self);
	g_free (self->search_needle);
	self->search_needle = needle && *needle ? g_strdup (needle) : NULL;
	self->search_match_offset = -1;
	if (!self->destroyed) {
		gtk_widget_queue_draw (self->drawing_area);
		g_signal_emit (self, signals[SEARCH_CHANGED], 0);
	}
}

static gboolean
start_search (NemoPagedViewer *self, gboolean backwards)
{
	if (self->destroyed || !self->search_needle || !self->location ||
	    (!self->opened && !self->opening))
		return FALSE;
	cancel_search (self);
	self->search_pending = TRUE;
	if (self->opening) {
		self->deferred_search = TRUE;
		self->deferred_backwards = backwards;
	} else {
		Request *request = request_new (self, REQUEST_SEARCH);
		request->backwards = backwards;
		request_start (request, &self->search_cancel);
	}
	gtk_widget_queue_draw (self->drawing_area);
	g_signal_emit (self, signals[SEARCH_CHANGED], 0);
	return TRUE;
}

gboolean
nemo_paged_viewer_search_find_next (NemoPagedViewer *self)
{
	g_return_val_if_fail (NEMO_IS_PAGED_VIEWER (self), FALSE);
	return start_search (self, FALSE);
}

gboolean
nemo_paged_viewer_search_find_prev (NemoPagedViewer *self)
{
	g_return_val_if_fail (NEMO_IS_PAGED_VIEWER (self), FALSE);
	return start_search (self, TRUE);
}

void
nemo_paged_viewer_search_clear (NemoPagedViewer *self)
{
	nemo_paged_viewer_search_set_needle (self, NULL);
}

gboolean
nemo_paged_viewer_search_has_match (NemoPagedViewer *self)
{
	g_return_val_if_fail (NEMO_IS_PAGED_VIEWER (self), FALSE);
	return self->search_needle != NULL && self->search_match_offset >= 0;
}

gboolean
nemo_paged_viewer_search_is_pending (NemoPagedViewer *self)
{
	g_return_val_if_fail (NEMO_IS_PAGED_VIEWER (self), FALSE);
	return self->search_pending;
}

const GError *
nemo_paged_viewer_search_get_error (NemoPagedViewer *self)
{
	g_return_val_if_fail (NEMO_IS_PAGED_VIEWER (self), NULL);
	return self->search_error;
}

#else /* Preserve the vanilla synchronous implementation and API contract. */

#include "nemo-paged-viewer.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <glib/gi18n.h>

#define PAGE_SIZE       (64 * 1024)
#define MAX_PAGES       8
#define HEX_BPL         16
#define MARGIN_H        8
#define MARGIN_V        4
#define SCAN_BUF        4096
#define READ_CHUNK      (256 * 1024)
#define SCROLL_LINES    3

typedef struct {
	gint64 file_offset;
	guint8 *data;
	gsize valid_bytes;
	guint tick;
} PageSlot;

struct _NemoPagedViewer {
	GtkBox parent;
	GtkWidget *drawing_area;
	GtkWidget *scrollbar;
	GtkAdjustment *vadjust;
	gulong vadjust_handler;
	gboolean in_adj_update;
	int fd;
	gint64 file_size;
	PageSlot cache[MAX_PAGES];
	int n_cached;
	guint tick;
	NemoViewerMode mode;
	PangoFontDescription *font_desc;
	int char_w;
	int char_h;
	gboolean font_measured;
	int vis_lines;
	int vis_cols;
	gint64 top_offset;
	gdouble avg_line_len;
	gdouble scroll_accum;
	gchar *search_needle;
	gsize search_needle_len;
	gint64 search_match_offset;
	gboolean search_active;
};

G_DEFINE_TYPE (NemoPagedViewer, nemo_paged_viewer, GTK_TYPE_BOX)

static void update_scrollbar (NemoPagedViewer *self);
static void scroll_by_lines (NemoPagedViewer *self, int delta);
static gssize cache_read (NemoPagedViewer *self, gint64 off, void *buf, gsize count);

static const guint8 *
mem_icasefind (const guint8 *hay, gsize hlen, const guint8 *ndl, gsize nlen)
{
	if (nlen == 0) return hay;
	if (nlen > hlen) return NULL;
	for (gsize i = 0; i <= hlen - nlen; i++) {
		gboolean ok = TRUE;
		for (gsize j = 0; j < nlen; j++) {
			if (g_ascii_tolower (hay[i + j]) != ndl[j]) {
				ok = FALSE;
				break;
			}
		}
		if (ok) return hay + i;
	}
	return NULL;
}

static gint64
search_scan_forward (NemoPagedViewer *self, gint64 start_offset)
{
	const guint8 *ndl = (const guint8 *) self->search_needle;
	gsize nlen = self->search_needle_len;
	gsize overlap = (nlen > 1) ? nlen - 1 : 0;
	gsize chunk_size = (gsize) MIN ((gint64) READ_CHUNK, self->file_size);
	guint8 *chunk;
	gint64 pos;
	if (nlen == 0 || self->fd < 0 || self->file_size == 0)
		return -1;
	chunk = g_malloc (chunk_size);
	pos = start_offset;
	while (pos < self->file_size) {
		gssize nread = cache_read (self, pos, chunk, chunk_size);
		if (nread <= 0) break;
		const guint8 *found = mem_icasefind (chunk, (gsize) nread, ndl, nlen);
		if (found) {
			gint64 match = pos + (gint64) (found - chunk);
			g_free (chunk);
			return match;
		}
		gsize advance = ((gsize) nread > overlap) ? (gsize) nread - overlap : 1;
		pos += (gint64) advance;
	}
	g_free (chunk);
	return -1;
}

static gint64
search_scan_backward (NemoPagedViewer *self, gint64 before_offset)
{
	const guint8 *ndl = (const guint8 *) self->search_needle;
	gsize nlen = self->search_needle_len;
	gsize overlap = (nlen > 1) ? nlen - 1 : 0;
	gsize chunk_size = (gsize) MIN ((gint64) READ_CHUNK, self->file_size);
	guint8 *chunk;
	gint64 pos;
	gint64 last;
	if (nlen == 0 || before_offset <= 0 || self->fd < 0)
		return -1;
	chunk = g_malloc (chunk_size);
	pos = 0;
	last = -1;
	while (pos < before_offset) {
		gsize max_read = MIN (chunk_size, (gsize)(before_offset - pos) + overlap);
		gssize nread = cache_read (self, pos, chunk, max_read);
		if (nread <= 0) break;
		const guint8 *p = chunk;
		gsize remaining = (gsize) nread;
		while (remaining >= nlen) {
			const guint8 *found = mem_icasefind (p, remaining, ndl, nlen);
			if (!found) break;
			gint64 abs = pos + (gint64) (found - chunk);
			if (abs < before_offset)
				last = abs;
			p = found + 1;
			remaining = (gsize) ((chunk + nread) - p);
		}
		gsize advance = ((gsize) nread > overlap) ? (gsize) nread - overlap : 1;
		pos += (gint64) advance;
	}
	g_free (chunk);
	return last;
}

static void
scroll_to_match (NemoPagedViewer *self)
{
	if (self->search_match_offset < 0)
		return;
	if (self->mode == NEMO_VIEWER_MODE_HEX) {
		gint64 match_row = self->search_match_offset / HEX_BPL;
		gint64 top_row = MAX (0, match_row - 2);
		self->top_offset = top_row * HEX_BPL;
	} else {
		gint64 pos = self->search_match_offset;
		gsize scan_back = (gsize) MIN (pos, (gint64) SCAN_BUF);
		gint64 new_top = 0;
		if (scan_back > 0) {
			guint8 *buf = g_malloc (scan_back);
			gssize nread = cache_read (self, pos - (gint64) scan_back, buf, scan_back);
			if (nread > 0) {
				const guint8 *p = (const guint8 *) buf + nread - 1;
				int nl = 0;
				while (p >= buf) {
					if (*p == '\n' && ++nl >= 3) break;
					p--;
				}
				new_top = (nl >= 3)
					? pos - (gint64) scan_back + (gint64) (p - buf) + 1
					: 0;
			}
			g_free (buf);
		}
		self->top_offset = MAX (0, new_top);
	}
	update_scrollbar (self);
	gtk_widget_queue_draw (self->drawing_area);
}

static void
cache_clear (NemoPagedViewer *self)
{
	for (int i = 0; i < self->n_cached; i++) {
		g_free (self->cache[i].data);
		self->cache[i].data = NULL;
		self->cache[i].file_offset = -1;
	}
	self->n_cached = 0;
	self->tick = 0;
}

static PageSlot *
cache_get_page (NemoPagedViewer *self, gint64 base)
{
	for (int i = 0; i < self->n_cached; i++) {
		if (self->cache[i].file_offset == base) {
			self->cache[i].tick = ++self->tick;
			return &self->cache[i];
		}
	}
	PageSlot *slot;
	if (self->n_cached < MAX_PAGES) {
		slot = &self->cache[self->n_cached++];
		slot->data = g_malloc (PAGE_SIZE);
	} else {
		slot = &self->cache[0];
		for (int i = 1; i < MAX_PAGES; i++) {
			if (self->cache[i].tick < slot->tick)
				slot = &self->cache[i];
		}
	}
	gssize n = pread (self->fd, slot->data, PAGE_SIZE, base);
	if (n < 0) n = 0;
	slot->file_offset = base;
	slot->valid_bytes = (gsize) n;
	slot->tick = ++self->tick;
	return slot;
}

static gssize
cache_read (NemoPagedViewer *self, gint64 off, void *buf, gsize count)
{
	if (self->fd < 0 || off < 0) return -1;
	if (off >= self->file_size) return 0;
	if ((gint64)(off + count) > self->file_size)
		count = (gsize)(self->file_size - off);
	guint8 *p = (guint8 *) buf;
	gsize left = count;
	while (left > 0) {
		gint64 base = off & ~((gint64) PAGE_SIZE - 1);
		gsize off_in = (gsize)(off - base);
		PageSlot *s = cache_get_page (self, base);
		if (s == NULL || s->valid_bytes <= off_in)
			break;
		gsize avail = s->valid_bytes - off_in;
		gsize n = MIN (avail, left);
		memcpy (p, s->data + off_in, n);
		p += n;
		off += n;
		left -= n;
	}
	return (gssize)(count - left);
}

static gint64
find_line_start (NemoPagedViewer *self, gint64 offset)
{
	if (offset <= 0) return 0;
	guint8 buf[SCAN_BUF];
	gint64 scan = offset - 1;
	while (scan >= 0) {
		gint64 chunk_start = MAX (0, scan - (gint64) sizeof (buf) + 1);
		gsize chunk_len = (gsize)(scan - chunk_start + 1);
		gssize n = cache_read (self, chunk_start, buf, chunk_len);
		if (n <= 0) return 0;
		for (gssize i = n - 1; i >= 0; i--) {
			if (buf[i] == '\n')
				return chunk_start + i + 1;
		}
		if (chunk_start == 0) return 0;
		scan = chunk_start - 1;
	}
	return 0;
}

static gint64
skip_lines_forward (NemoPagedViewer *self, gint64 offset, int n)
{
	guint8 buf[SCAN_BUF];
	gint64 pos = offset;
	int found = 0;
	while (found < n && pos < self->file_size) {
		gsize to_read = (gsize) MIN ((gint64) sizeof (buf), self->file_size - pos);
		gssize nr = cache_read (self, pos, buf, to_read);
		if (nr <= 0) break;
		for (gssize i = 0; i < nr; i++) {
			if (buf[i] == '\n') {
				found++;
				if (found == n)
					return MIN (pos + i + 1, self->file_size);
			}
		}
		pos += nr;
	}
	return MIN (pos, self->file_size);
}

static gint64
skip_lines_backward (NemoPagedViewer *self, gint64 offset, int n)
{
	gint64 pos = offset;
	for (int i = 0; i < n && pos > 0; i++)
		pos = find_line_start (self, pos - 1);
	return pos;
}

static gdouble
estimate_avg_line_len (NemoPagedViewer *self)
{
	guint8 buf[PAGE_SIZE];
	gssize n = cache_read (self, 0, buf, sizeof (buf));
	if (n <= 0) return 80.0;
	int newlines = 0;
	for (gssize i = 0; i < n; i++) {
		if (buf[i] == '\n')
			newlines++;
	}
	if (newlines == 0)
		return (gdouble) n;
	return (gdouble) n / (gdouble) newlines;
}

static void
measure_font (NemoPagedViewer *self)
{
	PangoContext *ctx;
	PangoFontMetrics *m;
	if (self->font_measured)
		return;
	ctx = gtk_widget_get_pango_context (self->drawing_area);
	if (ctx == NULL)
		return;
	m = pango_context_get_metrics (ctx, self->font_desc, NULL);
	self->char_w = pango_font_metrics_get_approximate_char_width (m) / PANGO_SCALE;
	self->char_h = (pango_font_metrics_get_ascent (m) +
	                pango_font_metrics_get_descent (m)) / PANGO_SCALE + 2;
	pango_font_metrics_unref (m);
	if (self->char_w < 1) self->char_w = 8;
	if (self->char_h < 1) self->char_h = 16;
	self->font_measured = TRUE;
}

static void
draw_text_mode (NemoPagedViewer *self, cairo_t *cr, PangoLayout *layout,
                int width, int height)
{
	guint8 *chunk;
	gssize nread;
	int y = MARGIN_V;
	int lines_drawn = 0;
	gsize pos = 0;
	gint64 line_byte_offset;
	gsize chunk_size = (gsize) MIN ((gint64) READ_CHUNK,
	                                self->file_size - self->top_offset);
	if (chunk_size == 0) return;
	chunk = g_malloc (chunk_size);
	nread = cache_read (self, self->top_offset, chunk, chunk_size);
	if (nread <= 0) {
		g_free (chunk);
		return;
	}
	line_byte_offset = self->top_offset;
	while (lines_drawn < self->vis_lines && pos < (gsize) nread) {
		gsize line_end = pos;
		while (line_end < (gsize) nread && chunk[line_end] != '\n')
			line_end++;
		gsize line_len = line_end - pos;
		gchar *safe = g_utf8_make_valid ((const gchar *)(chunk + pos), (gssize) line_len);
		pango_layout_set_text (layout, safe, -1);
		if (self->search_active && self->search_match_offset >= 0 &&
		    self->search_match_offset >= line_byte_offset &&
		    self->search_match_offset < line_byte_offset + (gint64) line_len) {
			gsize match_in_line = (gsize)(self->search_match_offset - line_byte_offset);
			gsize match_end = MIN (match_in_line + self->search_needle_len, strlen (safe));
			PangoAttrList *attrs = pango_attr_list_new ();
			PangoAttribute *bg = pango_attr_background_new (0xFFFF, 0xD700, 0x0000);
			bg->start_index = (guint) match_in_line;
			bg->end_index = (guint) match_end;
			pango_attr_list_insert (attrs, bg);
			PangoAttribute *fg_attr = pango_attr_foreground_new (0, 0, 0);
			fg_attr->start_index = (guint) match_in_line;
			fg_attr->end_index = (guint) match_end;
			pango_attr_list_insert (attrs, fg_attr);
			pango_layout_set_attributes (layout, attrs);
			pango_attr_list_unref (attrs);
		}
		cairo_move_to (cr, MARGIN_H, y);
		pango_cairo_show_layout (cr, layout);
		pango_layout_set_attributes (layout, NULL);
		g_free (safe);
		y += self->char_h;
		line_byte_offset += (gint64)(line_len + 1);
		pos = line_end + 1;
		lines_drawn++;
	}
	g_free (chunk);
}

static void
draw_hex_mode (NemoPagedViewer *self, cairo_t *cr, PangoLayout *layout,
               int width, int height)
{
	gsize needed = (gsize)(self->vis_lines + 1) * HEX_BPL;
	guint8 *data;
	gssize nread;
	int y = MARGIN_V;
	gsize pos = 0;
	char line_buf[256];
	if (needed > READ_CHUNK) needed = READ_CHUNK;
	data = g_malloc (needed);
	nread = cache_read (self, self->top_offset, data, needed);
	if (nread <= 0) {
		g_free (data);
		return;
	}
	for (int row = 0; row < self->vis_lines && pos < (gsize) nread; row++) {
		gsize row_len = MIN (HEX_BPL, (gsize) nread - pos);
		int off = 0;
		off += snprintf (line_buf + off, sizeof (line_buf) - off,
		                 "%08lx  ", (unsigned long)(self->top_offset + pos));
		for (int i = 0; i < HEX_BPL; i++) {
			if ((gsize) i < row_len)
				off += snprintf (line_buf + off, sizeof (line_buf) - off,
				                 "%02x ", data[pos + i]);
			else
				off += snprintf (line_buf + off, sizeof (line_buf) - off, "   ");
			if (i == 7)
				line_buf[off++] = ' ';
		}
		off += snprintf (line_buf + off, sizeof (line_buf) - off, " |");
		for (gsize i = 0; i < row_len; i++) {
			guint8 c = data[pos + i];
			line_buf[off++] = (c >= 0x20 && c <= 0x7e) ? c : '.';
		}
		line_buf[off++] = '|';
		line_buf[off] = '\0';
		pango_layout_set_text (layout, line_buf, off);
		if (self->search_active && self->search_match_offset >= 0) {
			gint64 row_abs = self->top_offset + (gint64) pos;
			if (self->search_match_offset >= row_abs &&
			    self->search_match_offset < row_abs + (gint64) row_len) {
				cairo_save (cr);
				cairo_set_source_rgba (cr, 1.0, 0.85, 0.0, 0.35);
				cairo_rectangle (cr, 0.0, (double) y,
				                 (double) width, (double) self->char_h);
				cairo_fill (cr);
				cairo_restore (cr);
			}
		}
		cairo_move_to (cr, MARGIN_H, y);
		pango_cairo_show_layout (cr, layout);
		y += self->char_h;
		pos += HEX_BPL;
	}
	g_free (data);
}

static gboolean
on_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
	NemoPagedViewer *self = NEMO_PAGED_VIEWER (user_data);
	GtkStyleContext *style_ctx;
	GdkRGBA fg;
	GtkAllocation alloc;
	PangoLayout *layout;
	measure_font (self);
	gtk_widget_get_allocation (widget, &alloc);
	style_ctx = gtk_widget_get_style_context (widget);
	gtk_render_background (style_ctx, cr, 0, 0, alloc.width, alloc.height);
	if (self->fd < 0 || self->file_size == 0)
		return TRUE;
	gtk_style_context_get_color (style_ctx, GTK_STATE_FLAG_NORMAL, &fg);
	gdk_cairo_set_source_rgba (cr, &fg);
	layout = pango_cairo_create_layout (cr);
	pango_layout_set_font_description (layout, self->font_desc);
	pango_layout_set_width (layout, (alloc.width - 2 * MARGIN_H) * PANGO_SCALE);
	pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
	if (self->mode == NEMO_VIEWER_MODE_HEX)
		draw_hex_mode (self, cr, layout, alloc.width, alloc.height);
	else
		draw_text_mode (self, cr, layout, alloc.width, alloc.height);
	g_object_unref (layout);
	return TRUE;
}

static void
update_scrollbar (NemoPagedViewer *self)
{
	if (self->fd < 0 || self->file_size == 0) {
		gtk_adjustment_configure (self->vadjust, 0, 0, 0, 0, 0, 0);
		return;
	}
	self->in_adj_update = TRUE;
	if (self->mode == NEMO_VIEWER_MODE_HEX) {
		gint64 total_lines = (self->file_size + HEX_BPL - 1) / HEX_BPL;
		gint64 cur_line = self->top_offset / HEX_BPL;
		gdouble page = MAX (1, self->vis_lines);
		gtk_adjustment_configure (self->vadjust, (gdouble) cur_line, 0.0,
		                          (gdouble) total_lines, 1.0, page, page);
	} else {
		gdouble page = self->vis_lines * self->avg_line_len;
		if (page < 1.0) page = 1.0;
		gtk_adjustment_configure (self->vadjust, (gdouble) self->top_offset,
		                          0.0, (gdouble) self->file_size,
		                          self->avg_line_len, page, page);
	}
	self->in_adj_update = FALSE;
}

static void
on_vadjust_value_changed (GtkAdjustment *adj, gpointer user_data)
{
	NemoPagedViewer *self = NEMO_PAGED_VIEWER (user_data);
	if (self->in_adj_update)
		return;
	if (self->mode == NEMO_VIEWER_MODE_HEX) {
		gint64 line = (gint64) gtk_adjustment_get_value (adj);
		self->top_offset = line * HEX_BPL;
		if (self->top_offset > self->file_size)
			self->top_offset = (self->file_size / HEX_BPL) * HEX_BPL;
	} else {
		gint64 offset = (gint64) gtk_adjustment_get_value (adj);
		if (offset > 0 && offset < self->file_size)
			self->top_offset = find_line_start (self, offset);
		else
			self->top_offset = offset;
	}
	if (self->top_offset < 0) self->top_offset = 0;
	gtk_widget_queue_draw (self->drawing_area);
}

static void
scroll_by_lines (NemoPagedViewer *self, int delta)
{
	if (self->fd < 0) return;
	if (self->mode == NEMO_VIEWER_MODE_HEX) {
		gint64 cur_line = self->top_offset / HEX_BPL;
		gint64 total = (self->file_size + HEX_BPL - 1) / HEX_BPL;
		gint64 max_line = MAX (0, total - self->vis_lines);
		cur_line += delta;
		if (cur_line < 0) cur_line = 0;
		if (cur_line > max_line) cur_line = max_line;
		self->top_offset = cur_line * HEX_BPL;
	} else {
		if (delta > 0) {
			self->top_offset = skip_lines_forward (self, self->top_offset, delta);
		} else if (delta < 0) {
			self->top_offset = skip_lines_backward (self, self->top_offset, -delta);
		}
	}
	if (self->top_offset < 0) self->top_offset = 0;
	if (self->top_offset > self->file_size) self->top_offset = self->file_size;
	update_scrollbar (self);
	gtk_widget_queue_draw (self->drawing_area);
}

static gboolean
on_scroll_event (GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
	NemoPagedViewer *self = NEMO_PAGED_VIEWER (user_data);
	int lines = 0;
	switch (event->direction) {
	case GDK_SCROLL_UP:
		lines = -SCROLL_LINES;
		break;
	case GDK_SCROLL_DOWN:
		lines = SCROLL_LINES;
		break;
	case GDK_SCROLL_SMOOTH:
		self->scroll_accum += event->delta_y;
		lines = (int) self->scroll_accum;
		self->scroll_accum -= (gdouble) lines;
		lines *= SCROLL_LINES;
		break;
	default:
		return FALSE;
	}
	if (lines != 0)
		scroll_by_lines (self, lines);
	return TRUE;
}

static gboolean
on_key_press (GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
	NemoPagedViewer *self = NEMO_PAGED_VIEWER (user_data);
	int page = MAX (1, self->vis_lines - 2);
	switch (event->keyval) {
	case GDK_KEY_Up:
	case GDK_KEY_k:
		scroll_by_lines (self, -1);
		return TRUE;
	case GDK_KEY_Down:
	case GDK_KEY_j:
		scroll_by_lines (self, 1);
		return TRUE;
	case GDK_KEY_Page_Up:
		scroll_by_lines (self, -page);
		return TRUE;
	case GDK_KEY_Page_Down:
	case GDK_KEY_space:
		scroll_by_lines (self, page);
		return TRUE;
	case GDK_KEY_Home:
		self->top_offset = 0;
		update_scrollbar (self);
		gtk_widget_queue_draw (self->drawing_area);
		return TRUE;
	case GDK_KEY_End: {
		if (self->mode == NEMO_VIEWER_MODE_HEX) {
			gint64 total = (self->file_size + HEX_BPL - 1) / HEX_BPL;
			gint64 max_line = MAX (0, total - self->vis_lines);
			self->top_offset = max_line * HEX_BPL;
		} else {
			self->top_offset = skip_lines_backward (self, self->file_size,
			                                         self->vis_lines - 1);
		}
		update_scrollbar (self);
		gtk_widget_queue_draw (self->drawing_area);
		return TRUE;
	}
	default:
		break;
	}
	return FALSE;
}

static void
on_size_allocate (GtkWidget *widget, GtkAllocation *alloc, gpointer user_data)
{
	NemoPagedViewer *self = NEMO_PAGED_VIEWER (user_data);
	measure_font (self);
	if (self->char_h > 0)
		self->vis_lines = (alloc->height - 2 * MARGIN_V) / self->char_h;
	if (self->char_w > 0)
		self->vis_cols = (alloc->width - 2 * MARGIN_H) / self->char_w;
	if (self->vis_lines < 1) self->vis_lines = 1;
	if (self->vis_cols < 1) self->vis_cols = 1;
	update_scrollbar (self);
}

static void
nemo_paged_viewer_init (NemoPagedViewer *self)
{
	self->fd = -1;
	self->file_size = 0;
	self->n_cached = 0;
	self->tick = 0;
	self->mode = NEMO_VIEWER_MODE_TEXT;
	self->font_measured = FALSE;
	self->char_w = 8;
	self->char_h = 16;
	self->vis_lines = 40;
	self->vis_cols = 80;
	self->top_offset = 0;
	self->avg_line_len = 80.0;
	self->scroll_accum = 0.0;
	self->in_adj_update = FALSE;
	gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_HORIZONTAL);
	self->search_needle = NULL;
	self->search_needle_len = 0;
	self->search_match_offset = -1;
	self->search_active = FALSE;
	self->font_desc = pango_font_description_from_string ("Monospace 10");
	self->drawing_area = gtk_drawing_area_new ();
	gtk_widget_set_can_focus (self->drawing_area, TRUE);
	gtk_widget_add_events (self->drawing_area,
	                       GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK);
	gtk_style_context_add_class (gtk_widget_get_style_context (self->drawing_area),
	                             GTK_STYLE_CLASS_VIEW);
	gtk_box_pack_start (GTK_BOX (self), self->drawing_area, TRUE, TRUE, 0);
	g_signal_connect (self->drawing_area, "draw", G_CALLBACK (on_draw), self);
	g_signal_connect (self->drawing_area, "scroll-event", G_CALLBACK (on_scroll_event), self);
	g_signal_connect (self->drawing_area, "key-press-event", G_CALLBACK (on_key_press), self);
	g_signal_connect (self->drawing_area, "size-allocate", G_CALLBACK (on_size_allocate), self);
	self->vadjust = gtk_adjustment_new (0, 0, 0, 1, 10, 10);
	self->scrollbar = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL, self->vadjust);
	gtk_box_pack_end (GTK_BOX (self), self->scrollbar, FALSE, FALSE, 0);
	self->vadjust_handler = g_signal_connect (self->vadjust, "value-changed",
	                                          G_CALLBACK (on_vadjust_value_changed), self);
	gtk_widget_show_all (GTK_WIDGET (self));
}

static void
nemo_paged_viewer_finalize (GObject *object)
{
	NemoPagedViewer *self = NEMO_PAGED_VIEWER (object);
	nemo_paged_viewer_close_file (self);
	g_free (self->search_needle);
	self->search_needle = NULL;
	if (self->font_desc != NULL)
		pango_font_description_free (self->font_desc);
	G_OBJECT_CLASS (nemo_paged_viewer_parent_class)->finalize (object);
}

static void
nemo_paged_viewer_class_init (NemoPagedViewerClass *klass)
{
	GObjectClass *obj_class = G_OBJECT_CLASS (klass);
	obj_class->finalize = nemo_paged_viewer_finalize;
}

NemoPagedViewer *
nemo_paged_viewer_new (void)
{
	return g_object_new (NEMO_TYPE_PAGED_VIEWER, NULL);
}

gboolean
nemo_paged_viewer_open_file (NemoPagedViewer *self, const gchar *path, GError **error)
{
	struct stat st;
	g_return_val_if_fail (NEMO_IS_PAGED_VIEWER (self), FALSE);
	g_return_val_if_fail (path != NULL, FALSE);
	nemo_paged_viewer_close_file (self);
	self->fd = open (path, O_RDONLY);
	if (self->fd < 0) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		             "Could not open %s: %s", path, g_strerror (errno));
		return FALSE;
	}
	if (fstat (self->fd, &st) < 0) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		             "Could not stat %s: %s", path, g_strerror (errno));
		close (self->fd);
		self->fd = -1;
		return FALSE;
	}
	self->file_size = st.st_size;
	self->top_offset = 0;
	self->scroll_accum = 0.0;
	self->avg_line_len = estimate_avg_line_len (self);
	update_scrollbar (self);
	gtk_widget_queue_draw (self->drawing_area);
	gtk_widget_grab_focus (self->drawing_area);
	return TRUE;
}

void
nemo_paged_viewer_close_file (NemoPagedViewer *self)
{
	g_return_if_fail (NEMO_IS_PAGED_VIEWER (self));
	cache_clear (self);
	if (self->fd >= 0) {
		close (self->fd);
		self->fd = -1;
	}
	self->file_size = 0;
	self->top_offset = 0;
	self->search_match_offset = -1;
	update_scrollbar (self);
	gtk_widget_queue_draw (self->drawing_area);
}

void
nemo_paged_viewer_set_mode (NemoPagedViewer *self, NemoViewerMode mode)
{
	g_return_if_fail (NEMO_IS_PAGED_VIEWER (self));
	if (self->mode == mode)
		return;
	self->mode = mode;
	self->top_offset = 0;
	self->scroll_accum = 0.0;
	update_scrollbar (self);
	gtk_widget_queue_draw (self->drawing_area);
}

void
nemo_paged_viewer_search_set_needle (NemoPagedViewer *self, const gchar *needle)
{
	g_return_if_fail (NEMO_IS_PAGED_VIEWER (self));
	g_free (self->search_needle);
	self->search_needle = NULL;
	self->search_needle_len = 0;
	self->search_match_offset = -1;
	self->search_active = FALSE;
	if (needle != NULL && needle[0] != '\0') {
		self->search_needle = g_ascii_strdown (needle, -1);
		self->search_needle_len = strlen (self->search_needle);
		self->search_active = TRUE;
	}
	gtk_widget_queue_draw (self->drawing_area);
}

gboolean
nemo_paged_viewer_search_find_next (NemoPagedViewer *self)
{
	gint64 start_from;
	gint64 found;
	g_return_val_if_fail (NEMO_IS_PAGED_VIEWER (self), FALSE);
	if (!self->search_active || self->fd < 0)
		return FALSE;
	start_from = (self->search_match_offset >= 0)
		? self->search_match_offset + 1 : self->top_offset;
	found = search_scan_forward (self, start_from);
	if (found < 0 && start_from > 0) {
		found = search_scan_forward (self, 0);
	}
	if (found >= 0) {
		self->search_match_offset = found;
		scroll_to_match (self);
		return TRUE;
	}
	return FALSE;
}

gboolean
nemo_paged_viewer_search_find_prev (NemoPagedViewer *self)
{
	gint64 before;
	gint64 found;
	g_return_val_if_fail (NEMO_IS_PAGED_VIEWER (self), FALSE);
	if (!self->search_active || self->fd < 0)
		return FALSE;
	before = (self->search_match_offset >= 0) ? self->search_match_offset : self->top_offset;
	found = search_scan_backward (self, before);
	if (found < 0) {
		found = search_scan_backward (self, self->file_size);
	}
	if (found >= 0) {
		self->search_match_offset = found;
		scroll_to_match (self);
		return TRUE;
	}
	return FALSE;
}

void
nemo_paged_viewer_search_clear (NemoPagedViewer *self)
{
	g_return_if_fail (NEMO_IS_PAGED_VIEWER (self));
	g_free (self->search_needle);
	self->search_needle = NULL;
	self->search_needle_len = 0;
	self->search_match_offset = -1;
	self->search_active = FALSE;
	gtk_widget_queue_draw (self->drawing_area);
}

gboolean
nemo_paged_viewer_search_has_match (NemoPagedViewer *self)
{
	g_return_val_if_fail (NEMO_IS_PAGED_VIEWER (self), FALSE);
	return self->search_active && (self->search_match_offset >= 0);
}

#endif /* NEMO_SMPL */
