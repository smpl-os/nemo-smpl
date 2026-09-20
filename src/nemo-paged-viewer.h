/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * nemo-paged-viewer.h — Custom widget for viewing arbitrarily large files
 *
 * Uses a bounded LRU page cache,
 * so only a few pages (~512 KB) are ever resident regardless of file size.
 * Supports TEXT and HEX display modes.
 *
 * Copyright (C) 2026 smplOS contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#ifndef NEMO_PAGED_VIEWER_H
#define NEMO_PAGED_VIEWER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NEMO_TYPE_PAGED_VIEWER (nemo_paged_viewer_get_type ())
G_DECLARE_FINAL_TYPE (NemoPagedViewer, nemo_paged_viewer, NEMO, PAGED_VIEWER, GtkBox)

typedef enum {
	NEMO_VIEWER_MODE_TEXT,
	NEMO_VIEWER_MODE_HEX,
} NemoViewerMode;

NemoPagedViewer *nemo_paged_viewer_new        (void);
#ifdef NEMO_SMPL
/* Nonblocking, including for native paths. "load-finished" carries a GError
 * (NULL on success); errors are also displayed in the viewer. */
void             nemo_paged_viewer_open_location (NemoPagedViewer *self,
                                                   GFile           *location);
/* Compatibility wrapper: TRUE means queued, not that opening succeeded.
 * Asynchronous errors are delivered by "load-finished", not @error. */
#endif
gboolean         nemo_paged_viewer_open_file  (NemoPagedViewer *self,
                                                const gchar     *path,
                                                GError         **error);
void             nemo_paged_viewer_close_file (NemoPagedViewer *self);
void             nemo_paged_viewer_set_mode   (NemoPagedViewer *self,
                                                NemoViewerMode   mode);

/* Search — case-insensitive ASCII, incremental.
 *
 * set_needle: store the search term; clears any previous match.
 * find_next:  scan forward from after the current match (wraps around).
 * find_prev:  scan backward before the current match (wraps around).
 * clear:      remove needle and match highlight.
 * In NEMO_SMPL builds, find_next/prev return TRUE when a request is accepted,
 * NOT a match result. "search-changed" notifies pending and completed state.
 * Vanilla builds retain synchronous find_next/prev results.
 * has_match:  TRUE if the last completed find succeeded.
 */
void             nemo_paged_viewer_search_set_needle (NemoPagedViewer *self,
                                                       const gchar     *needle);
gboolean         nemo_paged_viewer_search_find_next  (NemoPagedViewer *self);
gboolean         nemo_paged_viewer_search_find_prev  (NemoPagedViewer *self);
void             nemo_paged_viewer_search_clear      (NemoPagedViewer *self);
gboolean         nemo_paged_viewer_search_has_match  (NemoPagedViewer *self);
#ifdef NEMO_SMPL
gboolean         nemo_paged_viewer_search_is_pending (NemoPagedViewer *self);
/* Borrowed error, cleared on the next search, needle change, or close.
 * "search-changed" also notifies failures; a failure is not a no-match result. */
const GError    *nemo_paged_viewer_search_get_error  (NemoPagedViewer *self);
#endif

G_END_DECLS

#endif /* NEMO_PAGED_VIEWER_H */
