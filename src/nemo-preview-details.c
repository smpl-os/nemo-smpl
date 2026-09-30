/* Shared, asynchronous metadata and GPS map for sidebar and quick preview.
 * Copyright (C) 2026 The Nemo contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <config.h>
#include "nemo-preview-details.h"
#include "nemo-preview-utils.h"

#include <libnemo-private/nemo-file.h>
#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

#ifdef HAVE_EXIF
#include <libexif/exif-data.h>
#include <libexif/exif-loader.h>
#include <libexif/exif-utils.h>
#endif
#ifdef HAVE_LIBRAW
#include <libraw/libraw.h>
#endif

#define GPS_MAP_SIZE 150
#define GPS_MAP_TILE_SIZE 256
#define GPS_MAP_ZOOM 15
/* Never download an entire unbounded remote image just to inspect metadata. */
#define METADATA_READ_LIMIT (32 * 1024 * 1024)
#define MAP_READ_LIMIT (2 * 1024 * 1024)
#define DIRECTORY_SIZE_DELAY_MS 250

typedef enum {
	DETAIL_NAME, DETAIL_SIZE, DETAIL_TYPE, DETAIL_MODIFIED,
	DETAIL_PERMISSIONS, DETAIL_LOCATION, DETAIL_CAMERA, DETAIL_LENS,
	DETAIL_TAKEN, DETAIL_DIMENSIONS, DETAIL_APERTURE, DETAIL_SHUTTER,
	DETAIL_ISO, DETAIL_FOCAL, DETAIL_GPS, N_DETAILS
} DetailField;

typedef struct {
	char *values[N_DETAILS];
	char *warning;
	gboolean is_directory;
	gboolean has_gps;
	double latitude, longitude;
} DetailsResult;

struct _NemoPreviewDetails {
	GtkBox parent;
	GtkWidget *labels[N_DETAILS];
	GtkWidget *values[N_DETAILS];
	GtkWidget *status;
	GtkWidget *map_box;
	GtkWidget *map_event;
	GtkWidget *map_image;
	GtkWidget *map_status;
	GFile *file;
	GCancellable *cancellable;
	NemoFile *size_file;
	guint size_timeout_id;
	gboolean recompute_directory_size;
	guint64 generation;
	gboolean destroyed;
	double latitude, longitude;
};

typedef struct {
	GWeakRef details;
	guint64 generation;
	GFile *location;
	char *cache_path;
	double pixel_x, pixel_y;
} DetailsLoadData;

G_DEFINE_TYPE (NemoPreviewDetails, nemo_preview_details, GTK_TYPE_BOX)

static void start_metadata_load (NemoPreviewDetails *self);
static void directory_size_ready_cb (NemoFile *file, gpointer user_data);

static void
details_result_free (DetailsResult *result)
{
	for (guint i = 0; i < N_DETAILS; i++)
		g_free (result->values[i]);
	g_free (result->warning);
	g_free (result);
}

static DetailsLoadData *
details_load_data_new (NemoPreviewDetails *self, GFile *location)
{
	DetailsLoadData *data = g_new0 (DetailsLoadData, 1);

	g_weak_ref_init (&data->details, self);
	data->generation = self->generation;
	data->location = g_object_ref (location);
	return data;
}

static void
details_load_data_free (DetailsLoadData *data)
{
	g_weak_ref_clear (&data->details);
	g_object_unref (data->location);
	g_free (data->cache_path);
	g_free (data);
}

static NemoPreviewDetails *
details_load_get_current (GTask *task)
{
	DetailsLoadData *data = g_task_get_task_data (task);
	NemoPreviewDetails *self = g_weak_ref_get (&data->details);

	if (self != NULL &&
	    (self->destroyed || self->generation != data->generation ||
	     g_cancellable_is_cancelled (g_task_get_cancellable (task)))) {
		g_object_unref (self);
		return NULL;
	}
	return self;
}

static void
details_run_task (GTask *task, GTaskThreadFunc worker)
{
#ifdef NEMO_SMPL
	nemo_preview_run_task (task, worker);
#else
	g_task_run_in_thread (task, worker);
#endif
}

static void
set_field (NemoPreviewDetails *self, guint field, const char *text)
{
	gboolean present = text != NULL && text[0] != '\0';

	gtk_label_set_text (GTK_LABEL (self->values[field]), present ? text : "");
	gtk_widget_set_tooltip_text (self->values[field], present ? text : NULL);
	gtk_widget_set_visible (self->labels[field], present);
	gtk_widget_set_visible (self->values[field], present);
}

static void
reset_details (NemoPreviewDetails *self)
{
	self->generation++;
	self->recompute_directory_size = TRUE;
	if (self->size_timeout_id != 0) {
		g_source_remove (self->size_timeout_id);
		self->size_timeout_id = 0;
	}
	if (self->size_file != NULL) {
		nemo_file_cancel_call_when_ready (self->size_file,
						directory_size_ready_cb, self);
		g_clear_object (&self->size_file);
	}
	if (self->cancellable != NULL) {
		g_cancellable_cancel (self->cancellable);
		g_clear_object (&self->cancellable);
	}
	for (guint i = 0; i < N_DETAILS; i++)
		set_field (self, i, NULL);
	gtk_label_set_text (GTK_LABEL (self->status), "");
	gtk_widget_hide (self->status);
	gtk_label_set_text (GTK_LABEL (self->map_status), "");
	gtk_widget_hide (self->map_status);
	gtk_widget_hide (self->map_box);
	gtk_widget_hide (self->map_event);
	gtk_image_clear (GTK_IMAGE (self->map_image));
	self->latitude = self->longitude = 0;
}

static void
show_error (GtkWidget *label, const char *message, const GError *error)
{
	char *text = g_strdup_printf ("%s\n%s", message, error->message);
	gtk_label_set_text (GTK_LABEL (label), text);
	gtk_widget_show (label);
	g_free (text);
}

#if defined (HAVE_EXIF) || defined (HAVE_LIBRAW)
static gboolean
coordinate_valid (const double values[3], double limit)
{
	for (guint i = 0; i < 3; i++)
		if (!isfinite (values[i]) || values[i] < 0)
			return FALSE;
	return values[1] < 60 && values[2] < 60 &&
	       values[0] + values[1] / 60 + values[2] / 3600 <= limit;
}

static void
set_gps (DetailsResult *result, const double lat[3], const double lon[3],
	 char lat_ref, char lon_ref)
{
	lat_ref = g_ascii_toupper (lat_ref);
	lon_ref = g_ascii_toupper (lon_ref);
	if (!coordinate_valid (lat, 90) || !coordinate_valid (lon, 180) ||
	    (lat_ref != 'N' && lat_ref != 'S') ||
	    (lon_ref != 'E' && lon_ref != 'W'))
		return;

	result->has_gps = TRUE;
	result->latitude = (lat[0] + lat[1] / 60 + lat[2] / 3600) *
			   (lat_ref == 'S' ? -1 : 1);
	result->longitude = (lon[0] + lon[1] / 60 + lon[2] / 3600) *
			    (lon_ref == 'W' ? -1 : 1);
	g_free (result->values[DETAIL_GPS]);
	result->values[DETAIL_GPS] =
		g_strdup_printf ("%.0f°%.0f′%.1f″%c %.0f°%.0f′%.1f″%c",
				 lat[0], lat[1], lat[2], lat_ref,
				 lon[0], lon[1], lon[2], lon_ref);
}

static char *
camera_name (const char *make, const char *model)
{
	if (make == NULL || *make == '\0')
		return g_strdup (model);
	if (model == NULL || *model == '\0')
		return g_strdup (make);
	if (g_ascii_strncasecmp (make, model, strlen (make)) == 0)
		return g_strdup (model);
	return g_strdup_printf ("%s %s", make, model);
}
#endif

#ifdef HAVE_EXIF
static gboolean
read_gps_coordinate (ExifEntry *entry, ExifByteOrder order, double values[3])
{
	if (entry == NULL || entry->data == NULL || entry->size < 24 ||
	    entry->format != EXIF_FORMAT_RATIONAL || entry->components != 3)
		return FALSE;
	for (guint i = 0; i < 3; i++) {
		ExifRational value = exif_get_rational (entry->data + i * 8, order);
		if (value.denominator == 0)
			return FALSE;
		values[i] = (double) value.numerator / value.denominator;
	}
	return TRUE;
}

static char
read_gps_ref (ExifEntry *entry)
{
	if (entry == NULL || entry->data == NULL || entry->size < 2 ||
	    entry->format != EXIF_FORMAT_ASCII || entry->data[1] != '\0')
		return '\0';
	return entry->data[0];
}

static char *
exif_value (ExifData *exif, ExifTag tag)
{
	ExifEntry *entry = exif_data_get_entry (exif, tag);
	char buffer[256] = { 0 };

	if (entry == NULL || entry->data == NULL)
		return NULL;
	exif_entry_get_value (entry, buffer, sizeof buffer);
	g_strstrip (buffer);
	return buffer[0] != '\0' ? g_utf8_make_valid (buffer, -1) : NULL;
}

static void
extract_exif (DetailsResult *result, ExifData *exif)
{
	static const struct { DetailField field; ExifTag tag; } tags[] = {
		{ DETAIL_LENS, EXIF_TAG_LENS_MODEL },
		{ DETAIL_TAKEN, EXIF_TAG_DATE_TIME_ORIGINAL },
		{ DETAIL_APERTURE, EXIF_TAG_FNUMBER },
		{ DETAIL_SHUTTER, EXIF_TAG_EXPOSURE_TIME },
		{ DETAIL_ISO, EXIF_TAG_ISO_SPEED_RATINGS },
		{ DETAIL_FOCAL, EXIF_TAG_FOCAL_LENGTH }
	};
	char *make = exif_value (exif, EXIF_TAG_MAKE);
	char *model = exif_value (exif, EXIF_TAG_MODEL);
	char *width = exif_value (exif, EXIF_TAG_PIXEL_X_DIMENSION);
	char *height = exif_value (exif, EXIF_TAG_PIXEL_Y_DIMENSION);
	ExifContent *gps = exif->ifd[EXIF_IFD_GPS];
	ExifByteOrder order = exif_data_get_byte_order (exif);
	double lat[3], lon[3];

	result->values[DETAIL_CAMERA] = camera_name (make, model);
	g_free (make);
	g_free (model);
	for (guint i = 0; i < G_N_ELEMENTS (tags); i++)
		result->values[tags[i].field] = exif_value (exif, tags[i].tag);
	if (width != NULL && height != NULL &&
	    g_ascii_strtoull (width, NULL, 10) > 0 &&
	    g_ascii_strtoull (height, NULL, 10) > 0)
		result->values[DETAIL_DIMENSIONS] = g_strdup_printf ("%s × %s", width, height);
	g_free (width);
	g_free (height);
	if (read_gps_coordinate (exif_content_get_entry (gps, 2), order, lat) &&
	    read_gps_coordinate (exif_content_get_entry (gps, 4), order, lon))
		set_gps (result, lat, lon,
			 read_gps_ref (exif_content_get_entry (gps, 1)),
			 read_gps_ref (exif_content_get_entry (gps, 3)));
}

static gboolean
load_exif (GFile *file, DetailsResult *result, GCancellable *cancel, GError **error)
{
	GFileInputStream *stream = g_file_read (file, cancel, error);
	ExifLoader *loader;
	ExifData *exif;
	guchar buffer[16384];
	gsize total = 0;
	gssize count;

	if (stream == NULL)
		return FALSE;
	loader = exif_loader_new ();
	if (loader == NULL) {
		g_object_unref (stream);
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
				     _("Unable to read image metadata."));
		return FALSE;
	}
	while (total < METADATA_READ_LIMIT &&
	       (count = g_input_stream_read (G_INPUT_STREAM (stream), buffer,
					    MIN (sizeof buffer, METADATA_READ_LIMIT - total),
					    cancel, error)) > 0) {
		total += count;
		if (!exif_loader_write (loader, buffer, count))
			break;
	}
	exif = *error == NULL ? exif_loader_get_data (loader) : NULL;
	if (exif != NULL) {
		extract_exif (result, exif);
		exif_data_unref (exif);
	}
	exif_loader_unref (loader);
	g_object_unref (stream);
	return *error == NULL;
}
#endif

/* Remote metadata and tile reads are bounded and cancellable. */
static GByteArray *
read_prefix (GFile *file, gsize limit, GCancellable *cancel, GError **error)
{
	GFileInputStream *stream = g_file_read (file, cancel, error);
	GByteArray *bytes;
	guchar buffer[16384];
	gssize count;

	if (stream == NULL)
		return NULL;
	bytes = g_byte_array_new ();
	while (bytes->len < limit &&
	       (count = g_input_stream_read (G_INPUT_STREAM (stream), buffer,
					    MIN (sizeof buffer, limit - bytes->len),
					    cancel, error)) > 0)
		g_byte_array_append (bytes, buffer, count);
	g_object_unref (stream);
	if (*error != NULL) {
		g_byte_array_unref (bytes);
		return NULL;
	}
	return bytes;
}

#ifdef HAVE_LIBRAW
static int
metadata_raw_progress (void *data, enum LibRaw_progress stage, int iteration, int expected)
{
	return g_cancellable_is_cancelled (data) ? 1 : 0;
}

static void
extract_raw (DetailsResult *result, libraw_data_t *raw)
{
	libraw_imgother_t *other = &raw->other;
	libraw_gps_info_t *gps = &other->parsed_gps;
	char *make = g_strndup (raw->idata.make, sizeof raw->idata.make);
	char *model = g_strndup (raw->idata.model, sizeof raw->idata.model);
	double lat[3], lon[3];

	result->values[DETAIL_CAMERA] = camera_name (g_strstrip (make), g_strstrip (model));
	g_free (make);
	g_free (model);
	result->values[DETAIL_LENS] = g_strndup (raw->lens.Lens, sizeof raw->lens.Lens);
	if (*result->values[DETAIL_LENS] == '\0') {
		g_free (result->values[DETAIL_LENS]);
		result->values[DETAIL_LENS] = g_strndup (raw->lens.makernotes.Lens,
						       sizeof raw->lens.makernotes.Lens);
	}
	if (raw->sizes.width > 0 && raw->sizes.height > 0)
		result->values[DETAIL_DIMENSIONS] =
			g_strdup_printf ("%u × %u", raw->sizes.width, raw->sizes.height);
	if (other->timestamp > 0) {
		GDateTime *date = g_date_time_new_from_unix_local (other->timestamp);
		if (date != NULL) {
			result->values[DETAIL_TAKEN] = g_date_time_format (date, "%Y-%m-%d %H:%M:%S");
			g_date_time_unref (date);
		}
	}
	if (isfinite (other->aperture) && other->aperture > 0)
		result->values[DETAIL_APERTURE] = g_strdup_printf ("f/%.1f", other->aperture);
	if (isfinite (other->shutter) && other->shutter > 0)
		result->values[DETAIL_SHUTTER] = other->shutter < 1 ?
			g_strdup_printf ("1/%.0f s", 1.0 / other->shutter) :
			g_strdup_printf ("%.2f s", other->shutter);
	if (isfinite (other->iso_speed) && other->iso_speed > 0)
		result->values[DETAIL_ISO] = g_strdup_printf ("%.0f", other->iso_speed);
	if (isfinite (other->focal_len) && other->focal_len > 0)
		result->values[DETAIL_FOCAL] = g_strdup_printf ("%.1f mm", other->focal_len);
	if (gps->gpsparsed) {
		for (guint i = 0; i < 3; i++) {
			lat[i] = gps->latitude[i];
			lon[i] = gps->longitude[i];
		}
		set_gps (result, lat, lon, gps->latref, gps->longref);
	}
}

static gboolean
load_raw (GFile *file, DetailsResult *result, GCancellable *cancel, GError **error)
{
	GByteArray *bytes = NULL;
	libraw_data_t *raw;
	char *path;
	int status;
	gboolean limited = FALSE;
	gboolean success = FALSE;

	if (g_cancellable_set_error_if_cancelled (cancel, error))
		return FALSE;
	raw = libraw_init (0);
	if (raw == NULL) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
				     _("Unable to read image metadata."));
		return FALSE;
	}
	libraw_set_progress_handler (raw, metadata_raw_progress, cancel);
	path = g_file_get_path (file);
	/* Metadata IFDs need not precede pixel data. Native files can seek to
	 * them without reading the whole file; neither path unpacks pixels. */
	if (path != NULL) {
		status = libraw_open_file (raw, path);
	} else {
		/* One look-ahead byte distinguishes a complete file from a prefix. */
		bytes = read_prefix (file, METADATA_READ_LIMIT + 1, cancel, error);
		if (bytes == NULL)
			goto out;
		limited = bytes->len > METADATA_READ_LIMIT;
		if (limited)
			g_byte_array_set_size (bytes, METADATA_READ_LIMIT);
		status = bytes->len > 0 ? libraw_open_buffer (raw, bytes->data, bytes->len) :
					 LIBRAW_FILE_UNSUPPORTED;
	}
	if (g_cancellable_set_error_if_cancelled (cancel, error))
		goto out;
	if (status == LIBRAW_SUCCESS)
		extract_raw (result, raw);
	if (limited) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
			     _("Remote RAW metadata may be incomplete (inspection limited to %u MiB)."),
			     METADATA_READ_LIMIT / (1024 * 1024));
	} else if (status != LIBRAW_SUCCESS) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			     _("Unable to decode RAW metadata: %s"), libraw_strerror (status));
	} else
		success = TRUE;
out:
	libraw_close (raw);
	if (bytes != NULL)
		g_byte_array_unref (bytes);
	g_free (path);
	return success;
}
#endif

static void
metadata_worker (GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
	DetailsLoadData *data = task_data;
	DetailsResult *result = g_new0 (DetailsResult, 1);
	GError *error = NULL;
	GFileInfo *info;
	GFile *parent;
	const char *mime;
#if defined (HAVE_EXIF) || defined (HAVE_LIBRAW)
	gboolean raw;
#endif

	info = g_file_query_info (data->location,
		G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
		G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
		G_FILE_ATTRIBUTE_STANDARD_TYPE ","
		G_FILE_ATTRIBUTE_STANDARD_SIZE ","
		G_FILE_ATTRIBUTE_STANDARD_TARGET_URI ","
		G_FILE_ATTRIBUTE_TIME_MODIFIED ","
		G_FILE_ATTRIBUTE_UNIX_MODE,
		G_FILE_QUERY_INFO_NONE, cancel, &error);
	if (info == NULL) {
		details_result_free (result);
		g_task_return_error (task, error);
		return;
	}
	result->values[DETAIL_NAME] = g_strdup (g_file_info_get_display_name (info));
	result->is_directory = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
	if (result->is_directory)
		result->values[DETAIL_SIZE] = g_strdup (_("Calculating…"));
	else if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_SIZE))
		result->values[DETAIL_SIZE] = g_format_size (g_file_info_get_size (info));
	mime = g_file_info_get_content_type (info);
	if (mime != NULL)
		result->values[DETAIL_TYPE] = g_content_type_get_description (mime);
	if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
		guint64 modified = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
		GDateTime *date = modified <= G_MAXINT64 ? g_date_time_new_from_unix_local (modified) : NULL;
		if (date != NULL) {
			result->values[DETAIL_MODIFIED] = g_date_time_format (date, "%c");
			g_date_time_unref (date);
		}
	}
	if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
		guint32 mode = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE);
		char permissions[11] = "----------";
		const char *rwx = "rwxrwxrwx";
		permissions[0] = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY ? 'd' : '-';
		for (guint i = 0; i < 9; i++)
			if (mode & (1 << (8 - i)))
				permissions[i + 1] = rwx[i];
		if (mode & 04000) permissions[3] = mode & 0100 ? 's' : 'S';
		if (mode & 02000) permissions[6] = mode & 0010 ? 's' : 'S';
		if (mode & 01000) permissions[9] = mode & 0001 ? 't' : 'T';
		result->values[DETAIL_PERMISSIONS] = g_strdup (permissions);
	}
	if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI)) {
		GFile *target = g_file_new_for_uri (g_file_info_get_attribute_string (
			info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI));
		parent = g_file_get_parent (target);
		g_object_unref (target);
	} else
		parent = g_file_get_parent (data->location);
	if (parent != NULL) {
		result->values[DETAIL_LOCATION] = g_file_get_parse_name (parent);
		g_object_unref (parent);
	}
#if defined (HAVE_EXIF) || defined (HAVE_LIBRAW)
	raw = nemo_preview_mime_is_raw_image (mime) || nemo_preview_file_is_raw (data->location);
	if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR) {
#ifdef HAVE_LIBRAW
		if (raw)
			load_raw (data->location, result, cancel, &error);
#endif
#ifdef HAVE_EXIF
		if (!raw && nemo_preview_mime_is_image (mime))
			load_exif (data->location, result, cancel, &error);
#endif
	}
#endif
	g_object_unref (info);
	if (error != NULL) {
		if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			details_result_free (result);
			g_task_return_error (task, error);
			return;
		}
		result->warning = g_strdup_printf ("%s\n%s", _("Unable to read image metadata."),
						   error->message);
		g_clear_error (&error);
	}
	g_task_return_pointer (task, result, (GDestroyNotify) details_result_free);
}

static void
gps_to_tile (double lat, double lon, int zoom, int *tile_x, int *tile_y,
	     double *pixel_x, double *pixel_y)
{
	double n = pow (2.0, zoom);
	double lat_rad = CLAMP (lat, -85.05112878, 85.05112878) * G_PI / 180.0;
	double tx = (lon + 180.0) / 360.0 * n;
	double ty = (1.0 - log (tan (lat_rad) + 1.0 / cos (lat_rad)) / G_PI) / 2.0 * n;

	*tile_x = CLAMP ((int) floor (tx), 0, (int) n - 1);
	*tile_y = CLAMP ((int) floor (ty), 0, (int) n - 1);
	*pixel_x = CLAMP ((tx - *tile_x) * GPS_MAP_TILE_SIZE, 0, GPS_MAP_TILE_SIZE - 1);
	*pixel_y = CLAMP ((ty - *tile_y) * GPS_MAP_TILE_SIZE, 0, GPS_MAP_TILE_SIZE - 1);
}

static char *
gps_map_cache_path (int zoom, int x, int y)
{
	char *name = g_strdup_printf ("%d_%d_%d.png", zoom, x, y);
	char *path = g_build_filename (g_get_user_cache_dir (), "nemo", "map-tiles", name, NULL);
	g_free (name);
	return path;
}

static void
gps_map_render_tile (NemoPreviewDetails *self, GdkPixbuf *tile, double px, double py)
{
	int x = CLAMP ((int) (px - GPS_MAP_SIZE / 2.0), 0, GPS_MAP_TILE_SIZE - GPS_MAP_SIZE);
	int y = CLAMP ((int) (py - GPS_MAP_SIZE / 2.0), 0, GPS_MAP_TILE_SIZE - GPS_MAP_SIZE);
	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
							      GPS_MAP_SIZE, GPS_MAP_SIZE);
	cairo_t *cr = cairo_create (surface);
	GdkPixbuf *image;

	gdk_cairo_set_source_pixbuf (cr, tile, -x, -y);
	cairo_paint (cr);
	for (guint outline = 0; outline < 2; outline++) {
		double cx = px - x, cy = py - y;
		if (outline == 0)
			cairo_set_source_rgba (cr, 1, 1, 1, 0.9);
		else
			cairo_set_source_rgba (cr, 0.9, 0.1, 0.1, 0.95);
		cairo_set_line_width (cr, outline == 0 ? 3.5 : 2);
		cairo_arc (cr, cx, cy, 9, 0, 2 * G_PI);
		cairo_stroke (cr);
		cairo_move_to (cr, cx, cy - 16);
		cairo_line_to (cr, cx, cy - 6);
		cairo_move_to (cr, cx, cy + 6);
		cairo_line_to (cr, cx, cy + 16);
		cairo_move_to (cr, cx - 16, cy);
		cairo_line_to (cr, cx - 6, cy);
		cairo_move_to (cr, cx + 6, cy);
		cairo_line_to (cr, cx + 16, cy);
		cairo_stroke (cr);
	}
	cairo_arc (cr, px - x, py - y, 3, 0, 2 * G_PI);
	cairo_fill (cr);
	cairo_destroy (cr);
	image = gdk_pixbuf_get_from_surface (surface, 0, 0, GPS_MAP_SIZE, GPS_MAP_SIZE);
	cairo_surface_destroy (surface);
	if (image != NULL) {
		gtk_image_set_from_pixbuf (GTK_IMAGE (self->map_image), image);
		gtk_widget_show (self->map_event);
		gtk_widget_show (self->map_box);
		g_object_unref (image);
	}
}

static GdkPixbuf *
read_map_tile (GFile *file, GCancellable *cancel, GError **error)
{
	GByteArray *bytes = read_prefix (file, MAP_READ_LIMIT, cancel, error);
	GInputStream *stream;
	GdkPixbuf *pixbuf;

	if (bytes == NULL)
		return NULL;
	stream = g_memory_input_stream_new_from_data (bytes->data, bytes->len, NULL);
	pixbuf = gdk_pixbuf_new_from_stream_at_scale (stream, GPS_MAP_TILE_SIZE,
						     GPS_MAP_TILE_SIZE, FALSE, cancel, error);
	g_object_unref (stream);
	g_byte_array_unref (bytes);
	return pixbuf;
}

static void
map_tile_worker (GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
	DetailsLoadData *data = task_data;
	GFile *cache = g_file_new_for_path (data->cache_path);
	GError *error = NULL;
	GdkPixbuf *pixbuf = read_map_tile (cache, cancel, &error);

	if (pixbuf == NULL && !g_cancellable_is_cancelled (cancel)) {
		g_clear_error (&error);
		pixbuf = read_map_tile (data->location, cancel, &error);
		if (pixbuf != NULL) {
			GFile *dir = g_file_get_parent (cache);
			char *buffer = NULL;
			gsize length;

			if (!g_file_make_directory_with_parents (dir, cancel, &error) &&
			    g_error_matches (error, G_IO_ERROR, G_IO_ERROR_EXISTS))
				g_clear_error (&error);
			g_object_unref (dir);
			if (error == NULL &&
			    gdk_pixbuf_save_to_buffer (pixbuf, &buffer, &length, "png", &error, NULL))
				g_file_replace_contents (cache, buffer, length, NULL, FALSE,
							 G_FILE_CREATE_PRIVATE |
							 G_FILE_CREATE_REPLACE_DESTINATION,
							 NULL, cancel, &error);
			g_free (buffer);
			/* A cache write failure must not hide a successfully fetched map. */
			if (error != NULL &&
			    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
				g_warning ("Preview details: cannot cache GPS map: %s", error->message);
			g_clear_error (&error);
		}
	}
	g_object_unref (cache);
	if (pixbuf != NULL)
		g_task_return_pointer (task, pixbuf, g_object_unref);
	else
		g_task_return_error (task, error);
}

static void
map_tile_ready_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	GTask *task = G_TASK (result);
	DetailsLoadData *data = g_task_get_task_data (task);
	NemoPreviewDetails *self = details_load_get_current (task);
	GError *error = NULL;
	GdkPixbuf *pixbuf = g_task_propagate_pointer (task, &error);

	if (self != NULL) {
		gtk_widget_hide (self->map_status);
		if (pixbuf != NULL)
			gps_map_render_tile (self, pixbuf, data->pixel_x, data->pixel_y);
		else if (error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			show_error (self->map_status, _("Unable to load location map."), error);
		g_object_unref (self);
	}
	g_clear_object (&pixbuf);
	g_clear_error (&error);
}

static void
gps_map_fetch_tile (NemoPreviewDetails *self)
{
	int x, y;
	double px, py;
	char *uri;
	GFile *file;
	DetailsLoadData *data;
	GTask *task;

	gps_to_tile (self->latitude, self->longitude, GPS_MAP_ZOOM, &x, &y, &px, &py);
	uri = g_strdup_printf ("https://tile.openstreetmap.org/%d/%d/%d.png", GPS_MAP_ZOOM, x, y);
	file = g_file_new_for_uri (uri);
	g_free (uri);
	data = details_load_data_new (self, file);
	data->cache_path = gps_map_cache_path (GPS_MAP_ZOOM, x, y);
	data->pixel_x = px;
	data->pixel_y = py;
	task = g_task_new (NULL, self->cancellable, map_tile_ready_cb, NULL);
	g_task_set_priority (task, G_PRIORITY_LOW);
	g_task_set_task_data (task, data, (GDestroyNotify) details_load_data_free);
	gtk_label_set_text (GTK_LABEL (self->map_status), _("Loading location map…"));
	gtk_widget_show (self->map_status);
	details_run_task (task, map_tile_worker);
	g_object_unref (task);
	g_object_unref (file);
}

static void
directory_size_ready_cb (NemoFile *file, gpointer user_data)
{
	NemoPreviewDetails *self = user_data;
	guint unreadable;
	goffset size;
	NemoRequestStatus status;
	char *formatted;

	status = nemo_file_get_deep_counts (file, NULL, NULL, &unreadable,
					   NULL, &size, TRUE);
	if (!nemo_file_is_directory (file) || status != NEMO_REQUEST_DONE) {
		set_field (self, DETAIL_SIZE, _("Unknown"));
		gtk_label_set_text (GTK_LABEL (self->status),
				    _("Unable to calculate folder size."));
		gtk_widget_show (self->status);
		return;
	}
	formatted = g_format_size (size);
	if (unreadable != 0) {
		char *partial = g_strdup_printf (_("At least %s"), formatted);
		set_field (self, DETAIL_SIZE, partial);
		g_free (partial);
		gtk_label_set_text (GTK_LABEL (self->status),
				    _("Some folder contents could not be read."));
		gtk_widget_show (self->status);
	} else
		set_field (self, DETAIL_SIZE, formatted);
	g_free (formatted);
}

static gboolean
start_directory_size (gpointer user_data)
{
	NemoPreviewDetails *self = user_data;

	self->size_timeout_id = 0;
	self->size_file = nemo_file_get (self->file);
	if (self->recompute_directory_size) {
		self->recompute_directory_size = FALSE;
		nemo_file_recompute_deep_counts (self->size_file);
	}
	nemo_file_call_when_ready (self->size_file,
				  NEMO_FILE_ATTRIBUTE_INFO | NEMO_FILE_ATTRIBUTE_DEEP_COUNTS,
				  directory_size_ready_cb, self);
	return G_SOURCE_REMOVE;
}

static void
metadata_ready_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	GTask *task = G_TASK (result);
	NemoPreviewDetails *self = details_load_get_current (task);
	GError *error = NULL;
	DetailsResult *metadata = g_task_propagate_pointer (task, &error);

	if (self != NULL) {
		gtk_widget_hide (self->status);
		if (metadata != NULL) {
			for (guint i = 0; i < N_DETAILS; i++) {
				char *valid = metadata->values[i] != NULL ?
					g_utf8_make_valid (metadata->values[i], -1) : NULL;
				set_field (self, i, valid);
				g_free (valid);
			}
			if (metadata->warning != NULL) {
				gtk_label_set_text (GTK_LABEL (self->status), metadata->warning);
				gtk_widget_show (self->status);
			}
			if (metadata->is_directory)
				self->size_timeout_id = g_timeout_add_full (
					G_PRIORITY_LOW, DIRECTORY_SIZE_DELAY_MS,
					start_directory_size, self, NULL);
			if (metadata->has_gps) {
				self->latitude = metadata->latitude;
				self->longitude = metadata->longitude;
				gps_map_fetch_tile (self);
			}
		} else if (error != NULL && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			show_error (self->status, _("Unable to read file information."), error);
		g_object_unref (self);
	}
	if (metadata != NULL)
		details_result_free (metadata);
	g_clear_error (&error);
}

static void
start_metadata_load (NemoPreviewDetails *self)
{
	GTask *task;

	self->cancellable = g_cancellable_new ();
	task = g_task_new (NULL, self->cancellable, metadata_ready_cb, NULL);
	g_task_set_priority (task, G_PRIORITY_LOW);
	g_task_set_task_data (task, details_load_data_new (self, self->file),
			     (GDestroyNotify) details_load_data_free);
	gtk_label_set_text (GTK_LABEL (self->status), _("Loading information…"));
	gtk_widget_show (self->status);
	details_run_task (task, metadata_worker);
	g_object_unref (task);
}

void
nemo_preview_details_set_file (NemoPreviewDetails *self, GFile *file)
{
	gboolean recompute;

	g_return_if_fail (NEMO_IS_PREVIEW_DETAILS (self));
	g_return_if_fail (file == NULL || G_IS_FILE (file));
	if (self->destroyed)
		return;
	/* Deep-count completion emits NemoFile::changed. A metadata refresh for
	 * the same selection must not invalidate the count that just finished. */
	recompute = self->recompute_directory_size ||
		    self->file == NULL || file == NULL || self->cancellable == NULL ||
		    !g_file_equal (self->file, file);
	reset_details (self);
	self->recompute_directory_size = recompute;
	g_set_object (&self->file, file);
	if (file != NULL)
		start_metadata_load (self);
}

void
nemo_preview_details_clear (NemoPreviewDetails *self)
{
	g_return_if_fail (NEMO_IS_PREVIEW_DETAILS (self));
	if (self->destroyed)
		return;
	reset_details (self);
	g_clear_object (&self->file);
}

static gboolean
gps_map_clicked_cb (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	NemoPreviewDetails *self = user_data;
	GtkWidget *toplevel = gtk_widget_get_toplevel (widget);
	char lat[G_ASCII_DTOSTR_BUF_SIZE], lon[G_ASCII_DTOSTR_BUF_SIZE];
	char *uri;
	GError *error = NULL;

	if (event->type != GDK_BUTTON_PRESS || event->button != 1 || self->destroyed)
		return FALSE;
	g_ascii_dtostr (lat, sizeof lat, self->latitude);
	g_ascii_dtostr (lon, sizeof lon, self->longitude);
	uri = g_strdup_printf ("https://www.openstreetmap.org/?mlat=%s&mlon=%s#map=16/%s/%s",
			       lat, lon, lat, lon);
	if (!gtk_show_uri_on_window (GTK_IS_WINDOW (toplevel) ? GTK_WINDOW (toplevel) : NULL,
				    uri, event->time, &error)) {
		show_error (self->map_status, _("Unable to open location map."), error);
		g_clear_error (&error);
	}
	g_free (uri);
	return TRUE;
}

static void
nemo_preview_details_map (GtkWidget *widget)
{
	NemoPreviewDetails *self = NEMO_PREVIEW_DETAILS (widget);
	GTK_WIDGET_CLASS (nemo_preview_details_parent_class)->map (widget);
	if (!self->destroyed && self->file != NULL && self->cancellable == NULL)
		start_metadata_load (self);
}

static void
nemo_preview_details_unmap (GtkWidget *widget)
{
	NemoPreviewDetails *self = NEMO_PREVIEW_DETAILS (widget);
	if (!self->destroyed)
		reset_details (self);
	GTK_WIDGET_CLASS (nemo_preview_details_parent_class)->unmap (widget);
}

static void
nemo_preview_details_hide (GtkWidget *widget)
{
	NemoPreviewDetails *self = NEMO_PREVIEW_DETAILS (widget);
	if (!self->destroyed)
		reset_details (self);
	GTK_WIDGET_CLASS (nemo_preview_details_parent_class)->hide (widget);
}

static void
details_shutdown (NemoPreviewDetails *self)
{
	if (self->destroyed)
		return;
	reset_details (self);
	self->destroyed = TRUE;
	g_clear_object (&self->file);
}

static void
nemo_preview_details_destroy (GtkWidget *widget)
{
	details_shutdown (NEMO_PREVIEW_DETAILS (widget));
	GTK_WIDGET_CLASS (nemo_preview_details_parent_class)->destroy (widget);
}

static void
nemo_preview_details_dispose (GObject *object)
{
	details_shutdown (NEMO_PREVIEW_DETAILS (object));
	G_OBJECT_CLASS (nemo_preview_details_parent_class)->dispose (object);
}

static void
nemo_preview_details_class_init (NemoPreviewDetailsClass *klass)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
	G_OBJECT_CLASS (klass)->dispose = nemo_preview_details_dispose;
	widget_class->destroy = nemo_preview_details_destroy;
	widget_class->map = nemo_preview_details_map;
	widget_class->unmap = nemo_preview_details_unmap;
	widget_class->hide = nemo_preview_details_hide;
}

static GtkWidget *
new_status_label (void)
{
	GtkWidget *label = gtk_label_new ("");
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_max_width_chars (GTK_LABEL (label), 40);
	gtk_label_set_xalign (GTK_LABEL (label), 0);
	gtk_widget_set_no_show_all (label, TRUE);
	return label;
}

static void
nemo_preview_details_init (NemoPreviewDetails *self)
{
	const char *titles[N_DETAILS] = {
		_("Name:"), _("Size:"), _("Type:"), _("Modified:"), _("Permissions:"),
		_("Location:"), _("Camera:"), _("Lens:"), _("Date taken:"), _("Dimensions:"),
		_("Aperture:"), _("Shutter:"), _("ISO:"), _("Focal length:"), _("GPS:")
	};
	GtkWidget *column = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
	GtkWidget *grid = gtk_grid_new ();
	GtkWidget *attribution;

	gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_HORIZONTAL);
	gtk_box_set_spacing (GTK_BOX (self), 8);
	gtk_widget_set_margin_start (GTK_WIDGET (self), 12);
	gtk_widget_set_margin_end (GTK_WIDGET (self), 12);
	gtk_widget_set_margin_top (GTK_WIDGET (self), 12);
	gtk_widget_set_margin_bottom (GTK_WIDGET (self), 12);
	gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
	gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
	for (guint i = 0; i < N_DETAILS; i++) {
		self->labels[i] = gtk_label_new (titles[i]);
		self->values[i] = gtk_label_new ("");
		gtk_widget_set_halign (self->labels[i], GTK_ALIGN_END);
		gtk_widget_set_valign (self->labels[i], GTK_ALIGN_START);
		gtk_style_context_add_class (gtk_widget_get_style_context (self->labels[i]), "dim-label");
		gtk_widget_set_halign (self->values[i], GTK_ALIGN_START);
		gtk_widget_set_valign (self->values[i], GTK_ALIGN_START);
		gtk_label_set_selectable (GTK_LABEL (self->values[i]), TRUE);
		gtk_label_set_ellipsize (GTK_LABEL (self->values[i]), PANGO_ELLIPSIZE_MIDDLE);
		gtk_label_set_max_width_chars (GTK_LABEL (self->values[i]), 30);
		gtk_widget_set_no_show_all (self->labels[i], TRUE);
		gtk_widget_set_no_show_all (self->values[i], TRUE);
		gtk_grid_attach (GTK_GRID (grid), self->labels[i], 0, i, 1, 1);
		gtk_grid_attach (GTK_GRID (grid), self->values[i], 1, i, 1, 1);
	}
	gtk_box_pack_start (GTK_BOX (column), grid, FALSE, FALSE, 0);
	self->status = new_status_label ();
	self->map_status = new_status_label ();
	gtk_box_pack_start (GTK_BOX (column), self->status, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (column), self->map_status, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (self), column, TRUE, TRUE, 0);
	self->map_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_valign (self->map_box, GTK_ALIGN_START);
	gtk_widget_set_no_show_all (self->map_box, TRUE);
	self->map_event = gtk_event_box_new ();
	gtk_widget_set_no_show_all (self->map_event, TRUE);
	self->map_image = gtk_image_new ();
	gtk_widget_set_size_request (self->map_image, GPS_MAP_SIZE, GPS_MAP_SIZE);
	gtk_container_add (GTK_CONTAINER (self->map_event), self->map_image);
	gtk_widget_set_tooltip_text (self->map_event, _("Click to open in map"));
	gtk_widget_add_events (self->map_event, GDK_BUTTON_PRESS_MASK);
	g_signal_connect (self->map_event, "button-press-event", G_CALLBACK (gps_map_clicked_cb), self);
	gtk_box_pack_start (GTK_BOX (self->map_box), self->map_event, FALSE, FALSE, 0);
	attribution = gtk_label_new (NULL);
	gtk_label_set_markup (GTK_LABEL (attribution),
			     "<small>© <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap</a>\ncontributors</small>");
	gtk_box_pack_start (GTK_BOX (self->map_box), attribution, FALSE, FALSE, 0);
	gtk_box_pack_end (GTK_BOX (self), self->map_box, FALSE, FALSE, 0);
	gtk_widget_show_all (column);
	gtk_widget_show (self->map_image);
	gtk_widget_show (attribution);
}

NemoPreviewDetails *
nemo_preview_details_new (void)
{
	return g_object_new (NEMO_TYPE_PREVIEW_DETAILS, NULL);
}
