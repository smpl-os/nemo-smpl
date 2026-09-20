/* nemo-icon-fallback.h
 *
 * smplOS: provide standard freedesktop fallbacks for the xapp icon names
 * ("xsi-*", "xapp-*") that Nemo asks for throughout its UI.
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

/* Install fallbacks for any xapp icon name the active icon theme does not
 * provide, and keep them in sync when the theme changes. Safe to call more
 * than once; only the first call has an effect.
 *
 * Must be called after gtk_init(). On builds without NEMO_SMPL this is a
 * no-op.
 */
void nemo_icon_fallback_init (void);

G_END_DECLS

#endif /* NEMO_ICON_FALLBACK_H */
