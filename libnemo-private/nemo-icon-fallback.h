/* nemo-icon-fallback.h
 *
 * smplOS: provide embedded flat fallback artwork for Nemo's UI and files.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef NEMO_ICON_FALLBACK_H
#define NEMO_ICON_FALLBACK_H

#include <glib.h>

G_BEGIN_DECLS

#define NEMO_ICON_FALLBACK_RESOURCE_PATH "/org/nemo/default-icons"

/* Register embedded icons as ultimate theme fallbacks. No installed icon
 * package, cache directory, or preference change is required. Real themes
 * retain precedence. Safe to call more than once.
 *
 * Must be called after gtk_init(). On builds without NEMO_SMPL this is a
 * no-op.
 */
void nemo_icon_fallback_init (void);

G_END_DECLS

#endif /* NEMO_ICON_FALLBACK_H */
