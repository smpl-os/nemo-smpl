/* Shared, asynchronous metadata and GPS map for sidebar and quick preview.
 * Copyright (C) 2026 The Nemo contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef NEMO_PREVIEW_DETAILS_H
#define NEMO_PREVIEW_DETAILS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NEMO_TYPE_PREVIEW_DETAILS (nemo_preview_details_get_type ())
G_DECLARE_FINAL_TYPE (NemoPreviewDetails, nemo_preview_details, NEMO, PREVIEW_DETAILS, GtkBox)

NemoPreviewDetails *nemo_preview_details_new      (void);
/* The caller owns panel visibility. Hiding/unmapping cancels outstanding work;
 * mapping again reloads the selected file. No image pixels are decoded here. */
void                nemo_preview_details_set_file (NemoPreviewDetails *self, GFile *file);
void                nemo_preview_details_clear    (NemoPreviewDetails *self);

G_END_DECLS

#endif
