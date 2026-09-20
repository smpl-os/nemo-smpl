/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 *  Nemo
 *
 *  Copyright (C) 2025 The Nemo contributors
 *
 *  Nemo is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  Nemo is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 51 Franklin Street, Suite 500,
 *  MA 02110-1335, USA.
 */

/* nemo-preview-pane.c - embedded file-preview panel for Nemo */

#include <config.h>
#include "nemo-preview-pane.h"
#include "nemo-preview-utils.h"
#include "nemo-image-viewer.h"
#include "nemo-paged-viewer.h"

#include <math.h>
#include <string.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <libnemo-private/nemo-file.h>
#include <libnemo-private/nemo-global-preferences.h>

#ifdef HAVE_EXIF
#include <libexif/exif-data.h>
#ifdef NEMO_SMPL
#include <libexif/exif-loader.h>
#endif
#include <libexif/exif-utils.h>
#endif

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/app/gstappsink.h>
#endif

#define GPS_MAP_SIZE          150

struct _NemoPreviewPane
{
	GtkBox		 parent;

	GtkWidget	*vpaned;
	GtkWidget	*stack;

	/* Image page (shared widget) */
	NemoImageViewer	*image_viewer;

	/* Text page (shared paged viewer — handles files of any size) */
	NemoPagedViewer	*paged_viewer;

#ifdef HAVE_GSTREAMER
	/* Video page */
	GtkWidget	*video_page_box;
	GtkWidget	*video_area;
	GtkWidget	*audio_icon;
	GtkWidget	*play_btn;
	GtkWidget	*mute_btn;
	GtkWidget	*seek_scale;
	GstElement	*pipeline;
	guint		 bus_watch_id;
	guint		 seek_update_id;
#ifdef NEMO_SMPL
	NemoPreviewMediaFrames *video_frames;
	gboolean	 video_playing;
	gboolean	 video_ready;
#endif
	cairo_surface_t	*frame_surface;
	GMutex		 frame_mutex;
	gint		 video_width;
	gint		 video_height;
	gboolean	 video_muted;
	gboolean	 seek_lock;
#endif

	/* Info page (icon only, for non-previewable files) */
	GtkWidget	*info_icon;
	GtkWidget	*info_error_label;

	/* Empty page */
	GtkWidget	*empty_label;

	/* Details panel (shown below stack for all file types) */
	GtkWidget	*details_scroll;
	GtkWidget	*details_grid;
	GtkWidget	*detail_name;
	GtkWidget	*detail_size;
	GtkWidget	*detail_type;
	GtkWidget	*detail_modified;
	GtkWidget	*detail_permissions;
	GtkWidget	*detail_location;
	GtkWidget	*detail_gps;
	GtkWidget	*detail_gps_label;

	/* GPS map mini-tile */
	GtkWidget	*detail_gps_map;
	GtkWidget	*gps_map_event_box;
	double		 gps_lat;
	double		 gps_lon;
#ifndef NEMO_SMPL
	GCancellable	*map_cancellable;
#endif

	gboolean	 details_vpaned_set;

	/* State */
	NemoFile	*current_file;
	GCancellable	*cancellable;
	gulong		 file_changed_id;
	guint64		 generation;
	gboolean	 destroyed;
};

G_DEFINE_TYPE (NemoPreviewPane, nemo_preview_pane, GTK_TYPE_BOX)

/* Forward declarations */
#ifdef HAVE_GSTREAMER
static void stop_video (NemoPreviewPane *self);
#endif

static void
cancel_pending_loads (NemoPreviewPane *self)
{
	self->generation++;
	if (self->cancellable != NULL) {
		g_cancellable_cancel (self->cancellable);
		g_clear_object (&self->cancellable);
	}
#ifndef NEMO_SMPL
	if (self->map_cancellable != NULL) {
		g_cancellable_cancel (self->map_cancellable);
		g_clear_object (&self->map_cancellable);
	}
#endif
}

#ifdef NEMO_SMPL
typedef struct {
	GWeakRef pane;
	guint64 generation;
	GFile *location;
	char *cache_path;
	double pixel_x;
	double pixel_y;
} PaneLoadData;

static PaneLoadData *
pane_load_data_new (NemoPreviewPane *self, GFile *location)
{
	PaneLoadData *data = g_new0 (PaneLoadData, 1);

	g_weak_ref_init (&data->pane, self);
	data->generation = self->generation;
	data->location = g_object_ref (location);
	return data;
}

static void
pane_load_data_free (PaneLoadData *data)
{
	g_weak_ref_clear (&data->pane);
	g_object_unref (data->location);
	g_free (data->cache_path);
	g_free (data);
}

static NemoPreviewPane *
pane_load_get_current (GTask *task)
{
	PaneLoadData *data = g_task_get_task_data (task);
	NemoPreviewPane *self = g_weak_ref_get (&data->pane);

	if (self != NULL &&
	    (self->destroyed || self->generation != data->generation ||
	     g_cancellable_is_cancelled (g_task_get_cancellable (task)))) {
		g_object_unref (self);
		return NULL;
	}

	return self;
}

/* This helper is used only by workers, including stream close on unref. */
static GdkPixbuf *
load_scaled_pixbuf (GFile *location, int size,
		    GCancellable *cancellable, GError **error)
{
	GFileInputStream *stream;
	GdkPixbuf *pixbuf;

	stream = g_file_read (location, cancellable, error);
	if (stream == NULL)
		return NULL;

	pixbuf = gdk_pixbuf_new_from_stream_at_scale (G_INPUT_STREAM (stream),
						     size, size, TRUE,
						     cancellable, error);
	g_object_unref (stream);
	return pixbuf;
}
#endif /* NEMO_SMPL */

static void
load_image_preview (NemoPreviewPane *self, NemoFile *file)
{
	GFile *location;
	gboolean fit;

	fit = g_settings_get_boolean (nemo_preview_pane_preferences,
				      "fit-to-pane");
	nemo_image_viewer_set_fit (self->image_viewer, fit);
	nemo_image_viewer_set_show_controls (self->image_viewer, TRUE);

	location = nemo_file_get_location (file);
#ifdef NEMO_SMPL
	nemo_image_viewer_load_location (self->image_viewer, location);
#else
	{
		char *mime = nemo_file_get_mime_type (file);

		if (nemo_preview_mime_is_raw_image (mime)) {
			char *path = g_file_get_path (location);
			if (path != NULL) {
				GError *error = NULL;
				if (!nemo_image_viewer_load_file (self->image_viewer, path, &error)) {
					g_warning ("Preview pane: could not load RAW image: %s",
						   error ? error->message : "unknown");
					g_clear_error (&error);
				}
				g_free (path);
			}
		} else {
			GError *error = NULL;
			GFileInputStream *stream = g_file_read (location, NULL, &error);

			if (error != NULL) {
				g_warning ("Preview pane: cannot open file for reading: %s",
					   error->message);
				g_error_free (error);
				g_free (mime);
				g_object_unref (location);
				return;
			}
			nemo_image_viewer_load_stream_async (self->image_viewer,
							    G_INPUT_STREAM (stream),
							    self->cancellable);
		}
		g_free (mime);
	}
#endif
	g_object_unref (location);

	gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "image");
}

static void
load_text_preview (NemoPreviewPane *self, NemoFile *file)
{
	GFile *location;

	location = nemo_file_get_location (file);
#ifdef NEMO_SMPL
	nemo_paged_viewer_set_mode (self->paged_viewer,
				    NEMO_VIEWER_MODE_TEXT);
	nemo_paged_viewer_open_location (self->paged_viewer, location);
	g_object_unref (location);
#else
	{
		char *path = g_file_get_path (location);
		g_object_unref (location);
		if (path == NULL)
			return;
		nemo_paged_viewer_set_mode (self->paged_viewer, NEMO_VIEWER_MODE_TEXT);
		if (!nemo_paged_viewer_open_file (self->paged_viewer, path, NULL)) {
			g_free (path);
			return;
		}
		g_free (path);
	}
#endif

	gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "text");
}

#ifdef HAVE_EXIF
#define GPS_MAP_TILE_SIZE 256
#define GPS_MAP_ZOOM       15

/* Convert decimal degrees to OSM tile coordinates */
static void
gps_to_tile (double lat, double lon, int zoom,
             int *tile_x, int *tile_y,
             double *pixel_x, double *pixel_y)
{
	double n = pow (2.0, zoom);
#ifdef NEMO_SMPL
	double lat_rad = CLAMP (lat, -85.05112878, 85.05112878) * G_PI / 180.0;
#else
	double lat_rad = lat * G_PI / 180.0;
#endif
	double tx = (lon + 180.0) / 360.0 * n;
	double ty = (1.0 - log (tan (lat_rad) + 1.0 / cos (lat_rad)) / G_PI)
		    / 2.0 * n;

#ifdef NEMO_SMPL
	*tile_x = CLAMP ((int) floor (tx), 0, (int) n - 1);
	*tile_y = CLAMP ((int) floor (ty), 0, (int) n - 1);
#else
	*tile_x = (int) floor (tx);
	*tile_y = (int) floor (ty);
#endif
	*pixel_x = (tx - *tile_x) * GPS_MAP_TILE_SIZE;
	*pixel_y = (ty - *tile_y) * GPS_MAP_TILE_SIZE;
}

/* Build the cache directory path for map tiles */
static char *
gps_map_cache_dir (void)
{
	return g_build_filename (g_get_user_cache_dir (),
				 "nemo", "map-tiles", NULL);
}

/* Build a cache file path for a specific tile */
static char *
gps_map_cache_path (int zoom, int tile_x, int tile_y)
{
	char *dir = gps_map_cache_dir ();
	char *filename = g_strdup_printf ("%d_%d_%d.png",
					  zoom, tile_x, tile_y);
	char *path = g_build_filename (dir, filename, NULL);

	g_free (dir);
	g_free (filename);
	return path;
}

/* Draw a crosshair marker and crop to GPS_MAP_SIZE centered on the
 * GPS point, then set it on the GtkImage widget. */
static void
gps_map_render_tile (NemoPreviewPane *self,
                     GdkPixbuf       *tile_pixbuf,
                     double           pixel_x,
                     double           pixel_y)
{
	cairo_surface_t *surface;
	cairo_t *cr;
	GdkPixbuf *result;
	int src_x, src_y;
	int tw, th;

	if (tile_pixbuf == NULL)
		return;

	tw = gdk_pixbuf_get_width (tile_pixbuf);
	th = gdk_pixbuf_get_height (tile_pixbuf);

	/* Calculate crop origin so the GPS point is centered in the
	 * GPS_MAP_SIZE square.  Clamp to tile boundaries. */
	src_x = (int) (pixel_x - GPS_MAP_SIZE / 2.0);
	src_y = (int) (pixel_y - GPS_MAP_SIZE / 2.0);
	if (src_x < 0) src_x = 0;
	if (src_y < 0) src_y = 0;
	if (src_x + GPS_MAP_SIZE > tw) src_x = tw - GPS_MAP_SIZE;
	if (src_y + GPS_MAP_SIZE > th) src_y = th - GPS_MAP_SIZE;
	if (src_x < 0) src_x = 0;
	if (src_y < 0) src_y = 0;

	/* Create a cairo surface and paint the cropped region */
	surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
					      GPS_MAP_SIZE, GPS_MAP_SIZE);
	cr = cairo_create (surface);

	gdk_cairo_set_source_pixbuf (cr, tile_pixbuf, -src_x, -src_y);
	cairo_paint (cr);

	/* Draw crosshair at the GPS point */
	{
		double cx = pixel_x - src_x;
		double cy = pixel_y - src_y;

		cairo_set_line_width (cr, 2.5);

		/* White outline for visibility */
		cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.9);
		cairo_arc (cr, cx, cy, 10.0, 0, 2 * G_PI);
		cairo_stroke (cr);
		cairo_move_to (cr, cx, cy - 16);
		cairo_line_to (cr, cx, cy - 6);
		cairo_stroke (cr);
		cairo_move_to (cr, cx, cy + 6);
		cairo_line_to (cr, cx, cy + 16);
		cairo_stroke (cr);
		cairo_move_to (cr, cx - 16, cy);
		cairo_line_to (cr, cx - 6, cy);
		cairo_stroke (cr);
		cairo_move_to (cr, cx + 6, cy);
		cairo_line_to (cr, cx + 16, cy);
		cairo_stroke (cr);

		/* Red inner */
		cairo_set_source_rgba (cr, 0.9, 0.1, 0.1, 0.95);
		cairo_set_line_width (cr, 2.0);
		cairo_arc (cr, cx, cy, 9.0, 0, 2 * G_PI);
		cairo_stroke (cr);
		cairo_move_to (cr, cx, cy - 15);
		cairo_line_to (cr, cx, cy - 6);
		cairo_stroke (cr);
		cairo_move_to (cr, cx, cy + 6);
		cairo_line_to (cr, cx, cy + 15);
		cairo_stroke (cr);
		cairo_move_to (cr, cx - 15, cy);
		cairo_line_to (cr, cx - 6, cy);
		cairo_stroke (cr);
		cairo_move_to (cr, cx + 6, cy);
		cairo_line_to (cr, cx + 15, cy);
		cairo_stroke (cr);

		/* Center dot */
		cairo_set_source_rgba (cr, 0.9, 0.1, 0.1, 1.0);
		cairo_arc (cr, cx, cy, 3.0, 0, 2 * G_PI);
		cairo_fill (cr);
	}

	cairo_destroy (cr);

	result = gdk_pixbuf_get_from_surface (surface, 0, 0,
					      GPS_MAP_SIZE, GPS_MAP_SIZE);
	cairo_surface_destroy (surface);

	if (result != NULL) {
		gtk_image_set_from_pixbuf (
			GTK_IMAGE (self->detail_gps_map), result);
		gtk_widget_show (self->gps_map_event_box);
		g_object_unref (result);
	}
}

#ifdef NEMO_SMPL
static void
map_tile_worker (GTask *task, gpointer source_object,
		 gpointer task_data, GCancellable *cancellable)
{
	PaneLoadData *data = task_data;
	GFile *cache = g_file_new_for_path (data->cache_path);
	GdkPixbuf *pixbuf = NULL;
	GError *error = NULL;
	char *buffer = NULL;
	gsize length;

	pixbuf = load_scaled_pixbuf (cache, GPS_MAP_TILE_SIZE, cancellable, &error);
	if (pixbuf != NULL) {
		g_object_unref (cache);
		g_task_return_pointer (task, pixbuf, g_object_unref);
		return;
	}

	if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
	    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		g_debug ("Preview pane: cannot read cached GPS map: %s", error->message);
	g_clear_error (&error);

	pixbuf = load_scaled_pixbuf (data->location, GPS_MAP_TILE_SIZE,
				    cancellable, &error);
	if (pixbuf == NULL) {
		g_object_unref (cache);
		g_task_return_error (task, error);
		return;
	}

	if (!g_cancellable_is_cancelled (cancellable)) {
		GFile *dir = g_file_get_parent (cache);

		if (!g_file_make_directory_with_parents (dir, cancellable, &error) &&
		    g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
			g_clear_error (&error);
		g_object_unref (dir);

		if (error == NULL &&
		    gdk_pixbuf_save_to_buffer (pixbuf, &buffer, &length,
					      "png", &error, NULL)) {
			g_file_replace_contents (cache, buffer, length, NULL, FALSE,
						 G_FILE_CREATE_PRIVATE |
						 G_FILE_CREATE_REPLACE_DESTINATION,
						 NULL, cancellable, &error);
			g_free (buffer);
		}
		if (error != NULL) {
			if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
				g_debug ("Preview pane: cannot cache GPS map: %s", error->message);
			g_clear_error (&error);
		}
	}

	g_object_unref (cache);
	g_task_return_pointer (task, pixbuf, g_object_unref);
}

static void
map_tile_ready_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	GTask *task = G_TASK (result);
	PaneLoadData *data = g_task_get_task_data (task);
	NemoPreviewPane *self = pane_load_get_current (task);
	GError *error = NULL;
	GdkPixbuf *pixbuf = g_task_propagate_pointer (task, &error);

	if (self != NULL) {
		if (pixbuf != NULL)
			gps_map_render_tile (self, pixbuf, data->pixel_x, data->pixel_y);
		else if (error != NULL &&
			 !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_debug ("Preview pane: GPS map unavailable: %s", error->message);
		g_object_unref (self);
	}
	g_clear_object (&pixbuf);
	g_clear_error (&error);
}

static void
gps_map_fetch_tile (NemoPreviewPane *self,
                    double lat, double lon)
{
	int tile_x, tile_y;
	double pixel_x, pixel_y;
	char *url;
	GFile *tile_file;
	PaneLoadData *data;
	GTask *task;

	gps_to_tile (lat, lon, GPS_MAP_ZOOM,
	             &tile_x, &tile_y, &pixel_x, &pixel_y);

	url = g_strdup_printf ("https://tile.openstreetmap.org/%d/%d/%d.png",
			       GPS_MAP_ZOOM, tile_x, tile_y);
	tile_file = g_file_new_for_uri (url);
	g_free (url);

	data = pane_load_data_new (self, tile_file);
	data->pixel_x = pixel_x;
	data->pixel_y = pixel_y;
	data->cache_path = gps_map_cache_path (GPS_MAP_ZOOM, tile_x, tile_y);
	task = g_task_new (NULL, self->cancellable, map_tile_ready_cb, NULL);
	g_task_set_priority (task, G_PRIORITY_LOW);
	g_task_set_task_data (task, data, (GDestroyNotify) pane_load_data_free);
	nemo_preview_run_task (task, map_tile_worker);
	g_object_unref (task);
	g_object_unref (tile_file);
}

typedef struct {
	double latitude;
	double longitude;
	char label[128];
} GpsData;

static gboolean
read_gps_coordinate (ExifEntry *entry, ExifByteOrder order,
		     double values[3], double limit)
{
	guint i;

	if (entry == NULL || entry->data == NULL || entry->size < 24 ||
	    entry->format != EXIF_FORMAT_RATIONAL || entry->components < 3)
		return FALSE;

	for (i = 0; i < 3; i++) {
		ExifRational value = exif_get_rational (entry->data + i * 8, order);
		if (value.denominator == 0)
			return FALSE;
		values[i] = (double) value.numerator / value.denominator;
	}

	return values[1] < 60 && values[2] < 60 &&
	       values[0] + values[1] / 60 + values[2] / 3600 <= limit;
}

static void
gps_metadata_worker (GTask *task, gpointer source_object,
		     gpointer task_data, GCancellable *cancellable)
{
	PaneLoadData *data = task_data;
	GFileInputStream *stream;
	GError *error = NULL;
	ExifLoader *loader;
	ExifData *exif;
	GpsData *gps = NULL;
	guchar buffer[16384];
	gssize count;

	if (g_task_return_error_if_cancelled (task))
		return;

	stream = g_file_read (data->location, cancellable, &error);
	if (stream == NULL) {
		g_task_return_error (task, error);
		return;
	}

	loader = exif_loader_new ();
	if (loader == NULL) {
		g_object_unref (stream);
		g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
					"Could not allocate EXIF loader");
		return;
	}
	while ((count = g_input_stream_read (G_INPUT_STREAM (stream), buffer,
					    sizeof buffer, cancellable, &error)) > 0) {
		if (!exif_loader_write (loader, buffer, count))
			break;
	}
	g_object_unref (stream);
	exif = error == NULL ? exif_loader_get_data (loader) : NULL;
	exif_loader_unref (loader);

	if (error != NULL) {
		g_task_return_error (task, error);
		return;
	}

	if (exif != NULL) {
		ExifEntry *lat_entry, *lon_entry, *lat_ref, *lon_ref;
		ExifByteOrder order = exif_data_get_byte_order (exif);
		double lat[3], lon[3];
		char lat_c = 'N', lon_c = 'E';

		lat_entry = exif_content_get_entry (exif->ifd[EXIF_IFD_GPS], 0x0002);
		lon_entry = exif_content_get_entry (exif->ifd[EXIF_IFD_GPS], 0x0004);
		lat_ref = exif_content_get_entry (exif->ifd[EXIF_IFD_GPS], 0x0001);
		lon_ref = exif_content_get_entry (exif->ifd[EXIF_IFD_GPS], 0x0003);
		if (lat_ref != NULL && lat_ref->data != NULL && lat_ref->size > 0)
			lat_c = g_ascii_toupper (lat_ref->data[0]);
		if (lon_ref != NULL && lon_ref->data != NULL && lon_ref->size > 0)
			lon_c = g_ascii_toupper (lon_ref->data[0]);

		if (read_gps_coordinate (lat_entry, order, lat, 90) &&
		    read_gps_coordinate (lon_entry, order, lon, 180) &&
		    (lat_c == 'N' || lat_c == 'S') &&
		    (lon_c == 'E' || lon_c == 'W')) {
			gps = g_new0 (GpsData, 1);
			gps->latitude = lat[0] + lat[1] / 60 + lat[2] / 3600;
			gps->longitude = lon[0] + lon[1] / 60 + lon[2] / 3600;
			if (lat_c == 'S')
				gps->latitude = -gps->latitude;
			if (lon_c == 'W')
				gps->longitude = -gps->longitude;
			g_snprintf (gps->label, sizeof gps->label,
				    "%.0f\xc2\xb0%.0f'%.1f\"%c "
				    "%.0f\xc2\xb0%.0f'%.1f\"%c",
				    lat[0], lat[1], lat[2], lat_c,
				    lon[0], lon[1], lon[2], lon_c);
		}
		exif_data_unref (exif);
	}

	g_task_return_pointer (task, gps, g_free);
}

static void
gps_metadata_ready_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	GTask *task = G_TASK (result);
	NemoPreviewPane *self = pane_load_get_current (task);
	GError *error = NULL;
	GpsData *gps = g_task_propagate_pointer (task, &error);

	if (self != NULL) {
		if (gps != NULL) {
			gtk_label_set_text (GTK_LABEL (self->detail_gps), gps->label);
			gtk_widget_show (self->detail_gps);
			gtk_widget_show (self->detail_gps_label);
			self->gps_lat = gps->latitude;
			self->gps_lon = gps->longitude;
			gps_map_fetch_tile (self, gps->latitude, gps->longitude);
		} else {
			g_debug ("Preview pane: GPS metadata unavailable: %s",
				 error != NULL ? error->message : "no valid GPS coordinates");
		}
		g_object_unref (self);
	}
	g_clear_error (&error);
	g_free (gps);
}

static void
load_gps_metadata (NemoPreviewPane *self, NemoFile *file)
{
	char *mime = nemo_file_get_mime_type (file);

	if (nemo_preview_mime_is_image (mime)) {
		GFile *location = nemo_file_get_location (file);
		GTask *task = g_task_new (NULL, self->cancellable,
					 gps_metadata_ready_cb, NULL);

		g_task_set_priority (task, G_PRIORITY_LOW);
		g_task_set_task_data (task, pane_load_data_new (self, location),
				     (GDestroyNotify) pane_load_data_free);
		nemo_preview_run_task (task, gps_metadata_worker);
		g_object_unref (task);
		g_object_unref (location);
	}
	g_free (mime);
}
#else
typedef struct {
	NemoPreviewPane *self;
	double pixel_x;
	double pixel_y;
	char *cache_path;
} MapTileData;

static void
map_tile_data_free (MapTileData *data)
{
	g_free (data->cache_path);
	g_free (data);
}

static void
map_tile_download_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	MapTileData *data = user_data;
	GError *error = NULL;
	GFileInputStream *stream = g_file_read_finish (G_FILE (source), result, &error);
	GdkPixbuf *pixbuf = NULL;

	if (stream != NULL) {
		pixbuf = gdk_pixbuf_new_from_stream (G_INPUT_STREAM (stream), NULL, &error);
		g_object_unref (stream);
	}
	if (pixbuf != NULL) {
		char *dir = gps_map_cache_dir ();
		g_mkdir_with_parents (dir, 0755);
		g_free (dir);
		gdk_pixbuf_save (pixbuf, data->cache_path, "png", NULL, NULL);
		gps_map_render_tile (data->self, pixbuf, data->pixel_x, data->pixel_y);
		g_object_unref (pixbuf);
	}
	g_clear_error (&error);
	map_tile_data_free (data);
}

static void
gps_map_fetch_tile (NemoPreviewPane *self, double lat, double lon)
{
	int tile_x, tile_y;
	double pixel_x, pixel_y;
	char *cache_path;
	GdkPixbuf *pixbuf;

	if (self->map_cancellable != NULL) {
		g_cancellable_cancel (self->map_cancellable);
		g_clear_object (&self->map_cancellable);
	}
	gps_to_tile (lat, lon, GPS_MAP_ZOOM, &tile_x, &tile_y, &pixel_x, &pixel_y);
	cache_path = gps_map_cache_path (GPS_MAP_ZOOM, tile_x, tile_y);
	pixbuf = gdk_pixbuf_new_from_file (cache_path, NULL);
	if (pixbuf != NULL) {
		gps_map_render_tile (self, pixbuf, pixel_x, pixel_y);
		g_object_unref (pixbuf);
		g_free (cache_path);
	} else {
		char *url = g_strdup_printf ("https://tile.openstreetmap.org/%d/%d/%d.png",
					     GPS_MAP_ZOOM, tile_x, tile_y);
		GFile *tile_file = g_file_new_for_uri (url);
		MapTileData *data = g_new0 (MapTileData, 1);

		g_free (url);
		self->map_cancellable = g_cancellable_new ();
		data->self = self;
		data->pixel_x = pixel_x;
		data->pixel_y = pixel_y;
		data->cache_path = cache_path;
		g_file_read_async (tile_file, G_PRIORITY_LOW, self->map_cancellable,
				   map_tile_download_cb, data);
		g_object_unref (tile_file);
	}
}

static void
load_gps_metadata (NemoPreviewPane *self, NemoFile *file)
{
	char *mime = nemo_file_get_mime_type (file);
	gboolean is_image = nemo_preview_mime_is_image (mime);
	GFile *location;
	char *path;
	ExifData *exif;
	ExifEntry *lat_entry, *lon_entry, *lat_ref, *lon_ref;
	ExifByteOrder order;
	double lat[3], lon[3];
	char lat_c = 'N', lon_c = 'E';
	char label[128];
	guint i;

	g_free (mime);
	if (!is_image)
		return;
	location = nemo_file_get_location (file);
	path = g_file_get_path (location);
	g_object_unref (location);
	if (path == NULL)
		return;
	exif = exif_data_new_from_file (path);
	g_free (path);
	if (exif == NULL)
		return;

	lat_entry = exif_content_get_entry (exif->ifd[EXIF_IFD_GPS], 0x0002);
	lon_entry = exif_content_get_entry (exif->ifd[EXIF_IFD_GPS], 0x0004);
	lat_ref = exif_content_get_entry (exif->ifd[EXIF_IFD_GPS], 0x0001);
	lon_ref = exif_content_get_entry (exif->ifd[EXIF_IFD_GPS], 0x0003);
	if (lat_entry == NULL || lon_entry == NULL ||
	    lat_entry->size < 24 || lon_entry->size < 24) {
		exif_data_unref (exif);
		return;
	}
	order = exif_data_get_byte_order (exif);
	for (i = 0; i < 3; i++) {
		ExifRational lat_r = exif_get_rational (lat_entry->data + i * 8, order);
		ExifRational lon_r = exif_get_rational (lon_entry->data + i * 8, order);
		lat[i] = lat_r.denominator > 0 ? (double) lat_r.numerator / lat_r.denominator : 0;
		lon[i] = lon_r.denominator > 0 ? (double) lon_r.numerator / lon_r.denominator : 0;
	}
	if (lat_ref != NULL && lat_ref->data != NULL)
		lat_c = lat_ref->data[0];
	if (lon_ref != NULL && lon_ref->data != NULL)
		lon_c = lon_ref->data[0];
	g_snprintf (label, sizeof label,
		    "%.0f\xc2\xb0%.0f'%.1f\"%c %.0f\xc2\xb0%.0f'%.1f\"%c",
		    lat[0], lat[1], lat[2], lat_c, lon[0], lon[1], lon[2], lon_c);
	gtk_label_set_text (GTK_LABEL (self->detail_gps), label);
	gtk_widget_show (self->detail_gps);
	gtk_widget_show (self->detail_gps_label);
	self->gps_lat = lat[0] + lat[1] / 60 + lat[2] / 3600;
	self->gps_lon = lon[0] + lon[1] / 60 + lon[2] / 3600;
	if (lat_c == 'S' || lat_c == 's')
		self->gps_lat = -self->gps_lat;
	if (lon_c == 'W' || lon_c == 'w')
		self->gps_lon = -self->gps_lon;
	gps_map_fetch_tile (self, self->gps_lat, self->gps_lon);
	exif_data_unref (exif);
}
#endif /* NEMO_SMPL */

/* Click handler: open the GPS location in the default map application */
static gboolean
gps_map_clicked_cb (GtkWidget      *widget,
                    GdkEventButton *event,
                    gpointer        user_data)
{
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (user_data);
	char *uri;

	if (event->type != GDK_BUTTON_PRESS || event->button != 1)
		return FALSE;

	/* Open OSM in the default browser */
	uri = g_strdup_printf ("https://www.openstreetmap.org/?mlat=%.6f&mlon=%.6f#map=16/%.6f/%.6f",
			       self->gps_lat, self->gps_lon,
			       self->gps_lat, self->gps_lon);

	gtk_show_uri_on_window (
		GTK_WINDOW (gtk_widget_get_toplevel (widget)),
		uri, event->time, NULL);
	g_free (uri);

	return TRUE;
}
#endif /* HAVE_EXIF */

static void
update_details (NemoPreviewPane *self, NemoFile *file)
{
	char *str;
	GFile *location;
	GFile *parent;
	char *parent_path;

	/* Name */
	str = nemo_file_get_display_name (file);
	gtk_label_set_text (GTK_LABEL (self->detail_name), str);
	g_free (str);

	/* Size */
	str = nemo_file_get_string_attribute (file, "size");
	gtk_label_set_text (GTK_LABEL (self->detail_size),
			    str != NULL ? str : "\xe2\x80\x94");
	g_free (str);

	/* Type */
	str = nemo_file_get_string_attribute (file, "type");
	gtk_label_set_text (GTK_LABEL (self->detail_type),
			    str != NULL ? str : "\xe2\x80\x94");
	g_free (str);

	/* Modified */
	str = nemo_file_get_string_attribute (file, "date_modified");
	gtk_label_set_text (GTK_LABEL (self->detail_modified),
			    str != NULL ? str : "\xe2\x80\x94");
	g_free (str);

	/* Permissions */
	str = nemo_file_get_string_attribute (file, "permissions");
	gtk_label_set_text (GTK_LABEL (self->detail_permissions),
			    str != NULL ? str : "\xe2\x80\x94");
	g_free (str);

	/* Location */
	str = nemo_file_get_activation_uri (file);
	if (str != NULL) {
		location = g_file_new_for_uri (str);
		g_free (str);
		parent = g_file_get_parent (location);
		if (parent != NULL) {
			parent_path = g_file_get_parse_name (parent);
			gtk_label_set_text (
				GTK_LABEL (self->detail_location),
				parent_path);
			g_free (parent_path);
			g_object_unref (parent);
		} else {
			gtk_label_set_text (
				GTK_LABEL (self->detail_location),
				"\xe2\x80\x94");
		}
		g_object_unref (location);
	} else {
		location = nemo_file_get_location (file);
		parent = g_file_get_parent (location);
		if (parent != NULL) {
			parent_path = g_file_get_parse_name (parent);
			gtk_label_set_text (
				GTK_LABEL (self->detail_location),
				parent_path);
			g_free (parent_path);
			g_object_unref (parent);
		} else {
			gtk_label_set_text (
				GTK_LABEL (self->detail_location),
				"\xe2\x80\x94");
		}
		g_object_unref (location);
	}

	/* GPS Location (images only) */
	gtk_widget_hide (self->detail_gps);
	gtk_widget_hide (self->detail_gps_label);
	gtk_widget_hide (self->gps_map_event_box);
	gtk_image_clear (GTK_IMAGE (self->detail_gps_map));
#ifdef HAVE_EXIF
	load_gps_metadata (self, file);
#endif

	gtk_widget_show (self->details_scroll);
}

static void
clear_details (NemoPreviewPane *self)
{
	gtk_label_set_text (GTK_LABEL (self->detail_name), "");
	gtk_label_set_text (GTK_LABEL (self->detail_size), "");
	gtk_label_set_text (GTK_LABEL (self->detail_type), "");
	gtk_label_set_text (GTK_LABEL (self->detail_modified), "");
	gtk_label_set_text (GTK_LABEL (self->detail_permissions), "");
	gtk_label_set_text (GTK_LABEL (self->detail_location), "");
	gtk_widget_hide (self->detail_gps);
	gtk_widget_hide (self->detail_gps_label);
	gtk_widget_hide (self->gps_map_event_box);
	gtk_image_clear (GTK_IMAGE (self->detail_gps_map));
	gtk_widget_hide (self->details_scroll);
}

static void
disconnect_file (NemoPreviewPane *self)
{
	if (self->file_changed_id != 0 && self->current_file != NULL) {
		g_signal_handler_disconnect (self->current_file,
					     self->file_changed_id);
		self->file_changed_id = 0;
	}
}

#ifdef NEMO_SMPL
static void
thumbnail_worker (GTask *task, gpointer source_object,
		  gpointer task_data, GCancellable *cancellable)
{
	PaneLoadData *data = task_data;
	GError *error = NULL;
	GdkPixbuf *pixbuf;

	pixbuf = load_scaled_pixbuf (data->location, 128, cancellable, &error);
	if (pixbuf != NULL)
		g_task_return_pointer (task, pixbuf, g_object_unref);
	else
		g_task_return_error (task, error);
}

static void
thumbnail_ready_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	GTask *task = G_TASK (result);
	NemoPreviewPane *self = pane_load_get_current (task);
	GError *error = NULL;
	GdkPixbuf *pixbuf = g_task_propagate_pointer (task, &error);

	if (self != NULL) {
		if (pixbuf != NULL)
			gtk_image_set_from_pixbuf (GTK_IMAGE (self->info_icon), pixbuf);
		else if (error != NULL &&
			 !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_debug ("Preview pane: thumbnail unavailable: %s", error->message);
		g_object_unref (self);
	}
	g_clear_object (&pixbuf);
	g_clear_error (&error);
}
#endif /* NEMO_SMPL */

static void
show_info_preview (NemoPreviewPane *self, NemoFile *file)
{
	GIcon *icon;
	char *thumb_path;

	gtk_widget_hide (self->info_error_label);

#ifndef NEMO_SMPL
	thumb_path = nemo_file_get_thumbnail_path (file);
	if (thumb_path != NULL) {
		GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale (
			thumb_path, 128, 128, TRUE, NULL);
		g_free (thumb_path);
		if (pixbuf != NULL) {
			gtk_image_set_from_pixbuf (GTK_IMAGE (self->info_icon), pixbuf);
			g_object_unref (pixbuf);
			gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "info");
			return;
		}
	}
#endif

	/* Show the icon immediately while a thumbnail is being read. */
	icon = nemo_file_get_gicon (file, NEMO_FILE_ICON_FLAGS_NONE);
	if (icon != NULL) {
		gtk_image_set_from_gicon (GTK_IMAGE (self->info_icon),
					  icon, GTK_ICON_SIZE_DIALOG);
		g_object_unref (icon);
	} else {
		gtk_image_set_from_icon_name (GTK_IMAGE (self->info_icon),
					     "text-x-generic", GTK_ICON_SIZE_DIALOG);
	}

#ifdef NEMO_SMPL
	thumb_path = nemo_file_get_thumbnail_path (file);
	if (thumb_path != NULL) {
		GFile *location = g_file_new_for_path (thumb_path);
		GTask *task = g_task_new (NULL, self->cancellable,
					 thumbnail_ready_cb, NULL);

		g_task_set_priority (task, G_PRIORITY_LOW);
		g_task_set_task_data (task, pane_load_data_new (self, location),
				     (GDestroyNotify) pane_load_data_free);
		nemo_preview_run_task (task, thumbnail_worker);
		g_object_unref (task);
		g_object_unref (location);
		g_free (thumb_path);
	}
#endif

	gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "info");
}

#ifdef HAVE_GSTREAMER

static void
stop_video (NemoPreviewPane *self)
{
	if (self->seek_update_id != 0) {
		g_source_remove (self->seek_update_id);
		self->seek_update_id = 0;
	}

	if (self->bus_watch_id != 0) {
		g_source_remove (self->bus_watch_id);
		self->bus_watch_id = 0;
	}

#ifdef NEMO_SMPL
	nemo_preview_media_frames_stop (g_steal_pointer (&self->video_frames));
	self->video_playing = FALSE;
	self->video_ready = FALSE;
#endif
	if (self->pipeline != NULL) {
#ifdef NEMO_SMPL
		if (!nemo_preview_media_set_state_async (self->pipeline, GST_STATE_NULL))
			g_debug ("Preview pane: could not queue media shutdown");
#else
		gst_element_set_state (self->pipeline, GST_STATE_NULL);
#endif
		gst_object_unref (self->pipeline);
		self->pipeline = NULL;
	}

	g_mutex_lock (&self->frame_mutex);
	if (self->frame_surface != NULL) {
		cairo_surface_destroy (self->frame_surface);
		self->frame_surface = NULL;
	}
	self->video_width = 0;
	self->video_height = 0;
	g_mutex_unlock (&self->frame_mutex);

	self->seek_lock = FALSE;
}

#ifdef NEMO_SMPL
static gboolean
request_video_state (NemoPreviewPane *self, GstState state)
{
	if (nemo_preview_media_set_state_async (self->pipeline, state)) {
		self->video_playing = state == GST_STATE_PLAYING;
		return TRUE;
	}

	stop_video (self);
	if (self->current_file != NULL) {
		show_info_preview (self, self->current_file);
		gtk_label_set_text (GTK_LABEL (self->info_error_label),
				    _("Media preview is busy.\nTry selecting this file again."));
		gtk_widget_show (self->info_error_label);
	}
	return FALSE;
}
#endif

static gboolean
video_bus_message_cb (GstBus     *bus,
		     GstMessage *msg,
		     gpointer    user_data)
{
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (user_data);

	if (self->destroyed || self->pipeline == NULL)
		return G_SOURCE_REMOVE;

	switch (GST_MESSAGE_TYPE (msg)) {
#ifdef NEMO_SMPL
	case GST_MESSAGE_ASYNC_DONE:
		if (GST_MESSAGE_SRC (msg) == GST_OBJECT (self->pipeline))
			self->video_ready = TRUE;
		break;
	case GST_MESSAGE_STATE_CHANGED:
		if (GST_MESSAGE_SRC (msg) == GST_OBJECT (self->pipeline)) {
			GstState state, pending;
			gst_message_parse_state_changed (msg, NULL, &state, &pending);
			if (pending == GST_STATE_VOID_PENDING)
				self->video_ready = state >= GST_STATE_PAUSED;
		}
		break;
#endif
	case GST_MESSAGE_EOS:
#ifdef NEMO_SMPL
		if (!self->video_ready)
			break;
#endif
		/* Loop the clip so it keeps playing in the preview */
		gst_element_seek_simple (self->pipeline,
					 GST_FORMAT_TIME,
					 GST_SEEK_FLAG_FLUSH |
					 GST_SEEK_FLAG_KEY_UNIT,
					 0);
		break;

	case GST_MESSAGE_ERROR: {
		GError *err = NULL;
		char *label_text;

		gst_message_parse_error (msg, &err, NULL);
		g_warning ("Preview pane: media playback error: %s",
			   err ? err->message : "unknown");

		stop_video (self);

		/* Fall back to the info page and show a helpful message */
		if (self->current_file != NULL) {
			show_info_preview (self, self->current_file);

			if (err != NULL &&
			    ((err->domain == GST_CORE_ERROR
#ifdef NEMO_SMPL
			      && err->code != GST_CORE_ERROR_STATE_CHANGE
#endif
			     ) ||
			     err->domain == GST_STREAM_ERROR)) {
				label_text = g_strdup_printf (
					_("Unable to preview this file.\n"
					  "You may need to install additional "
					  "GStreamer plugins\n"
					  "(e.g. gstreamer-plugins-bad or "
					  "gstreamer-plugins-ugly)."));
			} else {
				label_text = g_strdup_printf (
					_("Unable to preview this file.\n%s"),
					err ? err->message : "");
			}

			gtk_label_set_text (
				GTK_LABEL (self->info_error_label),
				label_text);
			gtk_widget_show (self->info_error_label);
			g_free (label_text);
		}

		g_clear_error (&err);
		break;
	}

	default:
		break;
	}

	return TRUE;
}

static void
render_video_sample (NemoPreviewPane *self, GstSample *sample)
{
	GstBuffer *buffer;
	GstCaps *caps;
	GstVideoInfo vinfo;
	GstMapInfo map;
	cairo_surface_t *surface;
	int w, h, stride;
	const guint8 *src;
	guint8 *dst;
	int i;

	if (sample == NULL)
		return;
#ifdef NEMO_SMPL
	if (self->destroyed || self->pipeline == NULL) {
		gst_sample_unref (sample);
		return;
	}
#endif

	buffer = gst_sample_get_buffer (sample);
	caps = gst_sample_get_caps (sample);
	if (buffer == NULL || caps == NULL) {
		gst_sample_unref (sample);
		return;
	}

	if (!gst_video_info_from_caps (&vinfo, caps)) {
		gst_sample_unref (sample);
		return;
	}

	w = GST_VIDEO_INFO_WIDTH (&vinfo);
	h = GST_VIDEO_INFO_HEIGHT (&vinfo);

	if (!gst_buffer_map (buffer, &map, GST_MAP_READ)) {
		gst_sample_unref (sample);
		return;
	}

	surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24, w, h);
	stride = cairo_image_surface_get_stride (surface);
	dst = cairo_image_surface_get_data (surface);
	src = map.data;

	for (i = 0; i < h; i++) {
		memcpy (dst + i * stride,
		        src + i * (int) GST_VIDEO_INFO_PLANE_STRIDE (&vinfo, 0),
		        w * 4);
	}

	cairo_surface_mark_dirty (surface);
	gst_buffer_unmap (buffer, &map);
	gst_sample_unref (sample);

	g_mutex_lock (&self->frame_mutex);
	if (self->frame_surface != NULL)
		cairo_surface_destroy (self->frame_surface);
	self->frame_surface = surface;
	self->video_width = w;
	self->video_height = h;
	g_mutex_unlock (&self->frame_mutex);
	if (self->video_area != NULL)
		gtk_widget_queue_draw (self->video_area);
}

#ifdef NEMO_SMPL
static void
video_sample_ready_cb (GObject *widget, GstSample *sample)
{
	/* The per-sink handle discards old samples before a pipeline is detached. */
	render_video_sample (NEMO_PREVIEW_PANE (widget), sample);
}
#else
static GstFlowReturn
new_sample_cb (GstAppSink *sink, gpointer user_data)
{
	render_video_sample (NEMO_PREVIEW_PANE (user_data),
			     gst_app_sink_pull_sample (sink));

	return GST_FLOW_OK;
}
#endif

static gboolean
video_area_draw_cb (GtkWidget *widget,
		    cairo_t   *cr,
		    gpointer   user_data)
{
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (user_data);
	GtkAllocation alloc;

	gtk_widget_get_allocation (widget, &alloc);

	/* Always paint black background */
	cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
	cairo_paint (cr);

	g_mutex_lock (&self->frame_mutex);
	if (self->frame_surface != NULL && self->video_width > 0 && self->video_height > 0) {
		double sx = (double) alloc.width  / (double) self->video_width;
		double sy = (double) alloc.height / (double) self->video_height;
		double scale = (sx < sy) ? sx : sy;
		double dx = (alloc.width  - self->video_width  * scale) / 2.0;
		double dy = (alloc.height - self->video_height * scale) / 2.0;

		cairo_save (cr);
		cairo_translate (cr, dx, dy);
		cairo_scale (cr, scale, scale);
		cairo_set_source_surface (cr, self->frame_surface, 0, 0);
		cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_BILINEAR);
		cairo_paint (cr);
		cairo_restore (cr);
	}
	g_mutex_unlock (&self->frame_mutex);

	return TRUE;
}

static void
play_pause_clicked_cb (GtkButton *btn, gpointer user_data)
{
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (user_data);
#ifndef NEMO_SMPL
	GstState state;
#endif
	GtkWidget *img;

	if (self->pipeline == NULL) {
		return;
	}

#ifdef NEMO_SMPL
	if (!request_video_state (self, self->video_playing ?
				 GST_STATE_PAUSED : GST_STATE_PLAYING))
		return;
	img = gtk_image_new_from_icon_name (
		self->video_playing ? "media-playback-pause-symbolic" :
				      "media-playback-start-symbolic",
		GTK_ICON_SIZE_SMALL_TOOLBAR);
#else
	gst_element_get_state (self->pipeline, &state, NULL, 0);

	if (state == GST_STATE_PLAYING) {
		gst_element_set_state (self->pipeline, GST_STATE_PAUSED);
		img = gtk_image_new_from_icon_name (
			"media-playback-start-symbolic",
			GTK_ICON_SIZE_SMALL_TOOLBAR);
	} else {
		gst_element_set_state (self->pipeline, GST_STATE_PLAYING);
		img = gtk_image_new_from_icon_name (
			"media-playback-pause-symbolic",
			GTK_ICON_SIZE_SMALL_TOOLBAR);
	}
#endif

	gtk_button_set_image (GTK_BUTTON (btn), img);
}

static void
mute_clicked_cb (GtkButton *btn, gpointer user_data)
{
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (user_data);
	GtkWidget *img;

	if (self->pipeline == NULL) {
		return;
	}

	self->video_muted = !self->video_muted;
	g_object_set (self->pipeline, "mute", self->video_muted, NULL);

	img = gtk_image_new_from_icon_name (
		self->video_muted ? "audio-volume-muted-symbolic"
				  : "audio-volume-high-symbolic",
		GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_button_set_image (GTK_BUTTON (btn), img);
}

/* Format a GStreamer timestamp as M:SS for the seek bar label */
static gchar *
seek_scale_format_cb (GtkScale *scale,
		      gdouble   value,
		      gpointer  user_data)
{
	gint64 pos = (gint64) value;
	gint secs = (gint) (pos / GST_SECOND);
	gint mins = secs / 60;

	secs %= 60;
	return g_strdup_printf ("%d:%02d", mins, secs);
}

/* Periodic callback to update the seek bar position during playback */
static gboolean
seek_position_update_cb (gpointer user_data)
{
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (user_data);
	gint64 pos = 0, dur = 0;

	if (self->pipeline == NULL) {
		self->seek_update_id = 0;
		return G_SOURCE_REMOVE;
	}
#ifdef NEMO_SMPL
	if (!self->video_ready)
		return G_SOURCE_CONTINUE;
#endif

	/* Don't touch the slider while the user is interacting with it */
	if (self->seek_lock) {
		return G_SOURCE_CONTINUE;
	}

	if (!gst_element_query_position (self->pipeline, GST_FORMAT_TIME,
					 &pos)) {
		return G_SOURCE_CONTINUE;
	}

	if (gst_element_query_duration (self->pipeline, GST_FORMAT_TIME,
					&dur) && dur > 0) {
		gtk_range_set_range (GTK_RANGE (self->seek_scale),
				     0.0, (gdouble) dur);
	}

	gtk_range_set_value (GTK_RANGE (self->seek_scale), (gdouble) pos);

	return G_SOURCE_CONTINUE;
}

/*
 * Clicking anywhere on the slider should jump to that position, not
 * step incrementally.  GTK only does this for button 1, so we mangle
 * the event — same trick that Totem uses.
 */
static gboolean
seek_press_cb (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (user_data);

	event->button = GDK_BUTTON_PRIMARY;
	self->seek_lock = TRUE;

	return FALSE;
}

/*
 * Perform the actual seek when the user releases the mouse button.
 * Seeking only on release (rather than on every value-changed during
 * a drag) avoids flooding the pipeline with flush-seeks, which is
 * what causes snap-back and playback stalls.
 */
static gboolean
seek_release_cb (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (user_data);
	gint64 target;

	event->button = GDK_BUTTON_PRIMARY;
	self->seek_lock = FALSE;

	if (self->pipeline == NULL) {
		return FALSE;
	}
#ifdef NEMO_SMPL
	if (!self->video_ready)
		return FALSE;
#endif

	target = (gint64) gtk_range_get_value (GTK_RANGE (widget));
	gst_element_seek_simple (self->pipeline,
				 GST_FORMAT_TIME,
				 GST_SEEK_FLAG_FLUSH |
				 GST_SEEK_FLAG_KEY_UNIT,
				 target);

	return FALSE;
}

static void
load_media_preview (NemoPreviewPane *self,
		   NemoFile        *file,
		   gboolean         is_audio)
{
	GFile *location;
	char *uri;
	GstBus *bus;

	stop_video (self);

	if (!gst_is_initialized ()) {
		if (!gst_init_check (NULL, NULL, NULL)) {
			g_warning ("Preview pane: GStreamer init failed");
			show_info_preview (self, file);
			return;
		}
	}

	self->pipeline = gst_element_factory_make ("playbin",
						   "preview-player");
	if (self->pipeline == NULL) {
		g_warning ("Preview pane: failed to create playbin element");
		show_info_preview (self, file);
		return;
	}

	location = nemo_file_get_location (file);
	uri = g_file_get_uri (location);
	g_object_unref (location);

	g_object_set (self->pipeline,
		      "uri", uri,
		      "mute", self->video_muted,
		      NULL);
	g_free (uri);

	if (is_audio) {
		GIcon *gicon;

		gtk_widget_hide (self->video_area);
		gtk_widget_show (self->audio_icon);

		gicon = nemo_file_get_gicon (file,
					    NEMO_FILE_ICON_FLAGS_NONE);
		if (gicon != NULL) {
			gtk_image_set_from_gicon (
				GTK_IMAGE (self->audio_icon),
				gicon, GTK_ICON_SIZE_DIALOG);
			g_object_unref (gicon);
		} else {
			gtk_image_set_from_icon_name (
				GTK_IMAGE (self->audio_icon),
				"audio-x-generic-symbolic",
				GTK_ICON_SIZE_DIALOG);
		}
	} else {
		GstElement *appsink;
		GstCaps *caps;

		gtk_widget_show (self->video_area);
		gtk_widget_hide (self->audio_icon);

		/* Use appsink as video sink — extract frames and paint with Cairo */
		appsink = gst_element_factory_make ("appsink", "pp-vsink");
		if (appsink != NULL) {
			caps = gst_caps_from_string ("video/x-raw, format=BGRx");
			g_object_set (appsink,
			              "caps", caps,
			              "emit-signals", TRUE,
			              "max-buffers", 2,
			              "drop", TRUE,
			              NULL);
			gst_caps_unref (caps);

#ifdef NEMO_SMPL
			self->video_frames = nemo_preview_media_connect_sink (
				appsink, G_OBJECT (self), video_sample_ready_cb);
#else
			g_signal_connect (appsink, "new-sample",
			                  G_CALLBACK (new_sample_cb), self);
#endif

			g_object_set (self->pipeline, "video-sink", appsink, NULL);
		}
	}

	bus = gst_pipeline_get_bus (GST_PIPELINE (self->pipeline));
	self->bus_watch_id = gst_bus_add_watch (bus,
						video_bus_message_cb,
						self);
	gst_object_unref (bus);

	gtk_button_set_image (GTK_BUTTON (self->play_btn),
		gtk_image_new_from_icon_name (
                        "media-playback-start-symbolic",
                        GTK_ICON_SIZE_SMALL_TOOLBAR));
	gtk_button_set_image (GTK_BUTTON (self->mute_btn),
		gtk_image_new_from_icon_name (
			self->video_muted ? "audio-volume-muted-symbolic"
					   : "audio-volume-high-symbolic",
			GTK_ICON_SIZE_SMALL_TOOLBAR));

	gtk_range_set_range (GTK_RANGE (self->seek_scale), 0.0, 1.0);
	gtk_range_set_value (GTK_RANGE (self->seek_scale), 0.0);
	self->seek_lock = FALSE;

	if (self->seek_update_id != 0) {
		g_source_remove (self->seek_update_id);
	}
	self->seek_update_id = g_timeout_add (250,
                                              seek_position_update_cb,
                                              self);

#ifdef NEMO_SMPL
	if (!request_video_state (self, GST_STATE_PAUSED))
		return;
#else
	gst_element_set_state (self->pipeline, GST_STATE_PAUSED);
#endif

	nemo_image_viewer_set_show_controls (self->image_viewer, FALSE);
	gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "video");
}

#endif /* HAVE_GSTREAMER */

static void
refresh_file_preview (NemoPreviewPane *self, NemoFile *file)
{
	char *mime = nemo_file_get_mime_type (file);

	nemo_image_viewer_clear (self->image_viewer);
	nemo_paged_viewer_close_file (self->paged_viewer);

#ifdef HAVE_GSTREAMER
	if (!nemo_preview_mime_is_video (mime) &&
	    !nemo_preview_mime_is_audio (mime))
		stop_video (self);
#endif

	if (nemo_file_is_directory (file)) {
		show_info_preview (self, file);
#ifdef HAVE_GSTREAMER
	} else if (nemo_preview_mime_is_video (mime)) {
		if (self->pipeline == NULL)
			load_media_preview (self, file, FALSE);
	} else if (nemo_preview_mime_is_audio (mime)) {
		if (self->pipeline == NULL)
			load_media_preview (self, file, TRUE);
#endif
	} else if (nemo_preview_mime_is_image (mime)) {
		load_image_preview (self, file);
	} else if (nemo_preview_mime_is_text (mime)) {
		load_text_preview (self, file);
	} else {
		show_info_preview (self, file);
	}

	g_free (mime);
}

static void
file_changed_cb (NemoFile *file, gpointer user_data)
{
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (user_data);

	if (self->destroyed || file != self->current_file)
		return;
#ifdef NEMO_SMPL
	if (nemo_file_is_gone (file)) {
		nemo_preview_pane_clear (self);
		return;
	}

	cancel_pending_loads (self);
	self->cancellable = g_cancellable_new ();
#endif
	update_details (self, file);
#ifdef NEMO_SMPL
	if (g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (self->stack)),
		       "info") == 0) {
		gboolean had_error = gtk_widget_get_visible (self->info_error_label);
		show_info_preview (self, file);
		if (had_error)
			gtk_widget_show (self->info_error_label);
	}
#endif
}

void
nemo_preview_pane_set_file (NemoPreviewPane *self,
			    NemoFile        *file)
{
	g_return_if_fail (NEMO_IS_PREVIEW_PANE (self));

	if (self->destroyed)
		return;
	if (file == NULL) {
		nemo_preview_pane_clear (self);
		return;
	}

	cancel_pending_loads (self);
	self->cancellable = g_cancellable_new ();

#ifdef HAVE_GSTREAMER
	stop_video (self);
#endif

	disconnect_file (self);
	g_set_object (&self->current_file, file);

	self->file_changed_id =
		g_signal_connect (file, "changed",
				  G_CALLBACK (file_changed_cb), self);

	update_details (self, file);
	refresh_file_preview (self, file);
}

void
nemo_preview_pane_clear (NemoPreviewPane *self)
{
	g_return_if_fail (NEMO_IS_PREVIEW_PANE (self));

	if (self->destroyed)
		return;
	cancel_pending_loads (self);

	nemo_image_viewer_clear (self->image_viewer);
	nemo_paged_viewer_close_file (self->paged_viewer);

#ifdef HAVE_GSTREAMER
	stop_video (self);
#endif

	disconnect_file (self);
	g_clear_object (&self->current_file);

	clear_details (self);

	/* Reset vpaned position flag so it gets restored on next show */
	self->details_vpaned_set = FALSE;

	nemo_image_viewer_set_show_controls (self->image_viewer, FALSE);
	gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "empty");
}

static void
nemo_preview_pane_shutdown (NemoPreviewPane *self)
{
	if (self->destroyed)
		return;

	self->destroyed = TRUE;
	cancel_pending_loads (self);
	disconnect_file (self);
	g_signal_handlers_disconnect_by_data (self->vpaned, self);
	g_signal_handlers_disconnect_by_data (self->gps_map_event_box, self);
	nemo_image_viewer_clear (self->image_viewer);
	nemo_paged_viewer_close_file (self->paged_viewer);

#ifdef HAVE_GSTREAMER
	stop_video (self);
	g_signal_handlers_disconnect_by_data (self->video_area, self);
	g_signal_handlers_disconnect_by_data (self->play_btn, self);
	g_signal_handlers_disconnect_by_data (self->mute_btn, self);
	g_signal_handlers_disconnect_by_data (self->seek_scale, self);
#endif

	g_clear_object (&self->current_file);
}

static void
nemo_preview_pane_destroy (GtkWidget *widget)
{
	nemo_preview_pane_shutdown (NEMO_PREVIEW_PANE (widget));
	GTK_WIDGET_CLASS (nemo_preview_pane_parent_class)->destroy (widget);
}

static void
nemo_preview_pane_dispose (GObject *object)
{
	nemo_preview_pane_shutdown (NEMO_PREVIEW_PANE (object));
	G_OBJECT_CLASS (nemo_preview_pane_parent_class)->dispose (object);
}

static void
nemo_preview_pane_finalize (GObject *object)
{
#ifdef HAVE_GSTREAMER
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (object);
	g_mutex_clear (&self->frame_mutex);
#endif
	G_OBJECT_CLASS (nemo_preview_pane_parent_class)->finalize (object);
}

static void
nemo_preview_pane_class_init (NemoPreviewPaneClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

	object_class->dispose = nemo_preview_pane_dispose;
	object_class->finalize = nemo_preview_pane_finalize;
	widget_class->destroy = nemo_preview_pane_destroy;
}

static GtkWidget *
create_detail_row (GtkGrid *grid, const gchar *label_text, gint row)
{
	GtkWidget *label;
	GtkWidget *value;

	label = gtk_label_new (label_text);
	gtk_widget_set_halign (label, GTK_ALIGN_END);
	gtk_widget_set_valign (label, GTK_ALIGN_START);
	gtk_style_context_add_class (
		gtk_widget_get_style_context (label), "dim-label");
	gtk_grid_attach (grid, label, 0, row, 1, 1);
	gtk_widget_show (label);

	value = gtk_label_new ("");
	gtk_widget_set_halign (value, GTK_ALIGN_START);
	gtk_widget_set_valign (value, GTK_ALIGN_START);
	gtk_label_set_selectable (GTK_LABEL (value), TRUE);
	gtk_label_set_ellipsize (GTK_LABEL (value), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars (GTK_LABEL (value), 30);
	gtk_grid_attach (grid, value, 1, row, 1, 1);
	gtk_widget_show (value);

	return value;
}

static void
vpaned_size_allocate_cb (GtkWidget    *widget,
			 GtkAllocation *allocation,
			 gpointer      user_data)
{
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (user_data);
	gint saved_height, position;

	if (self->details_vpaned_set) {
		return;
	}

	self->details_vpaned_set = TRUE;

	saved_height = g_settings_get_int (nemo_preview_pane_preferences,
					   "details-height");
	if (saved_height > 50 && allocation->height > saved_height) {
		position = allocation->height - saved_height;
		gtk_paned_set_position (GTK_PANED (widget), position);
	} else {
		gtk_paned_set_position (GTK_PANED (widget),
					(int) (allocation->height * 0.6));
	}
}

static void
vpaned_position_changed_cb (GObject    *paned,
			    GParamSpec *pspec,
			    gpointer    user_data)
{
	NemoPreviewPane *self = NEMO_PREVIEW_PANE (user_data);
	gint position, total_height, details_height;

	if (!self->details_vpaned_set) {
		return;
	}

	position = gtk_paned_get_position (GTK_PANED (paned));
	total_height = gtk_widget_get_allocated_height (GTK_WIDGET (paned));
	details_height = total_height - position;

	if (details_height > 50 && total_height > 0) {
		g_settings_set_int (nemo_preview_pane_preferences,
				    "details-height", details_height);
	}
}

static void
nemo_preview_pane_init (NemoPreviewPane *self)
{
	GtkWidget *header;
	GtkWidget *sep;
	GtkWidget *info_box;
	GtkStyleContext *ctx;
	GtkGrid *grid;

	self->cancellable = g_cancellable_new ();
	self->details_vpaned_set = FALSE;

#ifdef HAVE_GSTREAMER
	g_mutex_init (&self->frame_mutex);
	self->frame_surface = NULL;
	self->video_width = 0;
	self->video_height = 0;
#endif

	gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
				       GTK_ORIENTATION_VERTICAL);
	gtk_widget_set_size_request (GTK_WIDGET (self), 250, -1);

	header = gtk_label_new (_("Preview"));
	ctx = gtk_widget_get_style_context (header);
	gtk_style_context_add_class (ctx, "dim-label");
	gtk_widget_set_margin_top (header, 8);
	gtk_widget_set_margin_bottom (header, 4);
	gtk_box_pack_start (GTK_BOX (self), header, FALSE, FALSE, 0);
	gtk_widget_show (header);

	sep = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
	gtk_box_pack_start (GTK_BOX (self), sep, FALSE, FALSE, 0);
	gtk_widget_show (sep);

	/* Vertical paned: stack on top, details on bottom */
	self->vpaned = gtk_paned_new (GTK_ORIENTATION_VERTICAL);
	gtk_box_pack_start (GTK_BOX (self), self->vpaned, TRUE, TRUE, 0);
	gtk_widget_show (self->vpaned);

	self->stack = gtk_stack_new ();
	gtk_stack_set_transition_type (GTK_STACK (self->stack),
				      GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_set_transition_duration (GTK_STACK (self->stack), 150);
	gtk_paned_pack1 (GTK_PANED (self->vpaned), self->stack, TRUE, FALSE);
	gtk_widget_show (self->stack);

	/* Empty page */
	self->empty_label = gtk_label_new (_("Select a file to preview"));
	ctx = gtk_widget_get_style_context (self->empty_label);
	gtk_style_context_add_class (ctx, "dim-label");
	gtk_widget_set_valign (self->empty_label, GTK_ALIGN_CENTER);
	gtk_stack_add_named (GTK_STACK (self->stack), self->empty_label, "empty");
	gtk_widget_show (self->empty_label);

	/* Image page — shared NemoImageViewer widget */
	self->image_viewer = nemo_image_viewer_new ();
	nemo_image_viewer_set_show_controls (self->image_viewer, FALSE);
	gtk_stack_add_named (GTK_STACK (self->stack),
			     GTK_WIDGET (self->image_viewer), "image");
	gtk_widget_show (GTK_WIDGET (self->image_viewer));

#ifdef HAVE_GSTREAMER
	/* Video page: GStreamer overlay area + transport controls */
	{
		GtkWidget *vctrl;

		self->video_page_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

		self->video_area = gtk_drawing_area_new ();
		gtk_widget_set_app_paintable (self->video_area, TRUE);
		gtk_widget_set_hexpand (self->video_area, TRUE);
		gtk_widget_set_vexpand (self->video_area, TRUE);
		g_signal_connect (self->video_area, "draw",
				  G_CALLBACK (video_area_draw_cb), self);
		gtk_box_pack_start (GTK_BOX (self->video_page_box),
				    self->video_area, TRUE, TRUE, 0);
		gtk_widget_show (self->video_area);

		/* Icon shown instead of video area for audio files */
		self->audio_icon = gtk_image_new ();
		gtk_image_set_pixel_size (GTK_IMAGE (self->audio_icon), 96);
		gtk_widget_set_halign (self->audio_icon, GTK_ALIGN_CENTER);
		gtk_widget_set_valign (self->audio_icon, GTK_ALIGN_CENTER);
		gtk_widget_set_hexpand (self->audio_icon, TRUE);
		gtk_widget_set_vexpand (self->audio_icon, TRUE);
		gtk_box_pack_start (GTK_BOX (self->video_page_box),
				    self->audio_icon, TRUE, TRUE, 0);
		gtk_widget_set_no_show_all (self->audio_icon, TRUE);

		/* Seek bar */
		self->seek_scale = gtk_scale_new_with_range (
			GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 1.0);
		gtk_scale_set_draw_value (GTK_SCALE (self->seek_scale),
					  TRUE);
		gtk_scale_set_value_pos (GTK_SCALE (self->seek_scale),
					 GTK_POS_RIGHT);
		g_signal_connect (self->seek_scale, "format-value",
				  G_CALLBACK (seek_scale_format_cb), self);
		g_signal_connect (self->seek_scale, "button-press-event",
				  G_CALLBACK (seek_press_cb), self);
		g_signal_connect (self->seek_scale, "button-release-event",
				  G_CALLBACK (seek_release_cb), self);
		gtk_widget_set_margin_start (self->seek_scale, 4);
		gtk_widget_set_margin_end (self->seek_scale, 4);
		gtk_box_pack_start (GTK_BOX (self->video_page_box),
				    self->seek_scale, FALSE, FALSE, 0);
		gtk_widget_show (self->seek_scale);

		/* Transport controls (play/pause + mute) */
		vctrl = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
		gtk_widget_set_margin_start (vctrl, 4);
		gtk_widget_set_margin_end (vctrl, 4);
		gtk_widget_set_margin_bottom (vctrl, 4);
		gtk_widget_set_halign (vctrl, GTK_ALIGN_CENTER);

		self->play_btn = gtk_button_new_from_icon_name (
			"media-playback-pause-symbolic",
			GTK_ICON_SIZE_SMALL_TOOLBAR);
		gtk_button_set_relief (GTK_BUTTON (self->play_btn),
				       GTK_RELIEF_NONE);
		gtk_widget_set_tooltip_text (self->play_btn,
					     _("Play / Pause"));
		g_signal_connect (self->play_btn, "clicked",
				  G_CALLBACK (play_pause_clicked_cb), self);
		gtk_box_pack_start (GTK_BOX (vctrl),
				    self->play_btn, FALSE, FALSE, 0);
		gtk_widget_show (self->play_btn);

		self->mute_btn = gtk_button_new_from_icon_name (
			"audio-volume-muted-symbolic",
			GTK_ICON_SIZE_SMALL_TOOLBAR);
		gtk_button_set_relief (GTK_BUTTON (self->mute_btn),
				       GTK_RELIEF_NONE);
		gtk_widget_set_tooltip_text (self->mute_btn,
					     _("Mute / Unmute"));
		g_signal_connect (self->mute_btn, "clicked",
				  G_CALLBACK (mute_clicked_cb), self);
		gtk_box_pack_start (GTK_BOX (vctrl),
				    self->mute_btn, FALSE, FALSE, 0);
		gtk_widget_show (self->mute_btn);

		self->video_muted = TRUE;

		gtk_box_pack_end (GTK_BOX (self->video_page_box),
				  vctrl, FALSE, FALSE, 0);
		gtk_widget_show (vctrl);
	}

	gtk_stack_add_named (GTK_STACK (self->stack),
			     self->video_page_box, "video");
	gtk_widget_show (self->video_page_box);
#endif

	/* Text page — shared NemoPagedViewer (handles files of any size) */
	self->paged_viewer = nemo_paged_viewer_new ();
	gtk_stack_add_named (GTK_STACK (self->stack),
			     GTK_WIDGET (self->paged_viewer), "text");
	gtk_widget_show (GTK_WIDGET (self->paged_viewer));

	/* Info page (icon only, for non-previewable files) */
	info_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_valign (info_box, GTK_ALIGN_CENTER);

	self->info_icon = gtk_image_new ();
	gtk_image_set_pixel_size (GTK_IMAGE (self->info_icon), 64);
	gtk_box_pack_start (GTK_BOX (info_box),
			    self->info_icon, FALSE, FALSE, 0);
	gtk_widget_show (self->info_icon);

	self->info_error_label = gtk_label_new (NULL);
	gtk_label_set_line_wrap (GTK_LABEL (self->info_error_label), TRUE);
	gtk_label_set_max_width_chars (GTK_LABEL (self->info_error_label), 40);
	gtk_label_set_justify (GTK_LABEL (self->info_error_label),
			       GTK_JUSTIFY_CENTER);
	gtk_widget_set_margin_top (self->info_error_label, 12);
	gtk_widget_set_no_show_all (self->info_error_label, TRUE);
	gtk_box_pack_start (GTK_BOX (info_box),
			    self->info_error_label, FALSE, FALSE, 0);

	gtk_stack_add_named (GTK_STACK (self->stack), info_box, "info");
	gtk_widget_show (info_box);

	/* Details panel (bottom of vpaned) */
	self->details_scroll = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (
		GTK_SCROLLED_WINDOW (self->details_scroll),
		GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

	/* Outer HBox: text metadata on left, map on right */
	{
		GtkWidget *details_hbox;

		details_hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_margin_start (details_hbox, 12);
		gtk_widget_set_margin_end (details_hbox, 12);
		gtk_widget_set_margin_top (details_hbox, 12);
		gtk_widget_set_margin_bottom (details_hbox, 12);

		/* Left side: text metadata grid */
		grid = GTK_GRID (gtk_grid_new ());
		gtk_grid_set_row_spacing (grid, 6);
		gtk_grid_set_column_spacing (grid, 12);
		self->details_grid = GTK_WIDGET (grid);

		self->detail_name = create_detail_row (grid, _("Name:"), 0);
		self->detail_size = create_detail_row (grid, _("Size:"), 1);
		self->detail_type = create_detail_row (grid, _("Type:"), 2);
		self->detail_modified = create_detail_row (grid, _("Modified:"), 3);
		self->detail_permissions = create_detail_row (grid, _("Permissions:"), 4);
		self->detail_location = create_detail_row (grid, _("Location:"), 5);
		self->detail_gps = create_detail_row (grid, _("GPS:"), 6);
		self->detail_gps_label = gtk_grid_get_child_at (grid, 0, 6);
		/* GPS row hidden by default, shown only when data is present */
		gtk_widget_set_no_show_all (self->detail_gps, TRUE);
		gtk_widget_set_no_show_all (self->detail_gps_label, TRUE);
		gtk_widget_hide (self->detail_gps);
		gtk_widget_hide (self->detail_gps_label);

		gtk_widget_show (GTK_WIDGET (grid));
		gtk_box_pack_start (GTK_BOX (details_hbox),
				    GTK_WIDGET (grid), TRUE, TRUE, 0);

		/* Right side: GPS map tile */
		self->gps_map_event_box = gtk_event_box_new ();
		gtk_widget_set_halign (self->gps_map_event_box, GTK_ALIGN_END);
		gtk_widget_set_valign (self->gps_map_event_box, GTK_ALIGN_START);
		self->detail_gps_map = gtk_image_new ();
		gtk_widget_set_size_request (self->detail_gps_map,
					     GPS_MAP_SIZE, GPS_MAP_SIZE);
		gtk_container_add (GTK_CONTAINER (self->gps_map_event_box),
				   self->detail_gps_map);
		gtk_widget_show (self->detail_gps_map);
		gtk_widget_set_tooltip_text (self->gps_map_event_box,
					     _("Click to open in map"));
		gtk_widget_set_events (self->gps_map_event_box,
				       GDK_BUTTON_PRESS_MASK);
#ifdef HAVE_EXIF
		g_signal_connect (self->gps_map_event_box,
				  "button-press-event",
				  G_CALLBACK (gps_map_clicked_cb), self);
#endif
		gtk_widget_set_no_show_all (self->gps_map_event_box, TRUE);
		gtk_widget_hide (self->gps_map_event_box);
		gtk_box_pack_end (GTK_BOX (details_hbox),
				  self->gps_map_event_box, FALSE, FALSE, 0);

		gtk_widget_show (details_hbox);
		gtk_container_add (GTK_CONTAINER (self->details_scroll),
				   details_hbox);
	}

	gtk_paned_pack2 (GTK_PANED (self->vpaned),
			 self->details_scroll, FALSE, FALSE);
	/* Details initially hidden, shown when a file is set */
	gtk_widget_hide (self->details_scroll);

	/* Connect signals for saving/restoring vpaned position */
	g_signal_connect (self->vpaned, "size-allocate",
			  G_CALLBACK (vpaned_size_allocate_cb), self);
	g_signal_connect (self->vpaned, "notify::position",
			  G_CALLBACK (vpaned_position_changed_cb), self);

	/* Start with empty page */
	gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "empty");
}

GtkWidget *
nemo_preview_pane_new (void)
{
	return g_object_new (NEMO_TYPE_PREVIEW_PANE, NULL);
}

void
nemo_preview_pane_toggle_details (NemoPreviewPane *self)
{
	g_return_if_fail (NEMO_IS_PREVIEW_PANE (self));

	if (self->destroyed)
		return;

	if (gtk_widget_get_visible (self->details_scroll)) {
		gtk_widget_hide (self->details_scroll);
	} else if (self->current_file != NULL) {
		gtk_widget_show (self->details_scroll);
	}
}

void
nemo_preview_pane_toggle_mute (NemoPreviewPane *self)
{
	g_return_if_fail (NEMO_IS_PREVIEW_PANE (self));

	if (self->destroyed)
		return;

#ifdef HAVE_GSTREAMER
	if (self->mute_btn != NULL) {
		g_signal_emit_by_name (self->mute_btn, "clicked");
	}
#endif
}

void
nemo_preview_pane_toggle_play (NemoPreviewPane *self)
{
	g_return_if_fail (NEMO_IS_PREVIEW_PANE (self));

	if (self->destroyed)
		return;

#ifdef HAVE_GSTREAMER
	if (self->play_btn != NULL) {
		g_signal_emit_by_name (self->play_btn, "clicked");
	}
#endif
}
