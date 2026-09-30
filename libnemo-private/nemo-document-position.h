/*
 * Persistent reading positions for document previews.
 * Copyright (C) 2026 smplOS contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NEMO_DOCUMENT_POSITION_H
#define NEMO_DOCUMENT_POSITION_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Reading positions are keyed by URI and invalidated when a local file's size
 * or modification time changes, so a rewritten document never reopens on a
 * page that no longer holds the same text. */
gboolean nemo_document_position_is_enabled (void);
gboolean nemo_document_position_lookup (GFile *file, guint *page, gdouble *offset);
void nemo_document_position_store (GFile *file, guint page, gdouble offset, guint page_count);
void nemo_document_position_forget (GFile *file);
void nemo_document_position_flush (void);

G_END_DECLS
#endif
