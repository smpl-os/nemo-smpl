/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * nemo-paged-viewer.c — Large-file viewer with pread()-based page cache
 *
 * Architecture (inspired by `less`, Double Commander, GHex):
 *
 *   ┌─────────────────────────────────────────────┐
 *   │  NemoPagedViewer  (GtkBox, horizontal)       │
 *   │  ┌──────────────────────┐  ┌─────────────┐  │
 *   │  │   GtkDrawingArea     │  │ GtkScrollbar │  │
 *   │  │  (Pango/Cairo text)  │  │  (vertical)  │  │
 *   │  │                      │  │              │  │
 *   │  │ draw() reads from    │  │ adjustment   │  │
 *   │  │ PageCache → pread()  │  │ tracks byte  │  │
 *   │  │                      │  │ offset       │  │
 *   │  └──────────────────────┘  └─────────────┘  │
 *   └─────────────────────────────────────────────┘
 *
 * Page cache: 8 × 64 KB slots = 512 KB max resident memory,
 * regardless of file size (handles multi-GB ISOs etc.)
 *
 * Copyright (C) 2026 smplOS contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#include <config.h>
#include "nemo-paged-viewer.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <glib/gi18n.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                           */
/* ------------------------------------------------------------------ */

#define PAGE_SIZE       (64 * 1024)     /* 64 KB per cache page         */
#define MAX_PAGES       8               /* 512 KB total cache           */
#define HEX_BPL         16              /* bytes per hex line           */
#define MARGIN_H        8               /* horizontal margin (px)       */
#define MARGIN_V        4               /* vertical margin (px)         */
#define SCAN_BUF        4096            /* backward scan buffer         */
#define READ_CHUNK      (256 * 1024)    /* max read for one screen      */
#define SCROLL_LINES    3               /* lines per mouse wheel click  */

/* ------------------------------------------------------------------ */
/* Page cache slot                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
	gint64  file_offset;    /* -1 = unused */
	guint8 *data;           /* g_malloc'd, PAGE_SIZE bytes */
	gsize   valid_bytes;    /* actual bytes (< PAGE_SIZE at EOF) */
	guint   tick;           /* LRU counter */
} PageSlot;

/* ------------------------------------------------------------------ */
/* Widget struct                                                      */
/* ------------------------------------------------------------------ */

struct _NemoPagedViewer {
	GtkBox parent;

	/* Widgets */
	GtkWidget       *drawing_area;
	GtkWidget       *scrollbar;
	GtkAdjustment   *vadjust;
	gulong           vadjust_handler;
	gboolean         in_adj_update;

	/* File state */
	int              fd;
	gint64           file_size;

	/* Page cache */
	PageSlot         cache[MAX_PAGES];
	int              n_cached;
	guint            tick;

	/* Display mode */
	NemoViewerMode   mode;

	/* Font metrics */
	PangoFontDescription *font_desc;
	int              char_w;        /* monospace char width (px)  */
	int              char_h;        /* line height (px)           */
	gboolean         font_measured;

	/* Viewport */
	int              vis_lines;     /* visible line count         */
	int              vis_cols;      /* visible column count        */
	gint64           top_offset;    /* byte offset of first line  */

	/* Text mode helpers */
	gdouble          avg_line_len;  /* estimated bytes per line   */

	/* Smooth scroll accumulator */
	gdouble          scroll_accum;

	/* Search state (case-insensitive ASCII) */
	gchar           *search_needle;         /* g_ascii_strdown'd needle, NULL if inactive */
	gsize            search_needle_len;     /* byte length */
	gint64           search_match_offset;   /* absolute file offset of current match, -1 = none */
	gboolean         search_active;         /* TRUE while a needle is set */
};

G_DEFINE_TYPE (NemoPagedViewer, nemo_paged_viewer, GTK_TYPE_BOX)

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

static void     update_scrollbar       (NemoPagedViewer *self);
static void     scroll_by_lines        (NemoPagedViewer *self, int delta);
static gssize   cache_read             (NemoPagedViewer *self, gint64 off, void *buf, gsize count);

/* ------------------------------------------------------------------ */
/* Search helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Case-insensitive ASCII byte search.
 * needle must already be g_ascii_strdown'd.
 * Returns pointer to first match in [hay, hay+hlen) or NULL.
 */
static const guint8 *
mem_icasefind (const guint8 *hay, gsize hlen,
               const guint8 *ndl, gsize nlen)
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

/* Scan forward from start_offset; return absolute file offset of next match
 * or -1 if none found.  Reads in READ_CHUNK blocks with needle-1 byte overlap
 * so matches spanning chunk boundaries are found. */
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

		/* Advance, keeping an overlap window to catch cross-boundary matches */
		gsize advance = ((gsize) nread > overlap) ? (gsize) nread - overlap : 1;
		pos += (gint64) advance;
	}

	g_free (chunk);
	return -1;
}

/* Scan backward before before_offset; return the last match offset or -1.
 * Strategy: scan forward from 0 collecting all matches < before_offset,
 * returning the highest one. */
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

		/* Collect all occurrences in this chunk that start before before_offset */
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

/* Scroll top_offset so the match is near the top of the viewport. */
static void
scroll_to_match (NemoPagedViewer *self)
{
	if (self->search_match_offset < 0)
		return;

	if (self->mode == NEMO_VIEWER_MODE_HEX) {
		gint64 match_row = self->search_match_offset / HEX_BPL;
		gint64 top_row   = MAX (0, match_row - 2);
		self->top_offset = top_row * HEX_BPL;
	} else {
		/* Scan back through the file to find a line ~3 above the match,
		 * so the matched line appears with a couple of lines of context. */
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

/* ================================================================== */
/*                          PAGE CACHE                                */
/* ================================================================== */

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
	/* Look up existing */
	for (int i = 0; i < self->n_cached; i++) {
		if (self->cache[i].file_offset == base) {
			self->cache[i].tick = ++self->tick;
			return &self->cache[i];
		}
	}

	/* Cache miss — find or allocate a slot */
	PageSlot *slot;
	if (self->n_cached < MAX_PAGES) {
		slot = &self->cache[self->n_cached++];
		slot->data = g_malloc (PAGE_SIZE);
	} else {
		/* Evict LRU */
		slot = &self->cache[0];
		for (int i = 1; i < MAX_PAGES; i++) {
			if (self->cache[i].tick < slot->tick)
				slot = &self->cache[i];
		}
	}

	/* Read from disk */
	gssize n = pread (self->fd, slot->data, PAGE_SIZE, base);
	if (n < 0) n = 0;

	slot->file_offset = base;
	slot->valid_bytes = (gsize) n;
	slot->tick = ++self->tick;
	return slot;
}

/* Read 'count' bytes starting at 'off' into 'buf', using the page cache.
 * Returns number of bytes read, or -1 on error. */
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
		gsize  off_in = (gsize)(off - base);
		PageSlot *s = cache_get_page (self, base);

		if (s == NULL || s->valid_bytes <= off_in)
			break;

		gsize avail = s->valid_bytes - off_in;
		gsize n = MIN (avail, left);
		memcpy (p, s->data + off_in, n);

		p    += n;
		off  += n;
		left -= n;
	}

	return (gssize)(count - left);
}

/* ================================================================== */
/*                     LINE NAVIGATION (TEXT MODE)                    */
/* ================================================================== */

/* Find the start of the line containing the byte at 'offset'
 * by scanning backward for '\n'.  Returns 0 at BOF. */
static gint64
find_line_start (NemoPagedViewer *self, gint64 offset)
{
	if (offset <= 0) return 0;

	guint8 buf[SCAN_BUF];
	gint64 scan = offset - 1;

	while (scan >= 0) {
		gint64 chunk_start = MAX (0, scan - (gint64) sizeof (buf) + 1);
		gsize  chunk_len   = (gsize)(scan - chunk_start + 1);
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

/* Move forward past 'n' newlines from 'offset'. */
static gint64
skip_lines_forward (NemoPagedViewer *self, gint64 offset, int n)
{
	guint8 buf[SCAN_BUF];
	gint64 pos = offset;
	int found = 0;

	while (found < n && pos < self->file_size) {
		gsize to_read = (gsize) MIN ((gint64) sizeof (buf),
		                             self->file_size - pos);
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

/* Move backward past 'n' newlines from 'offset' (which must be a line start). */
static gint64
skip_lines_backward (NemoPagedViewer *self, gint64 offset, int n)
{
	gint64 pos = offset;
	for (int i = 0; i < n && pos > 0; i++)
		pos = find_line_start (self, pos - 1);
	return pos;
}

/* Estimate average line length from first 64 KB. */
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

/* ================================================================== */
/*                        FONT MEASUREMENT                            */
/* ================================================================== */

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

/* ================================================================== */
/*                           DRAWING                                  */
/* ================================================================== */

static void
draw_text_mode (NemoPagedViewer *self, cairo_t *cr, PangoLayout *layout,
                int width, int height)
{
	guint8 *chunk;
	gssize nread;
	int y = MARGIN_V;
	int lines_drawn = 0;
	gsize pos = 0;
	gint64 line_byte_offset;      /* absolute file offset of current line start */

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
		/* Find end of line */
		gsize line_end = pos;
		while (line_end < (gsize) nread && chunk[line_end] != '\n')
			line_end++;

		gsize line_len = line_end - pos;

		/* Make valid UTF-8 */
		gchar *safe = g_utf8_make_valid ((const gchar *)(chunk + pos),
		                                 (gssize) line_len);

		pango_layout_set_text (layout, safe, -1);

		/* Highlight the search match if it falls on this line */
		if (self->search_active && self->search_match_offset >= 0 &&
		    self->search_match_offset >= line_byte_offset &&
		    self->search_match_offset < line_byte_offset + (gint64) line_len) {
			gsize match_in_line = (gsize)(self->search_match_offset - line_byte_offset);
			gsize match_end = MIN (match_in_line + self->search_needle_len,
			                      strlen (safe));

			PangoAttrList *attrs = pango_attr_list_new ();

			/* Yellow background (#FFD700 in 16-bit: 0xFFFF, 0xD700, 0x0000) */
			PangoAttribute *bg = pango_attr_background_new (0xFFFF, 0xD700, 0x0000);
			bg->start_index = (guint) match_in_line;
			bg->end_index   = (guint) match_end;
			pango_attr_list_insert (attrs, bg);

			/* Black foreground on yellow */
			PangoAttribute *fg_attr = pango_attr_foreground_new (0, 0, 0);
			fg_attr->start_index = (guint) match_in_line;
			fg_attr->end_index   = (guint) match_end;
			pango_attr_list_insert (attrs, fg_attr);

			pango_layout_set_attributes (layout, attrs);
			pango_attr_list_unref (attrs);
		}

		cairo_move_to (cr, MARGIN_H, y);
		pango_cairo_show_layout (cr, layout);

		/* Always reset attributes so the next line starts clean */
		pango_layout_set_attributes (layout, NULL);

		g_free (safe);

		y += self->char_h;
		line_byte_offset += (gint64)(line_len + 1);  /* +1 for the '\n' */
		pos = line_end + 1;    /* skip '\n' */
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

		/* Offset column */
		off += snprintf (line_buf + off, sizeof (line_buf) - off,
		                 "%08lx  ",
		                 (unsigned long)(self->top_offset + pos));

		/* Hex bytes */
		for (int i = 0; i < HEX_BPL; i++) {
			if ((gsize) i < row_len)
				off += snprintf (line_buf + off,
				                 sizeof (line_buf) - off,
				                 "%02x ", data[pos + i]);
			else
				off += snprintf (line_buf + off,
				                 sizeof (line_buf) - off,
				                 "   ");
			if (i == 7)
				line_buf[off++] = ' ';
		}

		off += snprintf (line_buf + off, sizeof (line_buf) - off, " |");

		/* ASCII column */
		for (gsize i = 0; i < row_len; i++) {
			guint8 c = data[pos + i];
			line_buf[off++] = (c >= 0x20 && c <= 0x7e) ? c : '.';
		}
		line_buf[off++] = '|';
		line_buf[off] = '\0';

		pango_layout_set_text (layout, line_buf, off);

		/* Search match highlight: draw a yellow tint behind the matched row */
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

	/* Background */
	style_ctx = gtk_widget_get_style_context (widget);
	gtk_render_background (style_ctx, cr, 0, 0, alloc.width, alloc.height);

	if (self->fd < 0 || self->file_size == 0)
		return TRUE;

	/* Foreground color */
	gtk_style_context_get_color (style_ctx, GTK_STATE_FLAG_NORMAL, &fg);
	gdk_cairo_set_source_rgba (cr, &fg);

	/* Pango layout for text rendering */
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

/* ================================================================== */
/*                         SCROLLBAR                                  */
/* ================================================================== */

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
		gint64 cur_line    = self->top_offset / HEX_BPL;
		gdouble page       = MAX (1, self->vis_lines);

		gtk_adjustment_configure (self->vadjust,
		                          (gdouble) cur_line,   /* value */
		                          0.0,                  /* lower */
		                          (gdouble) total_lines,/* upper */
		                          1.0,                  /* step  */
		                          page,                 /* page  */
		                          page);                /* page_size */
	} else {
		/* Text mode: byte-offset based scrollbar */
		gdouble page = self->vis_lines * self->avg_line_len;
		if (page < 1.0) page = 1.0;

		gtk_adjustment_configure (self->vadjust,
		                          (gdouble) self->top_offset,
		                          0.0,
		                          (gdouble) self->file_size,
		                          self->avg_line_len,
		                          page,
		                          page);
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
		/* Snap to the nearest line start */
		if (offset > 0 && offset < self->file_size)
			self->top_offset = find_line_start (self, offset);
		else
			self->top_offset = offset;
	}

	if (self->top_offset < 0) self->top_offset = 0;
	gtk_widget_queue_draw (self->drawing_area);
}

/* ================================================================== */
/*                     SCROLLING HELPERS                               */
/* ================================================================== */

static void
scroll_by_lines (NemoPagedViewer *self, int delta)
{
	if (self->fd < 0) return;

	if (self->mode == NEMO_VIEWER_MODE_HEX) {
		gint64 cur_line = self->top_offset / HEX_BPL;
		gint64 total    = (self->file_size + HEX_BPL - 1) / HEX_BPL;
		gint64 max_line = MAX (0, total - self->vis_lines);

		cur_line += delta;
		if (cur_line < 0) cur_line = 0;
		if (cur_line > max_line) cur_line = max_line;

		self->top_offset = cur_line * HEX_BPL;
	} else {
		if (delta > 0) {
			self->top_offset = skip_lines_forward (self,
			                                        self->top_offset,
			                                        delta);
		} else if (delta < 0) {
			self->top_offset = skip_lines_backward (self,
			                                         self->top_offset,
			                                         -delta);
		}
	}

	if (self->top_offset < 0) self->top_offset = 0;
	if (self->top_offset > self->file_size) self->top_offset = self->file_size;

	update_scrollbar (self);
	gtk_widget_queue_draw (self->drawing_area);
}

/* ================================================================== */
/*                        EVENT HANDLERS                              */
/* ================================================================== */

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
			self->top_offset = skip_lines_backward (self,
			                                         self->file_size,
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

/* ================================================================== */
/*                        GOBJECT LIFECYCLE                            */
/* ================================================================== */

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

	gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
	                                GTK_ORIENTATION_HORIZONTAL);

	/* Search state */
	self->search_needle = NULL;
	self->search_needle_len = 0;
	self->search_match_offset = -1;
	self->search_active = FALSE;

	/* Monospace font */
	self->font_desc = pango_font_description_from_string ("Monospace 10");

	/* Drawing area */
	self->drawing_area = gtk_drawing_area_new ();
	gtk_widget_set_can_focus (self->drawing_area, TRUE);
	gtk_widget_add_events (self->drawing_area,
	                       GDK_SCROLL_MASK |
	                       GDK_SMOOTH_SCROLL_MASK |
	                       GDK_KEY_PRESS_MASK);

	/* Add the "view" CSS class for theme-appropriate background */
	gtk_style_context_add_class (
		gtk_widget_get_style_context (self->drawing_area),
		GTK_STYLE_CLASS_VIEW);

	gtk_box_pack_start (GTK_BOX (self), self->drawing_area, TRUE, TRUE, 0);

	g_signal_connect (self->drawing_area, "draw",
	                  G_CALLBACK (on_draw), self);
	g_signal_connect (self->drawing_area, "scroll-event",
	                  G_CALLBACK (on_scroll_event), self);
	g_signal_connect (self->drawing_area, "key-press-event",
	                  G_CALLBACK (on_key_press), self);
	g_signal_connect (self->drawing_area, "size-allocate",
	                  G_CALLBACK (on_size_allocate), self);

	/* Scrollbar */
	self->vadjust = gtk_adjustment_new (0, 0, 0, 1, 10, 10);
	self->scrollbar = gtk_scrollbar_new (GTK_ORIENTATION_VERTICAL,
	                                     self->vadjust);
	gtk_box_pack_end (GTK_BOX (self), self->scrollbar, FALSE, FALSE, 0);

	self->vadjust_handler = g_signal_connect (self->vadjust, "value-changed",
	                                          G_CALLBACK (on_vadjust_value_changed),
	                                          self);

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

/* ================================================================== */
/*                          PUBLIC API                                */
/* ================================================================== */

NemoPagedViewer *
nemo_paged_viewer_new (void)
{
	return g_object_new (NEMO_TYPE_PAGED_VIEWER, NULL);
}

gboolean
nemo_paged_viewer_open_file (NemoPagedViewer *self,
                              const gchar     *path,
                              GError         **error)
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

	/* Estimate average line length for text mode scrollbar */
	self->avg_line_len = estimate_avg_line_len (self);

	update_scrollbar (self);
	gtk_widget_queue_draw (self->drawing_area);

	/* Grab keyboard focus */
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
	self->search_match_offset = -1;   /* reset match but keep needle for auto-search */

	update_scrollbar (self);
	gtk_widget_queue_draw (self->drawing_area);
}

void
nemo_paged_viewer_set_mode (NemoPagedViewer *self,
                             NemoViewerMode   mode)
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

/* ================================================================== */
/*                       SEARCH PUBLIC API                            */
/* ================================================================== */

void
nemo_paged_viewer_search_set_needle (NemoPagedViewer *self,
                                      const gchar     *needle)
{
	g_return_if_fail (NEMO_IS_PAGED_VIEWER (self));

	g_free (self->search_needle);
	self->search_needle = NULL;
	self->search_needle_len = 0;
	self->search_match_offset = -1;
	self->search_active = FALSE;

	if (needle != NULL && needle[0] != '\0') {
		self->search_needle       = g_ascii_strdown (needle, -1);
		self->search_needle_len   = strlen (self->search_needle);
		self->search_active       = TRUE;
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

	/* Start just after the current match, or from the current top if no match */
	start_from = (self->search_match_offset >= 0)
		? self->search_match_offset + 1
		: self->top_offset;

	found = search_scan_forward (self, start_from);

	if (found < 0 && start_from > 0) {
		/* Wrap around from the beginning */
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

	before = (self->search_match_offset >= 0)
		? self->search_match_offset
		: self->top_offset;

	found = search_scan_backward (self, before);

	if (found < 0) {
		/* Wrap around from the end */
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
	self->search_needle      = NULL;
	self->search_needle_len  = 0;
	self->search_match_offset = -1;
	self->search_active      = FALSE;

	gtk_widget_queue_draw (self->drawing_area);
}

gboolean
nemo_paged_viewer_search_has_match (NemoPagedViewer *self)
{
	g_return_val_if_fail (NEMO_IS_PAGED_VIEWER (self), FALSE);
	return self->search_active && (self->search_match_offset >= 0);
}
