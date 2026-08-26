/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */

/*
 * nemo-image-viewer.c — Reusable image-preview widget
 *
 * Provides a scrollable, zoomable image display that handles animated
 * GIFs (via GdkPixbufAnimation) and static images alike.  Both the
 * sidebar preview pane and the F3 quick-preview window embed this
 * widget, so the animation, zoom, and fit-to-container logic lives
 * in one place.
 *
 * Copyright (C) 2026 smplOS contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 */

#include <config.h>
#include "nemo-image-viewer.h"

#include <glib/gi18n.h>
#include <string.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#ifdef HAVE_LIBRAW
#include <libraw/libraw.h>
#endif

/* ------------------------------------------------------------------ */
/* Private data                                                       */
/* ------------------------------------------------------------------ */

struct _NemoImageViewer
{
	GtkBox               parent;

	/* Widgets */
	GtkWidget           *scroll;      /* GtkScrolledWindow */
	GtkWidget           *image;       /* GtkImage          */
	GtkWidget           *ctrl_box;    /* zoom controls     */
	GtkWidget           *zoom_scale;
	GtkWidget           *fit_check;

	/* Pixbuf data */
	GdkPixbuf           *original_pixbuf;
	GdkPixbufAnimation  *animation;
	GdkPixbufAnimationIter *anim_iter;
	guint                anim_timeout_id;
	gboolean             is_animated;

	/* Zoom state */
	double               zoom_level;
	gboolean             fit_to_container;
	gboolean             show_controls;
};

G_DEFINE_TYPE (NemoImageViewer, nemo_image_viewer, GTK_TYPE_BOX)

/* Forward declarations */
static void apply_zoom             (NemoImageViewer *self);
static void zoom_scale_changed_cb  (GtkRange *range, gpointer user_data);

/* ------------------------------------------------------------------ */
/* Animation helpers                                                  */
/* ------------------------------------------------------------------ */

static void
stop_animation (NemoImageViewer *self)
{
	if (self->anim_timeout_id != 0) {
		g_source_remove (self->anim_timeout_id);
		self->anim_timeout_id = 0;
	}
	g_clear_object (&self->anim_iter);
}

static void
clear_image_data (NemoImageViewer *self)
{
	stop_animation (self);
	g_clear_object (&self->animation);
	g_clear_object (&self->original_pixbuf);
	self->is_animated = FALSE;
}

static GdkPixbuf *
get_base_pixbuf (NemoImageViewer *self)
{
	if (self->is_animated && self->anim_iter != NULL)
		return gdk_pixbuf_animation_iter_get_pixbuf (self->anim_iter);
	return self->original_pixbuf;
}

static gboolean
anim_advance_cb (gpointer user_data)
{
	NemoImageViewer *self = NEMO_IMAGE_VIEWER (user_data);
	GTimeVal tv;
	int delay;

	if (self->anim_iter == NULL) {
		self->anim_timeout_id = 0;
		return G_SOURCE_REMOVE;
	}

	g_get_current_time (&tv);
	gdk_pixbuf_animation_iter_advance (self->anim_iter, &tv);
	apply_zoom (self);

	delay = gdk_pixbuf_animation_iter_get_delay_time (self->anim_iter);
	if (delay < 0)
		delay = 100;

	self->anim_timeout_id = g_timeout_add (delay, anim_advance_cb, self);
	return G_SOURCE_REMOVE;
}

static void
start_animation (NemoImageViewer *self)
{
	GTimeVal tv;
	int delay;

	g_get_current_time (&tv);

	self->anim_iter = gdk_pixbuf_animation_get_iter (self->animation, &tv);
	if (self->anim_iter == NULL)
		return;

	apply_zoom (self);

	delay = gdk_pixbuf_animation_iter_get_delay_time (self->anim_iter);
	if (delay < 0)
		delay = 100;

	self->anim_timeout_id = g_timeout_add (delay, anim_advance_cb, self);
}

/* ------------------------------------------------------------------ */
/* Zoom helpers                                                       */
/* ------------------------------------------------------------------ */

static double
compute_fit_zoom (NemoImageViewer *self, int image_width, int image_height)
{
	int avail_w, avail_h;
	double zw = 1.0, zh = 1.0, z;

	avail_w = gtk_widget_get_allocated_width  (self->scroll);
	avail_h = gtk_widget_get_allocated_height (self->scroll);

	if (avail_w > 20 && image_width  > avail_w - 20)
		zw = (double) (avail_w - 20) / image_width;
	if (avail_h > 20 && image_height > avail_h - 20)
		zh = (double) (avail_h - 20) / image_height;

	z = (zw < zh) ? zw : zh;
	if (z > 1.0) z = 1.0;
	return z;
}

static void
update_zoom_scale_quietly (NemoImageViewer *self, double value)
{
	g_signal_handlers_block_by_func (self->zoom_scale,
					 zoom_scale_changed_cb, self);
	gtk_range_set_value (GTK_RANGE (self->zoom_scale), value);
	g_signal_handlers_unblock_by_func (self->zoom_scale,
					   zoom_scale_changed_cb, self);
}

static void
apply_zoom (NemoImageViewer *self)
{
	GdkPixbuf *base, *scaled;
	int pw, ph, nw, nh, avail_w, avail_h;
	double z;

	base = get_base_pixbuf (self);
	if (base == NULL)
		return;

	pw = gdk_pixbuf_get_width (base);
	ph = gdk_pixbuf_get_height (base);
	z  = self->zoom_level;

	if (self->fit_to_container) {
		double zw = 1.0, zh = 1.0;

		avail_w = gtk_widget_get_allocated_width  (self->scroll);
		avail_h = gtk_widget_get_allocated_height (self->scroll);

		/* Fit while preserving aspect ratio: use the more restrictive
		 * of the two axes so the entire image is visible inside the
		 * container, matching whichever edge fills first. */
		if (avail_w > 20)
			zw = (double) (avail_w - 20) / pw;
		if (avail_h > 20)
			zh = (double) (avail_h - 20) / ph;

		z = (zw < zh) ? zw : zh;
		if (z > 4.0)  z = 4.0;
		if (z < 0.01) z = 0.01;
	}

	nw = MAX ((int) (pw * z), 1);
	nh = MAX ((int) (ph * z), 1);

	scaled = gdk_pixbuf_scale_simple (base, nw, nh, GDK_INTERP_BILINEAR);
	gtk_image_set_from_pixbuf (GTK_IMAGE (self->image), scaled);
	g_object_unref (scaled);
}

/* ------------------------------------------------------------------ */
/* Control callbacks                                                  */
/* ------------------------------------------------------------------ */

static void
zoom_scale_changed_cb (GtkRange *range, gpointer user_data)
{
	NemoImageViewer *self = NEMO_IMAGE_VIEWER (user_data);

	self->zoom_level = gtk_range_get_value (range);

	if (self->fit_to_container) {
		self->fit_to_container = FALSE;
		gtk_toggle_button_set_active (
			GTK_TOGGLE_BUTTON (self->fit_check), FALSE);
	}

	apply_zoom (self);
}

static void
fit_check_toggled_cb (GtkToggleButton *btn, gpointer user_data)
{
	NemoImageViewer *self = NEMO_IMAGE_VIEWER (user_data);

	self->fit_to_container = gtk_toggle_button_get_active (btn);
	if (self->fit_to_container)
		apply_zoom (self);
}

static void
scroll_size_allocate_cb (GtkWidget    *widget,
			 GdkRectangle *allocation,
			 gpointer      user_data)
{
	NemoImageViewer *self = NEMO_IMAGE_VIEWER (user_data);

	if (self->fit_to_container && get_base_pixbuf (self) != NULL)
		apply_zoom (self);
}

/* ------------------------------------------------------------------ */
/* Internal — consume a loaded GdkPixbufAnimation                     */
/* ------------------------------------------------------------------ */

static void
set_animation (NemoImageViewer *self, GdkPixbufAnimation *anim)
{
	GdkPixbuf *first;
	int pw, ph;

	clear_image_data (self);

	if (anim == NULL)
		return;

	if (gdk_pixbuf_animation_is_static_image (anim)) {
		self->original_pixbuf = g_object_ref (
			gdk_pixbuf_animation_get_static_image (anim));
		self->is_animated = FALSE;
		g_object_unref (anim);

		pw = gdk_pixbuf_get_width  (self->original_pixbuf);
		ph = gdk_pixbuf_get_height (self->original_pixbuf);

		if (self->fit_to_container) {
			apply_zoom (self);
		} else {
			self->zoom_level = compute_fit_zoom (self, pw, ph);
			update_zoom_scale_quietly (self, self->zoom_level);
			apply_zoom (self);
		}
	} else {
		self->animation  = anim;
		self->is_animated = TRUE;

		first = gdk_pixbuf_animation_get_static_image (anim);
		if (first != NULL) {
			pw = gdk_pixbuf_get_width  (first);
			ph = gdk_pixbuf_get_height (first);
			if (!self->fit_to_container)
				self->zoom_level = compute_fit_zoom (self, pw, ph);
			update_zoom_scale_quietly (self, self->zoom_level);
		}

		start_animation (self);
	}
}

/* ------------------------------------------------------------------ */
/* Async load callback                                                */
/* ------------------------------------------------------------------ */

static void
stream_load_ready_cb (GObject      *source,
		      GAsyncResult *result,
		      gpointer      user_data)
{
	NemoImageViewer *self = NEMO_IMAGE_VIEWER (user_data);
	GdkPixbufAnimation *anim;
	GError *error = NULL;

	anim = gdk_pixbuf_animation_new_from_stream_finish (result, &error);
	g_object_unref (source);   /* the stream */

	if (error != NULL) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			g_warning ("NemoImageViewer: load failed: %s",
				   error->message);
		g_error_free (error);
		return;
	}

	set_animation (self, anim);
}

/* ------------------------------------------------------------------ */
/* Construction / destruction                                         */
/* ------------------------------------------------------------------ */

static void
nemo_image_viewer_dispose (GObject *object)
{
	NemoImageViewer *self = NEMO_IMAGE_VIEWER (object);

	clear_image_data (self);

	G_OBJECT_CLASS (nemo_image_viewer_parent_class)->dispose (object);
}

static void
nemo_image_viewer_class_init (NemoImageViewerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->dispose = nemo_image_viewer_dispose;
}

static void
nemo_image_viewer_init (NemoImageViewer *self)
{
	self->zoom_level       = 1.0;
	self->fit_to_container = FALSE;
	self->show_controls    = TRUE;
	self->is_animated      = FALSE;

	gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
				       GTK_ORIENTATION_VERTICAL);

	/* Scrolled window for the image */
	self->scroll = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (
		GTK_SCROLLED_WINDOW (self->scroll),
		GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

	self->image = gtk_image_new ();
	gtk_widget_set_halign (self->image, GTK_ALIGN_CENTER);
	gtk_widget_set_valign (self->image, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top (self->image, 8);
	gtk_widget_set_margin_bottom (self->image, 8);
	gtk_container_add (GTK_CONTAINER (self->scroll), self->image);
	gtk_widget_show (self->image);

	gtk_box_pack_start (GTK_BOX (self), self->scroll, TRUE, TRUE, 0);
	gtk_widget_show (self->scroll);

	g_signal_connect (self->scroll, "size-allocate",
			  G_CALLBACK (scroll_size_allocate_cb), self);

	/* Zoom controls */
	self->ctrl_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_widget_set_margin_start (self->ctrl_box, 4);
	gtk_widget_set_margin_end   (self->ctrl_box, 4);
	gtk_widget_set_margin_top   (self->ctrl_box, 2);
	gtk_widget_set_margin_bottom (self->ctrl_box, 4);

	self->fit_check = gtk_check_button_new_with_label (_("Fit"));
	g_signal_connect (self->fit_check, "toggled",
			  G_CALLBACK (fit_check_toggled_cb), self);
	gtk_box_pack_start (GTK_BOX (self->ctrl_box),
			    self->fit_check, FALSE, FALSE, 0);
	gtk_widget_show (self->fit_check);

	self->zoom_scale = gtk_scale_new_with_range (
		GTK_ORIENTATION_HORIZONTAL, 0.1, 4.0, 0.1);
	gtk_scale_set_draw_value (GTK_SCALE (self->zoom_scale), TRUE);
	gtk_scale_set_value_pos  (GTK_SCALE (self->zoom_scale), GTK_POS_RIGHT);
	gtk_range_set_value (GTK_RANGE (self->zoom_scale), 1.0);
	gtk_widget_set_tooltip_text (self->zoom_scale, _("Zoom"));
	g_signal_connect (self->zoom_scale, "value-changed",
			  G_CALLBACK (zoom_scale_changed_cb), self);
	gtk_box_pack_start (GTK_BOX (self->ctrl_box),
			    self->zoom_scale, TRUE, TRUE, 0);
	gtk_widget_show (self->zoom_scale);

	gtk_box_pack_end (GTK_BOX (self), self->ctrl_box, FALSE, FALSE, 0);
	gtk_widget_show (self->ctrl_box);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

NemoImageViewer *
nemo_image_viewer_new (void)
{
	return g_object_new (NEMO_TYPE_IMAGE_VIEWER, NULL);
}

gboolean
nemo_image_viewer_load_file (NemoImageViewer *self,
			     const gchar     *path,
			     GError         **error)
{
	GdkPixbufAnimation *anim;

	g_return_val_if_fail (NEMO_IS_IMAGE_VIEWER (self), FALSE);
	g_return_val_if_fail (path != NULL, FALSE);

	anim = gdk_pixbuf_animation_new_from_file (path, error);
	if (anim != NULL) {
		set_animation (self, anim);
		return TRUE;
	}

#ifdef HAVE_LIBRAW
	/* Fallback: try loading as a camera RAW file (DNG, ARW, CR2, NEF, etc.) */
	{
		libraw_data_t *raw;
		libraw_processed_image_t *img;
		GdkPixbuf *pixbuf;
		int ret;

		raw = libraw_init (0);
		if (raw == NULL)
			return FALSE;

		ret = libraw_open_file (raw, path);
		if (ret != LIBRAW_SUCCESS) {
			libraw_close (raw);
			return FALSE;
		}

		/* Use half-size for faster preview, sRGB output */
		raw->params.half_size = 1;
		raw->params.use_camera_wb = 1;
		raw->params.output_bps = 8;
		raw->params.output_color = 1; /* sRGB */

		ret = libraw_unpack (raw);
		if (ret != LIBRAW_SUCCESS) {
			libraw_close (raw);
			return FALSE;
		}

		ret = libraw_dcraw_process (raw);
		if (ret != LIBRAW_SUCCESS) {
			libraw_close (raw);
			return FALSE;
		}

		img = libraw_dcraw_make_mem_image (raw, &ret);
		if (img == NULL || ret != LIBRAW_SUCCESS) {
			libraw_close (raw);
			return FALSE;
		}

		if (img->type != LIBRAW_IMAGE_BITMAP || img->colors != 3) {
			libraw_dcraw_clear_mem (img);
			libraw_close (raw);
			return FALSE;
		}

		/* Create a GdkPixbuf from the RGB data.
		 * We must copy because libraw owns the buffer. */
		pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8,
		                         img->width, img->height);
		if (pixbuf != NULL) {
			guint8 *dst = gdk_pixbuf_get_pixels (pixbuf);
			int dst_stride = gdk_pixbuf_get_rowstride (pixbuf);
			int src_stride = img->width * 3;
			int row;

			for (row = 0; row < (int) img->height; row++) {
				memcpy (dst + row * dst_stride,
				        img->data + row * src_stride,
				        src_stride);
			}
		}

		libraw_dcraw_clear_mem (img);
		libraw_close (raw);

		if (pixbuf == NULL)
			return FALSE;

		/* Clear previous gdk-pixbuf error since we succeeded via libraw */
		if (error != NULL)
			g_clear_error (error);

		/* Wrap the static pixbuf in a single-frame animation */
		{
			GdkPixbufSimpleAnim *simple;
			simple = gdk_pixbuf_simple_anim_new (
				gdk_pixbuf_get_width (pixbuf),
				gdk_pixbuf_get_height (pixbuf),
				0.0f);
			gdk_pixbuf_simple_anim_add_frame (simple, pixbuf);
			anim = GDK_PIXBUF_ANIMATION (simple);
		}
		g_object_unref (pixbuf);
		set_animation (self, anim);
		return TRUE;
	}
#endif /* HAVE_LIBRAW */

	return FALSE;
}

void
nemo_image_viewer_load_stream_async (NemoImageViewer *self,
				     GInputStream    *stream,
				     GCancellable    *cancellable)
{
	g_return_if_fail (NEMO_IS_IMAGE_VIEWER (self));
	g_return_if_fail (G_IS_INPUT_STREAM (stream));

	gdk_pixbuf_animation_new_from_stream_async (
		stream, cancellable, stream_load_ready_cb, self);
}

void
nemo_image_viewer_clear (NemoImageViewer *self)
{
	g_return_if_fail (NEMO_IS_IMAGE_VIEWER (self));

	clear_image_data (self);
	gtk_image_clear (GTK_IMAGE (self->image));
}

void
nemo_image_viewer_set_zoom (NemoImageViewer *self, double zoom)
{
	g_return_if_fail (NEMO_IS_IMAGE_VIEWER (self));

	self->zoom_level = CLAMP (zoom, 0.1, 4.0);
	update_zoom_scale_quietly (self, self->zoom_level);

	if (self->fit_to_container) {
		self->fit_to_container = FALSE;
		gtk_toggle_button_set_active (
			GTK_TOGGLE_BUTTON (self->fit_check), FALSE);
	}

	apply_zoom (self);
}

double
nemo_image_viewer_get_zoom (NemoImageViewer *self)
{
	g_return_val_if_fail (NEMO_IS_IMAGE_VIEWER (self), 1.0);
	return self->zoom_level;
}

void
nemo_image_viewer_set_fit (NemoImageViewer *self, gboolean fit)
{
	g_return_if_fail (NEMO_IS_IMAGE_VIEWER (self));

	self->fit_to_container = fit;
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->fit_check), fit);

	if (fit)
		apply_zoom (self);
}

gboolean
nemo_image_viewer_get_fit (NemoImageViewer *self)
{
	g_return_val_if_fail (NEMO_IS_IMAGE_VIEWER (self), FALSE);
	return self->fit_to_container;
}

void
nemo_image_viewer_set_show_controls (NemoImageViewer *self, gboolean show)
{
	g_return_if_fail (NEMO_IS_IMAGE_VIEWER (self));

	self->show_controls = show;
	gtk_widget_set_visible (self->ctrl_box, show);
}
