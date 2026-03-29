/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * nemo-dir-analyzer.c — Reusable directory-size analysis widget
 *
 * Copyright (C) 2026 smplOS contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#include <config.h>
#include "nemo-dir-analyzer.h"

#include <glib/gi18n.h>
#include <math.h>
#include <sched.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

/* Yield to the main thread every N directory entries so the scanner
 * doesn't saturate the I/O queue and starve the UI.               */
#define SCAN_YIELD_INTERVAL 64

#include "nemo-window-slot.h"
#include "nemo-window.h"

/* ── Bar-chart layout constants ────────────────────────────────── */
#define VBAR_W              28
#define VBAR_GAP             6
#define VBAR_MAX_H         130
#define VBAR_LABEL_H        65
#define VBAR_SIZE_H         18
#define VBAR_TOTAL_H       (VBAR_SIZE_H + VBAR_MAX_H + VBAR_LABEL_H)
#define VBAR_MAX_BARS       10

/* ── Colour palette ────────────────────────────────────────────── */

static const NemoDirColour palette[] = {
	{ 0.33, 0.63, 0.91 },  /* blue   */
	{ 0.42, 0.78, 0.44 },  /* green  */
	{ 0.94, 0.60, 0.22 },  /* orange */
	{ 0.84, 0.36, 0.36 },  /* red    */
	{ 0.62, 0.42, 0.82 },  /* purple */
	{ 0.24, 0.79, 0.76 },  /* teal   */
};

static const NemoDirColour free_colour = { 0.75, 0.75, 0.75 };

const NemoDirColour *
nemo_dir_get_colour (int idx)
{
	return &palette[idx % NEMO_DIR_N_COLOURS];
}

const NemoDirColour *
nemo_dir_get_free_colour (void)
{
	return &free_colour;
}

/* ── NemoDirEntry helpers ──────────────────────────────────────── */

void
nemo_dir_entry_clear (NemoDirEntry *e)
{
	g_free (e->name);
	g_free (e->full_path);
	e->name = NULL;
	e->full_path = NULL;
}

GArray *
nemo_dir_entry_array_new (void)
{
	GArray *a = g_array_new (FALSE, TRUE, sizeof (NemoDirEntry));
	g_array_set_clear_func (a, (GDestroyNotify) nemo_dir_entry_clear);
	return a;
}

GArray *
nemo_dir_entry_array_dup (GArray *src)
{
	GArray *dst;
	guint i;

	if (src == NULL)
		return NULL;

	dst = nemo_dir_entry_array_new ();

	for (i = 0; i < src->len; i++) {
		NemoDirEntry *s = &g_array_index (src, NemoDirEntry, i);
		NemoDirEntry d = { 0 };
		d.name      = g_strdup (s->name);
		d.full_path = g_strdup (s->full_path);
		d.size      = s->size;
		g_array_append_val (dst, d);
	}

	return dst;
}

/* ── Filesystem scan ───────────────────────────────────────────── */

static void
consider_top (GArray     *top,
              const char *root_path,
              const char *full_path,
              guint64     size)
{
	NemoDirEntry candidate = { 0 };
	guint i;
	const char *rel;

	if (size == 0 || top == NULL || full_path == NULL)
		return;

	candidate.full_path = g_strdup (full_path);
	if (g_strcmp0 (full_path, root_path) == 0) {
		candidate.name = g_strdup (".");
	} else if (g_str_has_prefix (full_path, root_path)) {
		rel = full_path + strlen (root_path);
		while (*rel == '/')
			rel++;
		candidate.name = g_strdup_printf ("./%s", rel);
	} else {
		candidate.name = g_path_get_basename (full_path);
	}
	candidate.size = size;

	/* Find insertion point (sorted descending) */
	for (i = 0; i < top->len; i++) {
		NemoDirEntry *e = &g_array_index (top, NemoDirEntry, i);
		if (candidate.size > e->size)
			break;
	}

	if (i >= NEMO_DIR_SCAN_MAX_ENTRIES) {
		nemo_dir_entry_clear (&candidate);
		return;
	}

	if (top->len < NEMO_DIR_SCAN_MAX_ENTRIES) {
		g_array_append_val (top, candidate);
	} else {
		NemoDirEntry *last = &g_array_index (top, NemoDirEntry,
		                                     top->len - 1);
		nemo_dir_entry_clear (last);
		*last = candidate;
	}

	/* Bubble into position */
	for (i = top->len - 1; i > 0; i--) {
		NemoDirEntry *a = &g_array_index (top, NemoDirEntry, i - 1);
		NemoDirEntry *b = &g_array_index (top, NemoDirEntry, i);
		if (a->size >= b->size)
			break;
		NemoDirEntry tmp = *a;
		*a = *b;
		*b = tmp;
	}
}

static guint64
scan_recursive (const char   *path,
                const char   *root_path,
                dev_t         dev,
                GCancellable *cancel,
                GArray       *top,
                gboolean      include_self,
                gint64        deadline_us,
                gboolean     *timed_out)
{
	DIR *dp;
	struct dirent *entry;
	struct stat pst;
	guint64 total = 0;

	if (deadline_us > 0 && g_get_monotonic_time () >= deadline_us) {
		if (timed_out) *timed_out = TRUE;
		return 0;
	}

	if (g_cancellable_is_cancelled (cancel))
		return 0;

	if (lstat (path, &pst) != 0)
		return 0;

	if (S_ISREG (pst.st_mode)) {
		total = (guint64) pst.st_size;
		consider_top (top, root_path, path, total);
		return total;
	}

	if (!S_ISDIR (pst.st_mode) || pst.st_dev != dev)
		return 0;

	dp = opendir (path);
	if (dp == NULL)
		return 0;

	guint yield_counter = 0;

	while ((entry = readdir (dp)) != NULL) {
		struct stat st;
		char *child;

		if (deadline_us > 0 && g_get_monotonic_time () >= deadline_us) {
			if (timed_out) *timed_out = TRUE;
			break;
		}

		if (g_cancellable_is_cancelled (cancel))
			break;

		/* Periodically yield so the UI's I/O isn't starved */
		if (++yield_counter % SCAN_YIELD_INTERVAL == 0)
			sched_yield ();

		if (g_strcmp0 (entry->d_name, ".") == 0 ||
		    g_strcmp0 (entry->d_name, "..") == 0)
			continue;

		child = g_build_filename (path, entry->d_name, NULL);

		if (lstat (child, &st) == 0) {
			if (S_ISREG (st.st_mode)) {
				total += (guint64) st.st_size;
				consider_top (top, root_path, child,
				              (guint64) st.st_size);
			} else if (S_ISDIR (st.st_mode) && st.st_dev == dev) {
				total += scan_recursive (child, root_path, dev,
				                         cancel, top, TRUE,
				                         deadline_us, timed_out);
			}
		}

		g_free (child);
	}

	closedir (dp);

	if (!include_self && top->len == 0 && total > 0)
		consider_top (top, root_path, path, total);

	if (include_self)
		consider_top (top, root_path, path, total);

	return total;
}

GArray *
nemo_dir_scan_collect_top (const char   *root_path,
                            GCancellable *cancel,
                            int           time_budget_ms,
                            guint64      *total_size_out)
{
	struct stat st;
	GArray *top;
	guint64 total;
	gint64  deadline_us;
	gboolean timed_out = FALSE;

	if (root_path == NULL)
		return NULL;

	if (stat (root_path, &st) != 0)
		return NULL;

	top = nemo_dir_entry_array_new ();
	deadline_us = (time_budget_ms > 0)
	              ? g_get_monotonic_time () + (gint64) time_budget_ms * 1000
	              : 0;

	total = scan_recursive (root_path, root_path, st.st_dev,
	                        cancel, top, FALSE,
	                        deadline_us, &timed_out);

	if (total_size_out != NULL)
		*total_size_out = total;

	return top;
}

/* ── Pareto chart drawing ──────────────────────────────────────── */

typedef struct {
	int     colour_idx;
	GArray *entries;
} ChartDrawData;

static void
chart_draw_data_free (gpointer data)
{
	ChartDrawData *cd = data;
	if (cd == NULL) return;
	if (cd->entries)
		g_array_unref (cd->entries);
	g_free (cd);
}

static gboolean
chart_draw_cb (GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
	ChartDrawData *cd = user_data;
	const NemoDirColour *colour = nemo_dir_get_colour (cd->colour_idx);
	guint count = MIN (cd->entries->len, (guint) VBAR_MAX_BARS);
	guint i;

	if (count == 0) return FALSE;

	guint64 max_size = g_array_index (cd->entries, NemoDirEntry, 0).size;
	if (max_size == 0) return FALSE;

	double baseline_y = VBAR_SIZE_H + VBAR_MAX_H;

	for (i = 0; i < count; i++) {
		NemoDirEntry *e = &g_array_index (cd->entries, NemoDirEntry, i);
		double frac  = (double) e->size / (double) max_size;
		double bar_h = frac * VBAR_MAX_H;
		double x     = i * (VBAR_W + VBAR_GAP);

		/* Bar */
		cairo_set_source_rgba (cr, colour->r, colour->g, colour->b, 0.70);
		cairo_rectangle (cr, x, baseline_y - bar_h, VBAR_W, bar_h);
		cairo_fill (cr);

		/* Name above bar */
		{
			PangoLayout *lay = pango_cairo_create_layout (cr);
			PangoFontDescription *fd =
				pango_font_description_from_string ("Sans 8");
			int tw, th;

			pango_layout_set_font_description (lay, fd);
			pango_font_description_free (fd);
			pango_layout_set_text (lay, e->name, -1);
			pango_layout_set_width (lay, VBAR_W * PANGO_SCALE);
			pango_layout_set_ellipsize (lay, PANGO_ELLIPSIZE_END);
			pango_layout_get_pixel_size (lay, &tw, &th);

			cairo_set_source_rgba (cr, 0.65, 0.65, 0.65, 1.0);
			cairo_move_to (cr,
			               x + (VBAR_W - tw) / 2.0,
			               baseline_y - bar_h - th - 2);
			pango_cairo_show_layout (cr, lay);
			g_object_unref (lay);
		}

		/* Size below bar */
		{
			char *sz = g_format_size (e->size);
			PangoLayout *lay = pango_cairo_create_layout (cr);
			PangoFontDescription *fd =
				pango_font_description_from_string ("Sans 7");
			int tw, th;

			pango_layout_set_font_description (lay, fd);
			pango_font_description_free (fd);
			pango_layout_set_text (lay, sz, -1);
			pango_layout_get_pixel_size (lay, &tw, &th);

			cairo_set_source_rgba (cr, 0.65, 0.65, 0.65, 1.0);
			cairo_move_to (cr,
			               x + (VBAR_W - tw) / 2.0,
			               baseline_y + 2);
			pango_cairo_show_layout (cr, lay);
			g_object_unref (lay);
			g_free (sz);
		}
	}

	return FALSE;
}

/* ── Click handler: double-click bar → navigate ────────────────── */

static gboolean
chart_button_press_cb (GtkWidget      *widget,
                       GdkEventButton *event,
                       gpointer        user_data)
{
	ChartDrawData *cd = user_data;
	guint count;
	int bar_idx;
	NemoDirEntry *e;
	GFile *location;
	GtkWidget *toplevel;

	if (event->type != GDK_2BUTTON_PRESS || event->button != 1)
		return FALSE;

	count   = MIN (cd->entries->len, (guint) VBAR_MAX_BARS);
	bar_idx = (int) (event->x) / (VBAR_W + VBAR_GAP);

	if (bar_idx < 0 || bar_idx >= (int) count)
		return FALSE;

	e = &g_array_index (cd->entries, NemoDirEntry, bar_idx);
	if (e->full_path == NULL)
		return FALSE;

	location = g_file_new_for_path (e->full_path);
	toplevel = gtk_widget_get_toplevel (widget);

	if (NEMO_IS_WINDOW (toplevel)) {
		NemoWindowSlot *slot =
			nemo_window_get_active_slot (NEMO_WINDOW (toplevel));
		if (slot != NULL)
			nemo_window_slot_open_location (slot, location, 0);
	}

	g_object_unref (location);
	return TRUE;
}

/* ── Tooltip on bar ────────────────────────────────────────────── */

static gboolean
chart_query_tooltip_cb (GtkWidget  *widget,
                        gint        x,
                        gint        y,
                        gboolean    keyboard_mode,
                        GtkTooltip *tooltip,
                        gpointer    user_data)
{
	ChartDrawData *cd = user_data;
	guint count;
	int bar_idx;
	NemoDirEntry *e;
	char *sz, *tip;

	(void) widget; (void) y; (void) keyboard_mode;

	count   = MIN (cd->entries->len, (guint) VBAR_MAX_BARS);
	bar_idx = x / (VBAR_W + VBAR_GAP);

	if (bar_idx < 0 || bar_idx >= (int) count)
		return FALSE;

	e  = &g_array_index (cd->entries, NemoDirEntry, bar_idx);
	sz = g_format_size (e->size);
	tip = g_strdup_printf ("%s\n%s",
	                       e->full_path != NULL ? e->full_path : e->name,
	                       sz);

	gtk_tooltip_set_text (tooltip, tip);
	g_free (tip);
	g_free (sz);
	return TRUE;
}

/* ── Pointer cursor on chart ───────────────────────────────────── */

static void
chart_realize_cb (GtkWidget *widget, gpointer data)
{
	GdkCursor *hand;
	(void) data;

	hand = gdk_cursor_new_from_name (gtk_widget_get_display (widget),
	                                 "pointer");
	if (hand != NULL) {
		gdk_window_set_cursor (gtk_widget_get_window (widget), hand);
		g_object_unref (hand);
	}
}

/* ── Create one Pareto bar chart widget ────────────────────────── */

static GtkWidget *
create_pareto_chart (GArray *entries, int colour_idx)
{
	GtkWidget *draw_area;
	ChartDrawData *cd;
	guint count = MIN (entries->len, (guint) VBAR_MAX_BARS);
	int chart_w = count * (VBAR_W + VBAR_GAP);

	draw_area = gtk_drawing_area_new ();
	gtk_widget_set_size_request (draw_area, chart_w, VBAR_TOTAL_H);
	gtk_widget_set_halign (draw_area, GTK_ALIGN_START);
	gtk_widget_set_margin_bottom (draw_area, 8);
	gtk_widget_set_hexpand (draw_area, TRUE);
	gtk_widget_set_has_tooltip (draw_area, TRUE);
	gtk_widget_add_events (draw_area, GDK_BUTTON_PRESS_MASK);

	cd = g_new0 (ChartDrawData, 1);
	cd->colour_idx = colour_idx;
	cd->entries    = g_array_ref (entries);

	g_object_set_data_full (G_OBJECT (draw_area), "chart-data",
	                        cd, chart_draw_data_free);
	g_signal_connect (draw_area, "draw",
	                  G_CALLBACK (chart_draw_cb), cd);
	g_signal_connect (draw_area, "button-press-event",
	                  G_CALLBACK (chart_button_press_cb), cd);
	g_signal_connect (draw_area, "query-tooltip",
	                  G_CALLBACK (chart_query_tooltip_cb), cd);
	g_signal_connect (draw_area, "realize",
	                  G_CALLBACK (chart_realize_cb), NULL);

	return draw_area;
}

/* ── Click handler for ranked list rows ────────────────────────── */

static gboolean
list_row_button_press_cb (GtkWidget      *widget,
                          GdkEventButton *event,
                          gpointer        user_data)
{
	const char *full_path;
	GFile *location;
	GtkWidget *toplevel;

	(void) user_data;

	if (event->button != 1 ||
	    !(event->type == GDK_BUTTON_PRESS ||
	      event->type == GDK_2BUTTON_PRESS))
		return FALSE;

	full_path = g_object_get_data (G_OBJECT (widget), "full-path");
	if (full_path == NULL)
		return FALSE;

	location = g_file_new_for_path (full_path);
	toplevel = gtk_widget_get_toplevel (widget);
	if (NEMO_IS_WINDOW (toplevel)) {
		NemoWindowSlot *slot =
			nemo_window_get_active_slot (NEMO_WINDOW (toplevel));
		if (slot != NULL)
			nemo_window_slot_open_location (slot, location, 0);
	}
	g_object_unref (location);
	return TRUE;
}

/* ── Build the ranked list grid ────────────────────────────────── */

static GtkWidget *
create_ranked_list (GArray *entries)
{
	GtkWidget *list_grid;
	guint i, count;

	count = MIN (entries->len, (guint) NEMO_DIR_SCAN_MAX_ENTRIES);

	list_grid = gtk_grid_new ();
	gtk_widget_set_margin_start (list_grid, 8);
	gtk_widget_set_margin_end (list_grid, 8);
	gtk_widget_set_margin_bottom (list_grid, 16);
	gtk_grid_set_row_spacing (GTK_GRID (list_grid), 2);
	gtk_grid_set_column_spacing (GTK_GRID (list_grid), 10);

	for (i = 0; i < count; i++) {
		NemoDirEntry *e = &g_array_index (entries, NemoDirEntry, i);
		GtkWidget *size_lbl, *row_click, *path_lbl;
		char *sz = g_format_size (e->size);

		size_lbl = gtk_label_new (sz);
		gtk_widget_set_halign (size_lbl, GTK_ALIGN_START);
		gtk_widget_set_opacity (size_lbl, 0.8);
		gtk_grid_attach (GTK_GRID (list_grid), size_lbl,
		                 0, (gint) i, 1, 1);
		gtk_widget_show (size_lbl);

		row_click = gtk_event_box_new ();
		gtk_widget_set_halign (row_click, GTK_ALIGN_FILL);
		gtk_widget_set_hexpand (row_click, TRUE);
		gtk_event_box_set_visible_window (
			GTK_EVENT_BOX (row_click), FALSE);
		gtk_widget_add_events (row_click, GDK_BUTTON_PRESS_MASK);
		g_object_set_data_full (G_OBJECT (row_click), "full-path",
		                        g_strdup (e->full_path ? e->full_path : ""),
		                        g_free);
		g_signal_connect (row_click, "button-press-event",
		                  G_CALLBACK (list_row_button_press_cb), NULL);
		g_signal_connect (row_click, "realize",
		                  G_CALLBACK (chart_realize_cb), NULL);

		path_lbl = gtk_label_new (e->name);
		gtk_label_set_xalign (GTK_LABEL (path_lbl), 0.0);
		gtk_label_set_ellipsize (GTK_LABEL (path_lbl),
		                         PANGO_ELLIPSIZE_MIDDLE);
		gtk_widget_set_halign (path_lbl, GTK_ALIGN_START);
		gtk_widget_set_hexpand (path_lbl, TRUE);
		gtk_widget_set_tooltip_text (path_lbl,
		                             e->full_path ? e->full_path : e->name);
		gtk_container_add (GTK_CONTAINER (row_click), path_lbl);
		gtk_widget_show (path_lbl);
		gtk_widget_show (row_click);

		gtk_grid_attach (GTK_GRID (list_grid), row_click,
		                 1, (gint) i, 1, 1);
		g_free (sz);
	}

	return list_grid;
}

/* ── NemoDirAnalyzer widget ────────────────────────────────────── */

struct _NemoDirAnalyzer {
	GtkBox         parent;

	GtkWidget     *status_label;   /* "Scanning…" or "Total: …"  */
	GtkWidget     *content_box;    /* hbox: chart + list          */

	GArray        *entries;        /* NemoDirEntry                */
	guint64        total_size;
	char          *display_name;
	int            colour_idx;

	GCancellable  *cancel;
	gboolean       scanning;
};

G_DEFINE_TYPE (NemoDirAnalyzer, nemo_dir_analyzer, GTK_TYPE_BOX)
enum { SIGNAL_SCAN_FINISHED, N_SIGNALS };
static guint signals[N_SIGNALS];
/* ── Rebuild content from entries ──────────────────────────────── */

static void
rebuild_content (NemoDirAnalyzer *self)
{
	GtkWidget *chart, *list;
	char *total_text;

	/* Clear old content */
	if (self->content_box != NULL) {
		gtk_widget_destroy (self->content_box);
		self->content_box = NULL;
	}

	if (self->entries == NULL || self->entries->len == 0) {
		gtk_label_set_text (GTK_LABEL (self->status_label),
		                    _("No files found."));
		gtk_widget_show (self->status_label);
		return;
	}

	/* Status line: total size */
	if (self->total_size > 0) {
		char *sz = g_format_size (self->total_size);
		if (self->display_name != NULL)
			total_text = g_strdup_printf (
				_("Total: %s — Top %u entries"),
				sz, MIN (self->entries->len,
				         (guint) NEMO_DIR_SCAN_MAX_ENTRIES));
		else
			total_text = g_strdup_printf (
				_("Total: %s — %u entries"),
				sz, MIN (self->entries->len,
				         (guint) NEMO_DIR_SCAN_MAX_ENTRIES));
		gtk_label_set_text (GTK_LABEL (self->status_label),
		                    total_text);
		g_free (total_text);
		g_free (sz);
		gtk_widget_show (self->status_label);
	} else {
		gtk_widget_hide (self->status_label);
	}

	/* Chart + list side by side */
	self->content_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 16);
	gtk_widget_set_margin_start (self->content_box, 8);
	gtk_widget_set_margin_end (self->content_box, 8);
	gtk_widget_set_margin_bottom (self->content_box, 16);

	chart = create_pareto_chart (self->entries, self->colour_idx);
	gtk_box_pack_start (GTK_BOX (self->content_box),
	                    chart, FALSE, FALSE, 0);
	gtk_widget_show (chart);

	list = create_ranked_list (self->entries);
	gtk_box_pack_start (GTK_BOX (self->content_box),
	                    list, TRUE, TRUE, 0);
	gtk_widget_show (list);

	gtk_box_pack_start (GTK_BOX (self), self->content_box,
	                    FALSE, FALSE, 0);
	gtk_widget_show (self->content_box);
}

/* ── Async scan: idle callback (delivers results to main thread) ── */

typedef struct {
	NemoDirAnalyzer *self;
	GCancellable    *cancel;
	GArray          *entries;
	guint64          total_size;
} ScanIdleData;

static gboolean
scan_idle_cb (gpointer data)
{
	ScanIdleData *sid = data;

	if (g_cancellable_is_cancelled (sid->cancel))
		goto out;

	if (sid->entries != NULL && sid->entries->len > 0) {
		g_clear_pointer (&sid->self->entries, g_array_unref);
		sid->self->entries    = g_array_ref (sid->entries);
		sid->self->total_size = sid->total_size;
		sid->self->scanning   = FALSE;

		rebuild_content (sid->self);
	} else {
		gtk_label_set_text (
			GTK_LABEL (sid->self->status_label),
			_("No files found."));
		gtk_widget_show (sid->self->status_label);
		sid->self->scanning = FALSE;
	}

	g_signal_emit (sid->self, signals[SIGNAL_SCAN_FINISHED], 0,
	               sid->total_size);

out:
	g_clear_object (&sid->cancel);
	if (sid->entries)
		g_array_unref (sid->entries);
	g_object_unref (sid->self);
	g_free (sid);
	return G_SOURCE_REMOVE;
}

/* ── Async scan: thread data ───────────────────────────────────── */

typedef struct {
	NemoDirAnalyzer *self;
	GCancellable    *cancel;
	char            *path;
	int              time_budget_ms;
} ScanThreadData;

static gpointer
scan_thread_func (gpointer data)
{
	ScanThreadData *td = data;
	GArray     *entries;
	guint64     total = 0;
	ScanIdleData *sid;

	entries = nemo_dir_scan_collect_top (td->path, td->cancel,
	                                     td->time_budget_ms, &total);

	sid = g_new0 (ScanIdleData, 1);
	sid->self       = g_object_ref (td->self);
	sid->cancel     = g_object_ref (td->cancel);
	sid->entries    = entries;   /* takes ownership */
	sid->total_size = total;

	g_idle_add (scan_idle_cb, sid);

	g_clear_object (&td->cancel);
	g_object_unref (td->self);
	g_free (td->path);
	g_free (td);
	return NULL;
}

/* ── GObject plumbing ──────────────────────────────────────────── */

static void
nemo_dir_analyzer_dispose (GObject *object)
{
	NemoDirAnalyzer *self = NEMO_DIR_ANALYZER (object);

	nemo_dir_analyzer_cancel (self);
	g_clear_pointer (&self->entries, g_array_unref);
	g_clear_pointer (&self->display_name, g_free);

	G_OBJECT_CLASS (nemo_dir_analyzer_parent_class)->dispose (object);
}

static void
nemo_dir_analyzer_class_init (NemoDirAnalyzerClass *klass)
{
	GObjectClass *oclass = G_OBJECT_CLASS (klass);
	oclass->dispose = nemo_dir_analyzer_dispose;

	signals[SIGNAL_SCAN_FINISHED] =
		g_signal_new ("scan-finished",
		              NEMO_TYPE_DIR_ANALYZER,
		              G_SIGNAL_RUN_LAST,
		              0, NULL, NULL, NULL,
		              G_TYPE_NONE, 1, G_TYPE_UINT64);
}

static void
nemo_dir_analyzer_init (NemoDirAnalyzer *self)
{
	self->colour_idx = 0;
	self->scanning   = FALSE;
	self->total_size = 0;

	gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
	                                GTK_ORIENTATION_VERTICAL);

	self->status_label = gtk_label_new ("");
	gtk_widget_set_halign (self->status_label, GTK_ALIGN_START);
	gtk_widget_set_margin_start (self->status_label, 8);
	gtk_widget_set_margin_top (self->status_label, 4);
	gtk_widget_set_opacity (self->status_label, 0.6);
	gtk_box_pack_start (GTK_BOX (self), self->status_label,
	                    FALSE, FALSE, 0);
	gtk_widget_show (self->status_label);
}

/* ── Public API ────────────────────────────────────────────────── */

NemoDirAnalyzer *
nemo_dir_analyzer_new (void)
{
	return g_object_new (NEMO_TYPE_DIR_ANALYZER, NULL);
}

void
nemo_dir_analyzer_show_scanning (NemoDirAnalyzer *self,
                                  const char      *display_name)
{
	g_return_if_fail (NEMO_IS_DIR_ANALYZER (self));

	g_free (self->display_name);
	self->display_name = g_strdup (display_name);

	if (self->content_box != NULL) {
		gtk_widget_destroy (self->content_box);
		self->content_box = NULL;
	}

	gtk_label_set_text (GTK_LABEL (self->status_label),
	                    _("Scanning largest directories…"));
	gtk_widget_show (self->status_label);
}

void
nemo_dir_analyzer_set_entries (NemoDirAnalyzer *self,
                                GArray          *entries,
                                const char      *display_name,
                                int              colour_idx)
{
	g_return_if_fail (NEMO_IS_DIR_ANALYZER (self));

	g_free (self->display_name);
	self->display_name = g_strdup (display_name);
	self->colour_idx   = colour_idx;

	g_clear_pointer (&self->entries, g_array_unref);
	if (entries != NULL)
		self->entries = nemo_dir_entry_array_dup (entries);

	/* Compute total from entries if not already set */
	if (self->entries != NULL && self->entries->len > 0) {
		guint64 sum = 0;
		guint i;
		for (i = 0; i < self->entries->len; i++)
			sum += g_array_index (self->entries,
			                     NemoDirEntry, i).size;
		if (sum > self->total_size)
			self->total_size = sum;
	}

	rebuild_content (self);
}

void
nemo_dir_analyzer_scan_async (NemoDirAnalyzer *self,
                               const char      *path,
                               const char      *display_name,
                               int              colour_idx,
                               int              time_budget_ms)
{
	ScanThreadData *td;

	g_return_if_fail (NEMO_IS_DIR_ANALYZER (self));
	g_return_if_fail (path != NULL);

	nemo_dir_analyzer_cancel (self);

	g_free (self->display_name);
	self->display_name = g_strdup (display_name);
	self->colour_idx   = colour_idx;
	self->scanning     = TRUE;

	nemo_dir_analyzer_show_scanning (self, display_name);

	self->cancel = g_cancellable_new ();

	td = g_new0 (ScanThreadData, 1);
	td->self           = g_object_ref (self);
	td->cancel         = g_object_ref (self->cancel);
	td->path           = g_strdup (path);
	td->time_budget_ms = time_budget_ms;

	g_thread_unref (g_thread_new ("dir-analyzer-scan",
	                              scan_thread_func, td));
}

void
nemo_dir_analyzer_cancel (NemoDirAnalyzer *self)
{
	g_return_if_fail (NEMO_IS_DIR_ANALYZER (self));

	if (self->cancel != NULL) {
		g_cancellable_cancel (self->cancel);
		g_clear_object (&self->cancel);
	}
	self->scanning = FALSE;
}

void
nemo_dir_analyzer_clear (NemoDirAnalyzer *self)
{
	g_return_if_fail (NEMO_IS_DIR_ANALYZER (self));

	nemo_dir_analyzer_cancel (self);
	g_clear_pointer (&self->entries, g_array_unref);
	self->total_size = 0;

	if (self->content_box != NULL) {
		gtk_widget_destroy (self->content_box);
		self->content_box = NULL;
	}

	gtk_label_set_text (GTK_LABEL (self->status_label), "");
	gtk_widget_hide (self->status_label);
}

GArray *
nemo_dir_analyzer_get_entries (NemoDirAnalyzer *self)
{
	g_return_val_if_fail (NEMO_IS_DIR_ANALYZER (self), NULL);
	return self->entries;
}
