/* nemo-icon-fallback.c
 *
 * Copyright (C) 2026 smplOS
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#include <config.h>
#include "nemo-icon-fallback.h"

#ifdef NEMO_SMPL
#include <gtk/gtk.h>
#endif

void
nemo_icon_fallback_init (void)
{
#ifdef NEMO_SMPL
    static gboolean initialized;
    GtkIconTheme *theme;

    if (initialized) {
        return;
    }

    theme = gtk_icon_theme_get_default ();
    g_return_if_fail (theme != NULL);

    /* Root-level resource icons are ultimate fallbacks, even when hicolor's
     * index is absent or incomplete. Real theme icons retain precedence. */
    gtk_icon_theme_add_resource_path (theme, NEMO_ICON_FALLBACK_RESOURCE_PATH);
    initialized = TRUE;
#endif
}
