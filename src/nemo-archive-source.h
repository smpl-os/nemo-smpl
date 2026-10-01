/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEMO_ARCHIVE_SOURCE_H
#define NEMO_ARCHIVE_SOURCE_H

#include <glib.h>

G_BEGIN_DECLS

/* Call after gst_init_check(). Registers a process-local URI source factory;
 * no plugin file or persistent GStreamer registry entry is installed.
 * Missing giosrc is reported only when opening archive media, not here, so
 * installing this handler cannot disable otherwise working local playback.
 */
gboolean nemo_archive_source_register (GError **error);

G_END_DECLS

#endif
