/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * nemo-quick-preview.h — Lightweight in-process file viewer (à la Double Commander F3)
 *
 * Copyright (C) 2026 smplOS contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#ifndef NEMO_QUICK_PREVIEW_H
#define NEMO_QUICK_PREVIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NEMO_TYPE_QUICK_PREVIEW (nemo_quick_preview_get_type ())
G_DECLARE_FINAL_TYPE (NemoQuickPreview, nemo_quick_preview, NEMO, QUICK_PREVIEW, GtkWindow)

NemoQuickPreview *nemo_quick_preview_get_instance (void);
#if defined(NEMO_SMPL) && defined(HAVE_GSTREAMER)
NemoQuickPreview *nemo_quick_preview_new_player (void);
#endif
void              nemo_quick_preview_show_file    (NemoQuickPreview *self,
                                                   GFile            *file,
                                                   GtkWindow        *parent);
void              nemo_quick_preview_dismiss      (NemoQuickPreview *self);

G_END_DECLS

#endif /* NEMO_QUICK_PREVIEW_H */
