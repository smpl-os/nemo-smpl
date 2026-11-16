/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NemoFlightRenderer NemoFlightRenderer;

typedef enum {
	NEMO_FLIGHT_SPACE,
	NEMO_FLIGHT_TERRAIN
} NemoFlightScene;

/* All calls require the owning worker's current OpenGL 3.3 core context.
 * The renderer targets framebuffer zero and sets its complete draw state.
 * Callers switching engines must establish their own GL state afterwards.
 * The default framebuffer needs at least 16 depth bits (24 recommended).
 * Seed identifies a world; time is finite, non-negative animation seconds.
 * Energy is clamped to [0,1] and never displaces the terrain camera.
 * Any positive resolution is accepted: widths <=480 and <=320 automatically
 * reduce terrain mesh density; <=480 also reduces crown tessellation.
 * Callers may upscale reduced-resolution readback without changing the route.
 *
 * Terrain travels at 95 world units/s through a periodic 5,898,240-unit world.
 * Camera coordinates are rebased every 1,024 units; time is reduced before
 * multiplication, including for large finite elapsed times. Heading follows
 * the route, roll is bounded to 0.035 radians, and pitch to atan(75/550).
 * Trees use a bounded, seed-stable placement cache, refreshed at cell changes.
 * No textures, external assets, or per-frame CPU allocations are used. */
NemoFlightRenderer *nemo_flight_renderer_new (GError **error);
gboolean nemo_flight_renderer_render (NemoFlightRenderer *self,
                                     NemoFlightScene scene,
                                     guint32 seed,
                                     double time,
                                     double energy,
                                     int width,
                                     int height,
                                     GError **error);
void nemo_flight_renderer_free (NemoFlightRenderer *self);

G_END_DECLS
