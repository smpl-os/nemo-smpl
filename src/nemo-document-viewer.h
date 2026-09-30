/*
 * Native, sandboxed document preview shared by Quick Preview and the sidebar.
 * Copyright (C) 2026 smplOS contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEMO_DOCUMENT_VIEWER_H
#define NEMO_DOCUMENT_VIEWER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NEMO_TYPE_DOCUMENT_VIEWER (nemo_document_viewer_get_type ())
G_DECLARE_FINAL_TYPE (NemoDocumentViewer, nemo_document_viewer, NEMO, DOCUMENT_VIEWER, GtkBox)

NemoDocumentViewer *nemo_document_viewer_new (void);
gboolean nemo_document_viewer_supports_file (GFile *file, const char *mime_type);
void nemo_document_viewer_load_file (NemoDocumentViewer *self, GFile *file, const char *mime_type);
void nemo_document_viewer_close (NemoDocumentViewer *self);
void nemo_document_viewer_scroll_page (NemoDocumentViewer *self, gboolean forward);
void nemo_document_viewer_search_set_needle (NemoDocumentViewer *self, const char *needle);
gboolean nemo_document_viewer_search_find_next (NemoDocumentViewer *self);
gboolean nemo_document_viewer_search_find_prev (NemoDocumentViewer *self);
void nemo_document_viewer_search_clear (NemoDocumentViewer *self);
gboolean nemo_document_viewer_search_has_match (NemoDocumentViewer *self);
gboolean nemo_document_viewer_search_is_pending (NemoDocumentViewer *self);
const GError *nemo_document_viewer_search_get_error (NemoDocumentViewer *self);
unsigned nemo_document_viewer_get_page_count (NemoDocumentViewer *self);
unsigned nemo_document_viewer_get_page (NemoDocumentViewer *self);

/* load-finished (const GError *error): NULL on successful first-page display.
 * search-changed (): inspect the search accessors; errors are borrowed. */

G_END_DECLS
#endif
