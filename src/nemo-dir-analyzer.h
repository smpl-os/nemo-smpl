/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * nemo-dir-analyzer.h — Reusable directory-size analysis widget
 *
 * Scans a directory tree and displays the results as a Pareto bar
 * chart + ranked list.  Used by:
 *   • NemoOverview — one analyzer per mounted volume
 *   • NemoQuickPreview — F3 on a directory
 *
 * Copyright (C) 2026 smplOS contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#ifndef NEMO_DIR_ANALYZER_H
#define NEMO_DIR_ANALYZER_H

#include <gtk/gtk.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* ── Data type for scan results ─────────────────────────────────── */

typedef struct {
	char    *name;       /* display name, e.g. "./home/user"   */
	char    *full_path;  /* absolute path for navigation       */
	guint64  size;
} NemoDirEntry;

void    nemo_dir_entry_clear     (NemoDirEntry *entry);
GArray *nemo_dir_entry_array_new (void);
GArray *nemo_dir_entry_array_dup (GArray *src);

/* ── Standalone scan function ───────────────────────────────────── */

#define NEMO_DIR_SCAN_MAX_ENTRIES  10

typedef struct {
	guint64 total_size;
	gboolean incomplete;
	gboolean timed_out;
} NemoDirScanStats;

typedef struct _NemoDirScanNode NemoDirScanNode;
struct _NemoDirScanNode {
	char *name;
	guint64 size;
	guint64 files;
	NemoDirScanNode *parent;
	GPtrArray *children; /* NULL for files; directories own their children. */
};

NemoDirScanNode *nemo_dir_scan_collect_tree (const char *root_path,
                                            GCancellable *cancel,
                                            NemoDirScanStats *stats,
                                            GError **error);
void nemo_dir_scan_node_free (NemoDirScanNode *node);
char *nemo_dir_scan_node_get_path (const NemoDirScanNode *node);

/* A non-positive budget scans to completion (or cancellation). */
GArray *nemo_dir_scan_collect_top (const char   *root_path,
                                   GCancellable *cancel,
                                   int           time_budget_ms,
                                   NemoDirScanStats *stats);

/* ── Colour palette shared by charts ────────────────────────────── */

typedef struct { double r, g, b; } NemoDirColour;

#define NEMO_DIR_N_COLOURS  6

const NemoDirColour *nemo_dir_get_colour     (int idx);
const NemoDirColour *nemo_dir_get_free_colour (void);

/* ── Widget ─────────────────────────────────────────────────────── */

#define NEMO_TYPE_DIR_ANALYZER (nemo_dir_analyzer_get_type ())
G_DECLARE_FINAL_TYPE (NemoDirAnalyzer, nemo_dir_analyzer,
                      NEMO, DIR_ANALYZER, GtkBox)

NemoDirAnalyzer *nemo_dir_analyzer_new (void);

/* Show a "Scanning…" placeholder. */
void nemo_dir_analyzer_show_scanning (NemoDirAnalyzer *self,
                                      const char      *display_name);

/* Feed pre-computed results (e.g. from a cache). */
void nemo_dir_analyzer_set_entries   (NemoDirAnalyzer *self,
                                      GArray          *entries,
                                      const char      *display_name,
                                      int              colour_idx,
                                      const NemoDirScanStats *stats);

/* Scan a path in a background thread and display results. */
void nemo_dir_analyzer_scan_async    (NemoDirAnalyzer *self,
                                      const char      *path,
                                      const char      *display_name,
                                      int              colour_idx,
                                      int              time_budget_ms);

/* Cancel an ongoing async scan. */
void nemo_dir_analyzer_cancel        (NemoDirAnalyzer *self);

/* Clear all content. */
void nemo_dir_analyzer_clear         (NemoDirAnalyzer *self);

/* Retrieve scan results (may be NULL). Caller must NOT free. */
GArray *nemo_dir_analyzer_get_entries (NemoDirAnalyzer *self);
const NemoDirScanStats *nemo_dir_analyzer_get_stats (NemoDirAnalyzer *self);

G_END_DECLS

#endif /* NEMO_DIR_ANALYZER_H */
