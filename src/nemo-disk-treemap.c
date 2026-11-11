/* Native, read-only disk-usage treemap. SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include "nemo-disk-treemap.h"
#include "nemo-dir-analyzer.h"
#include <libnemo-private/nemo-file-colors.h>
#include <libnemo-private/nemo-file-utilities.h>
#include <glib/gi18n.h>
#include <math.h>

typedef struct {
	NemoDirScanNode *node;
	double x, y, width, height;
	guint items;
	guint64 size;
	gboolean heading;
	NemoFileColorKind kind;
} TreemapRect;

typedef struct {
	NemoDirScanNode *tree;
	NemoDirScanStats stats;
} TreemapResult;

struct _NemoDiskTreemap {
	GtkBox parent;
	GtkWidget *area, *path_label, *status, *details, *up, *home, *open, *refresh, *spinner;
	NemoDirScanNode *tree, *current, *selected;
	NemoDirScanStats stats;
	GArray *rects;
	GCancellable *cancel;
	char *path;
	gboolean destroyed;
	int layout_width, layout_height;
	gint selected_rect;
};

G_DEFINE_TYPE (NemoDiskTreemap, nemo_disk_treemap, GTK_TYPE_BOX)

static TreemapRect *
treemap_selection (NemoDiskTreemap *self)
{
	return self->selected_rect >= 0 && (guint) self->selected_rect < self->rects->len
		? &g_array_index (self->rects, TreemapRect, self->selected_rect) : NULL;
}

static void
treemap_result_free (TreemapResult *result)
{
	nemo_dir_scan_node_free (result->tree);
	g_free (result);
}

static void
treemap_update_labels (NemoDiskTreemap *self)
{
	gtk_widget_set_sensitive (self->up, self->current != NULL && self->current->parent != NULL);
	gtk_widget_set_sensitive (self->home, self->current != NULL && self->current != self->tree);
	gtk_widget_set_sensitive (self->open, self->current != NULL);
	if (self->current == NULL)
		return;
	g_autofree char *path = nemo_dir_scan_node_get_path (self->current);
	g_autofree char *display = g_filename_display_name (path);
	gtk_label_set_text (GTK_LABEL (self->path_label), display);
	gtk_widget_set_tooltip_text (self->path_label, display);
	g_autofree char *size = g_format_size (self->current->size);
	g_autofree char *files = g_strdup_printf ("%" G_GUINT64_FORMAT, self->current->files);
	g_autofree char *summary = g_strdup_printf (
		ngettext ("%s in %s file", "%s in %s files", self->current->files), size, files);
	g_autofree char *status = self->stats.incomplete
		? g_strdup_printf (_("Incomplete: %s. Some contents could not be read."), summary)
		: g_strdup (summary);
	gtk_label_set_text (GTK_LABEL (self->status), status);
	if (self->selected != NULL) {
		TreemapRect *rect = treemap_selection (self);
		g_autofree char *selected_path = nemo_dir_scan_node_get_path (self->selected);
		g_autofree char *selected_display = g_filename_display_name (selected_path);
		g_autofree char *selected_size = g_format_size (rect != NULL ? rect->size : self->selected->size);
		g_autofree char *text = rect != NULL && rect->items > 1
			? g_strdup_printf (ngettext ("%s  -  %u small item, %s", "%s  -  %u small items, %s",
			                            rect->items), selected_display, rect->items, selected_size)
			: g_strdup_printf ("%s  -  %s", selected_display, selected_size);
		gtk_label_set_text (GTK_LABEL (self->details), text);
		gtk_widget_set_tooltip_text (self->details, text);
	} else {
		gtk_label_set_text (GTK_LABEL (self->details),
			_("Double-click or Enter: zoom into a folder or show a file. Backspace: zoom out."));
		gtk_widget_set_tooltip_text (self->details, NULL);
	}
}

static void
treemap_zoom (NemoDiskTreemap *self, NemoDirScanNode *node)
{
	if (node == NULL || node->children == NULL)
		return;
	self->current = node;
	self->selected = NULL;
	self->selected_rect = -1;
	g_array_set_size (self->rects, 0);
	self->layout_width = 0;
	treemap_update_labels (self);
	gtk_widget_queue_draw (self->area);
}

static void
treemap_open_node (NemoDiskTreemap *self, NemoDirScanNode *node)
{
	if (node == NULL)
		return;
	g_autofree char *path = nemo_dir_scan_node_get_path (node);
	g_autoptr (GFile) file = g_file_new_for_path (path);
	g_signal_emit_by_name (self, "open-location", file, node->children != NULL);
}

static void
treemap_up (GtkButton *button, NemoDiskTreemap *self)
{
	if (self->current != NULL)
		treemap_zoom (self, self->current->parent);
}

static void
treemap_home (GtkButton *button, NemoDiskTreemap *self)
{
	treemap_zoom (self, self->tree);
}

static void
treemap_open (GtkButton *button, NemoDiskTreemap *self)
{
	treemap_open_node (self, self->selected != NULL ? self->selected : self->current);
}

static void
treemap_measure_again (GtkButton *button, NemoDiskTreemap *self)
{
	if (self->path != NULL)
		nemo_disk_treemap_scan (self, self->path);
}

static void
treemap_kind_colour (GtkWidget *widget, NemoFileColorKind kind, GdkRGBA *colour)
{
	if (!nemo_file_color_for_kind (kind, gtk_widget_get_style_context (widget),
	                              GTK_STATE_FLAG_NORMAL, colour)) {
		const NemoDirColour *fallback = nemo_dir_get_colour (kind % NEMO_DIR_N_COLOURS);
		*colour = (GdkRGBA) { fallback->r, fallback->g, fallback->b, 1.0 };
		if (kind >= NEMO_DIR_N_COLOURS) {
			const NemoDirColour *mix = nemo_dir_get_colour ((kind + 2) % NEMO_DIR_N_COLOURS);
			colour->red = (colour->red + mix->r) / 2;
			colour->green = (colour->green + mix->g) / 2;
			colour->blue = (colour->blue + mix->b) / 2;
		}
		if (kind == NEMO_FILE_COLOR_DEFAULT)
			*colour = (GdkRGBA) { 0.46, 0.49, 0.52, 1.0 };
	}
	colour->alpha = 1.0;
}

static void treemap_layout_node (NemoDiskTreemap *,
                                NemoDirScanNode *, double, double, double, double, guint);

static void
treemap_add_rect (NemoDiskTreemap *self, TreemapRect rect, gboolean heading)
{
	rect.heading = heading;
	g_autofree char *type = rect.node->children == NULL
		? g_content_type_guess (rect.node->name, NULL, 0, NULL) : NULL;
	g_autofree char *mime = type != NULL ? g_content_type_get_mime_type (type) : NULL;
	rect.kind = nemo_file_color_kind_for_type (rect.node->name, mime, rect.node->children != NULL);
	g_array_append_val (self->rects, rect);
}

static double
treemap_linear (double channel)
{
	return channel <= 0.04045 ? channel / 12.92 : pow ((channel + 0.055) / 1.055, 2.4);
}

static void
treemap_paint_rect (NemoDiskTreemap *self, cairo_t *cr, PangoLayout *layout,
                    TreemapRect rect, gboolean selected)
{
	GdkRGBA colour;
	treemap_kind_colour (self->area, rect.kind, &colour);
	if (rect.items > 1)
		colour = (GdkRGBA) { 0.46, 0.49, 0.52, 1.0 };
	cairo_rectangle (cr, rect.x + 0.5, rect.y + 0.5,
	                 MAX (0, rect.width - 1), MAX (0, rect.height - 1));
	gdk_cairo_set_source_rgba (cr, &colour);
	cairo_fill_preserve (cr);
	cairo_set_source_rgba (cr, 0, 0, 0, 0.3);
	cairo_set_line_width (cr, selected ? 2 : 0.5);
	if (selected)
		cairo_set_source_rgb (cr, 1, 1, 1);
	cairo_stroke (cr);
	if (rect.width < 65 || rect.height < (rect.heading ? 20 : 32))
		return;
	g_autofree char *name = rect.items > 1
		? g_strdup_printf (ngettext ("%u small item", "%u small items", rect.items), rect.items)
		: g_filename_display_basename (rect.node->name);
	g_autofree char *size = g_format_size (rect.size);
	g_autofree char *text = rect.heading ? g_strdup (name) : g_strdup_printf ("%s\n%s", name, size);
	pango_layout_set_text (layout, text, -1);
	pango_layout_set_width (layout, (int) (rect.width - 10) * PANGO_SCALE);
	pango_layout_set_height (layout, (rect.heading ? 18 : (int) rect.height - 8) * PANGO_SCALE);
	pango_layout_set_ellipsize (layout, PANGO_ELLIPSIZE_END);
	int text_height;
	pango_layout_get_pixel_size (layout, NULL, &text_height);
	double luminance = 0.2126 * treemap_linear (colour.red) +
	                   0.7152 * treemap_linear (colour.green) + 0.0722 * treemap_linear (colour.blue);
	double ink = luminance > 0.179 ? 0 : 1;
	cairo_set_source_rgb (cr, ink, ink, ink);
	cairo_save (cr);
	cairo_rectangle (cr, rect.x + 4, rect.y + 2, rect.width - 8, rect.heading ? 18 : rect.height - 4);
	cairo_clip (cr);
	cairo_move_to (cr, rect.x + 5, rect.heading ? rect.y + 2 : rect.y + (rect.height - text_height) / 2);
	pango_cairo_show_layout (cr, layout);
	cairo_restore (cr);
}

/* Balanced, size-weighted bisection; subpixel groups keep their combined area. */
static void
treemap_layout_range (NemoDiskTreemap *self,
                      NemoDirScanNode *parent, guint first, guint count, guint64 total,
                      double x, double y, double width, double height, guint depth)
{
	if (count == 0 || total == 0 || width <= 0 || height <= 0)
		return;
	if (count == 1) {
		treemap_layout_node (self, g_ptr_array_index (parent->children, first),
		                    x, y, width, height, depth);
		return;
	}
	if (width * height < 600 || width < 6 || height < 6) {
		treemap_add_rect (self,
			(TreemapRect) { parent, x, y, width, height, count, total }, FALSE);
		return;
	}
	guint split = 1;
	guint64 left = ((NemoDirScanNode *) g_ptr_array_index (parent->children, first))->size;
	while (split < count - 1) {
		NemoDirScanNode *next = g_ptr_array_index (parent->children, first + split);
		if (fabs ((double) (left + next->size) - (double) total / 2) >
		    fabs ((double) left - (double) total / 2))
			break;
		left += next->size;
		split++;
	}
	double fraction = (double) left / total;
	if (width >= height) {
		treemap_layout_range (self, parent, first, split, left,
		                      x, y, width * fraction, height, depth);
		treemap_layout_range (self, parent, first + split, count - split, total - left,
		                      x + width * fraction, y, width * (1 - fraction), height, depth);
	} else {
		treemap_layout_range (self, parent, first, split, left,
		                      x, y, width, height * fraction, depth);
		treemap_layout_range (self, parent, first + split, count - split, total - left,
		                      x, y + height * fraction, width, height * (1 - fraction), depth);
	}
}

static void
treemap_layout_children (NemoDiskTreemap *self,
                         NemoDirScanNode *node, double x, double y, double width, double height,
                         guint depth)
{
	guint count = node->children->len;
	while (count > 0 && ((NemoDirScanNode *) g_ptr_array_index (node->children, count - 1))->size == 0)
		count--;
	treemap_layout_range (self, node, 0, count, node->size,
	                      x, y, width, height, depth);
}

static void
treemap_layout_node (NemoDiskTreemap *self,
                     NemoDirScanNode *node, double x, double y, double width, double height,
                     guint depth)
{
	gboolean expand = node->children != NULL && depth < 4 && width >= 80 && height >= 65;
	treemap_add_rect (self,
		(TreemapRect) { node, x, y, width, height, 1, node->size }, expand);
	if (expand)
		treemap_layout_children (self, node, x + 3, y + 22,
		                         width - 6, height - 25, depth + 1);
}

static gboolean
treemap_ensure_layout (NemoDiskTreemap *self)
{
	int width = gtk_widget_get_allocated_width (self->area);
	int height = gtk_widget_get_allocated_height (self->area);
	if (self->current == NULL || self->current->size == 0 || width <= 4 || height <= 4)
		return FALSE;
	if (self->layout_width != width || self->layout_height != height) {
		g_array_set_size (self->rects, 0);
		treemap_layout_children (self, self->current, 2, 2, width - 4, height - 4, 0);
		self->layout_width = width;
		self->layout_height = height;
		self->selected_rect = -1;
		for (guint i = 0; i < self->rects->len; i++) {
			TreemapRect *rect = &g_array_index (self->rects, TreemapRect, i);
			if (rect->node == self->selected && rect->items == 1) {
				self->selected_rect = i;
				break;
			}
		}
		if (self->selected_rect == -1 && self->selected != NULL) {
			self->selected = NULL;
			treemap_update_labels (self);
		}
	}
	return TRUE;
}

static gboolean
treemap_draw (GtkWidget *widget, cairo_t *cr, NemoDiskTreemap *self)
{
	int width = gtk_widget_get_allocated_width (widget);
	int height = gtk_widget_get_allocated_height (widget);
	GtkStyleContext *context = gtk_widget_get_style_context (widget);
	gtk_render_background (context, cr, 0, 0, width, height);
	PangoLayout *layout = gtk_widget_create_pango_layout (widget, NULL);
	if (treemap_ensure_layout (self)) {
		for (guint i = 0; i < self->rects->len; i++)
			treemap_paint_rect (self, cr, layout, g_array_index (self->rects, TreemapRect, i),
			                    self->selected_rect == (gint) i);
	} else {
		const char *message = self->cancel != NULL ? _("Measuring disk usage...")
			: self->current == NULL ? _("Select a volume to measure.")
			: self->stats.incomplete ? _("No readable non-empty files found.") : _("No non-empty files found.");
		pango_layout_set_text (layout, message, -1);
		GdkRGBA foreground;
		gtk_style_context_get_color (context, GTK_STATE_FLAG_NORMAL, &foreground);
		gdk_cairo_set_source_rgba (cr, &foreground);
		cairo_move_to (cr, 16, 20);
		pango_cairo_show_layout (cr, layout);
	}
	g_object_unref (layout);
	if (gtk_widget_has_focus (widget))
		gtk_render_focus (context, cr, 0, 0, width, height);
	return FALSE;
}

static TreemapRect *
treemap_hit (NemoDiskTreemap *self, double x, double y)
{
	treemap_ensure_layout (self);
	for (gint i = (gint) self->rects->len - 1; i >= 0; i--) {
		TreemapRect *rect = &g_array_index (self->rects, TreemapRect, i);
		if (x >= rect->x && y >= rect->y && x < rect->x + rect->width && y < rect->y + rect->height)
			return rect;
	}
	return NULL;
}

static gboolean
treemap_tooltip (GtkWidget *widget, gint x, gint y, gboolean keyboard,
                 GtkTooltip *tooltip, NemoDiskTreemap *self)
{
	TreemapRect *rect = keyboard ? treemap_selection (self) : treemap_hit (self, x, y);
	NemoDirScanNode *node = rect != NULL ? rect->node : keyboard ? self->selected : NULL;
	if (node == NULL)
		return FALSE;
	g_autofree char *path = nemo_dir_scan_node_get_path (node);
	g_autofree char *display = g_filename_display_name (path);
	g_autofree char *size = g_format_size (rect != NULL ? rect->size : node->size);
	g_autofree char *text = rect != NULL && rect->items > 1
		? g_strdup_printf (ngettext ("%s\n%u small item - %s", "%s\n%u small items - %s", rect->items),
		                   display, rect->items, size)
		: g_strdup_printf ("%s\n%s", display, size);
	gtk_tooltip_set_text (tooltip, text);
	return TRUE;
}

static gboolean
treemap_button (GtkWidget *widget, GdkEventButton *event, NemoDiskTreemap *self)
{
	if (event->button != 1 && event->button != 3)
		return FALSE;
	TreemapRect *rect = treemap_hit (self, event->x, event->y);
	if (rect == NULL)
		return FALSE;
	gtk_widget_grab_focus (widget);
	self->selected = rect->node;
	self->selected_rect = rect - (TreemapRect *) self->rects->data;
	treemap_update_labels (self);
	gtk_widget_queue_draw (widget);
	if (event->button == 3)
		treemap_zoom (self, rect->node->children != NULL ? rect->node : rect->node->parent);
	else if (event->button == 1 && event->type == GDK_2BUTTON_PRESS) {
		if (rect->node->children != NULL)
			treemap_zoom (self, rect->node);
		else
			treemap_open_node (self, rect->node);
	}
	return TRUE;
}

static gboolean
treemap_key (GtkWidget *widget, GdkEventKey *event, NemoDiskTreemap *self)
{
	if ((event->state & gtk_accelerator_get_default_mod_mask ()) != 0)
		return FALSE;
	if (event->keyval == GDK_KEY_BackSpace || event->keyval == GDK_KEY_minus) {
		treemap_up (NULL, self);
		return TRUE;
	}
	if (event->keyval == GDK_KEY_Home) {
		treemap_home (NULL, self);
		return TRUE;
	}
	if (event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
		if (self->selected != NULL && self->selected->children != NULL)
			treemap_zoom (self, self->selected);
		else
			treemap_open_node (self, self->selected);
		return TRUE;
	}
	int dx = event->keyval == GDK_KEY_Right ? 1 : event->keyval == GDK_KEY_Left ? -1 : 0;
	int dy = event->keyval == GDK_KEY_Down ? 1 : event->keyval == GDK_KEY_Up ? -1 : 0;
	if (dx == 0 && dy == 0)
		return FALSE;
	treemap_ensure_layout (self);
	double x = 0, y = 0;
	TreemapRect *selected = treemap_selection (self);
	if (selected != NULL) {
		x = selected->x + selected->width / 2;
		y = selected->y + selected->height / 2;
	}
	gint best = -1;
	double distance = G_MAXDOUBLE;
	for (guint i = 0; i < self->rects->len; i++) {
		TreemapRect *rect = &g_array_index (self->rects, TreemapRect, i);
		if ((gint) i == self->selected_rect)
			continue;
		double cx = rect->x + rect->width / 2 - x, cy = rect->y + rect->height / 2 - y;
		if (self->selected != NULL && (dx * cx + dy * cy) <= 0)
			continue;
		double score = cx * cx + cy * cy + 2 * pow (dx != 0 ? cy : cx, 2);
		if (score < distance) {
			distance = score;
			best = i;
		}
	}
	if (best >= 0) {
		self->selected_rect = best;
		self->selected = g_array_index (self->rects, TreemapRect, best).node;
		treemap_update_labels (self);
		gtk_widget_queue_draw (widget);
	}
	return TRUE;
}

static gpointer
treemap_scan_thread (gpointer data)
{
	GTask *task = data;
	GError *error = NULL;
	TreemapResult *result = g_new0 (TreemapResult, 1);
	nemo_thread_set_background_priority ();
	result->tree = nemo_dir_scan_collect_tree (g_task_get_task_data (task),
		g_task_get_cancellable (task), &result->stats, &error);
	if (result->tree == NULL) {
		treemap_result_free (result);
		g_task_return_error (task, error);
	} else {
		g_task_return_pointer (task, result, (GDestroyNotify) treemap_result_free);
	}
	g_object_unref (task);
	return NULL;
}

static void
treemap_scan_done (GObject *object, GAsyncResult *async, gpointer unused)
{
	NemoDiskTreemap *self = NEMO_DISK_TREEMAP (object);
	GTask *task = G_TASK (async);
	GError *error = NULL;
	TreemapResult *result = g_task_propagate_pointer (task, &error);
	if (self->destroyed || g_task_get_cancellable (task) != self->cancel) {
		g_clear_pointer (&result, treemap_result_free);
		g_clear_error (&error);
		return;
	}
	g_clear_object (&self->cancel);
	gtk_spinner_stop (GTK_SPINNER (self->spinner));
	if (result == NULL) {
		gtk_label_set_text (GTK_LABEL (self->status), error->message);
		g_clear_error (&error);
	} else {
		self->tree = result->tree;
		result->tree = NULL;
		self->stats = result->stats;
		treemap_result_free (result);
		treemap_zoom (self, self->tree);
	}
	gtk_widget_queue_draw (self->area);
}

void
nemo_disk_treemap_cancel (NemoDiskTreemap *self)
{
	if (self->cancel == NULL)
		return;
	g_cancellable_cancel (self->cancel);
	g_clear_object (&self->cancel);
	gtk_spinner_stop (GTK_SPINNER (self->spinner));
	gtk_label_set_text (GTK_LABEL (self->status), _("Measurement cancelled."));
	gtk_widget_queue_draw (self->area);
}

void
nemo_disk_treemap_scan (NemoDiskTreemap *self, const char *path)
{
	g_return_if_fail (NEMO_IS_DISK_TREEMAP (self) && !self->destroyed && path != NULL);
	char *new_path = g_strdup (path);
	nemo_disk_treemap_cancel (self);
	self->current = self->selected = NULL;
	self->selected_rect = -1;
	g_array_set_size (self->rects, 0);
	self->layout_width = 0;
	g_clear_pointer (&self->tree, nemo_dir_scan_node_free);
	g_free (self->path);
	self->path = new_path;
	self->cancel = g_cancellable_new ();
	g_autofree char *display = g_filename_display_name (self->path);
	gtk_label_set_text (GTK_LABEL (self->path_label), display);
	gtk_widget_set_tooltip_text (self->path_label, display);
	gtk_label_set_text (GTK_LABEL (self->status), _("Measuring disk usage..."));
	gtk_label_set_text (GTK_LABEL (self->details), "");
	gtk_widget_set_tooltip_text (self->details, NULL);
	treemap_update_labels (self);
	gtk_spinner_start (GTK_SPINNER (self->spinner));
	gtk_widget_queue_draw (self->area);
	GTask *task = g_task_new (self, self->cancel, treemap_scan_done, NULL);
	g_task_set_task_data (task, g_strdup (self->path), g_free);
	g_thread_unref (g_thread_new ("disk-heatmap", treemap_scan_thread, task));
}

const char *nemo_disk_treemap_get_path (NemoDiskTreemap *self) { return self->path; }
gboolean nemo_disk_treemap_is_loading (NemoDiskTreemap *self) { return self->cancel != NULL; }
gboolean nemo_disk_treemap_has_result (NemoDiskTreemap *self) { return self->tree != NULL; }

static void
treemap_destroy (GtkWidget *widget)
{
	NemoDiskTreemap *self = NEMO_DISK_TREEMAP (widget);
	self->destroyed = TRUE;
	nemo_disk_treemap_cancel (self);
	GTK_WIDGET_CLASS (nemo_disk_treemap_parent_class)->destroy (widget);
}

static void
treemap_finalize (GObject *object)
{
	NemoDiskTreemap *self = NEMO_DISK_TREEMAP (object);
	nemo_dir_scan_node_free (self->tree);
	g_array_unref (self->rects);
	g_free (self->path);
	G_OBJECT_CLASS (nemo_disk_treemap_parent_class)->finalize (object);
}

static void
nemo_disk_treemap_class_init (NemoDiskTreemapClass *klass)
{
	GTK_WIDGET_CLASS (klass)->destroy = treemap_destroy;
	G_OBJECT_CLASS (klass)->finalize = treemap_finalize;
	g_signal_new ("open-location", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST,
	              0, NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 2,
	              G_TYPE_FILE, G_TYPE_BOOLEAN);
}

static gboolean
treemap_legend_draw (GtkWidget *widget, cairo_t *cr, gpointer data)
{
	GdkRGBA colour;
	treemap_kind_colour (widget, GPOINTER_TO_UINT (data), &colour);
	gdk_cairo_set_source_rgba (cr, &colour);
	cairo_rectangle (cr, 1, 2, 12, 12);
	cairo_fill (cr);
	return FALSE;
}

static void
nemo_disk_treemap_init (NemoDiskTreemap *self)
{
	gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
	gtk_box_set_spacing (GTK_BOX (self), 8);
	self->rects = g_array_new (FALSE, FALSE, sizeof (TreemapRect));
	self->selected_rect = -1;
	GtkWidget *toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	self->up = gtk_button_new_with_label (_("Zoom Out"));
	self->home = gtk_button_new_with_label (_("Reset Zoom"));
	self->open = gtk_button_new_with_label (_("Open Location"));
	self->refresh = gtk_button_new_with_label (_("Measure Again"));
	self->path_label = gtk_label_new ("");
	gtk_label_set_ellipsize (GTK_LABEL (self->path_label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_xalign (GTK_LABEL (self->path_label), 0);
	gtk_box_pack_start (GTK_BOX (toolbar), self->up, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (toolbar), self->home, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (toolbar), self->path_label, TRUE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX (toolbar), self->open, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (toolbar), self->refresh, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (self), toolbar, FALSE, FALSE, 0);
	g_signal_connect (self->up, "clicked", G_CALLBACK (treemap_up), self);
	g_signal_connect (self->home, "clicked", G_CALLBACK (treemap_home), self);
	g_signal_connect (self->open, "clicked", G_CALLBACK (treemap_open), self);
	g_signal_connect (self->refresh, "clicked", G_CALLBACK (treemap_measure_again), self);
	GtkWidget *summary = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
	self->spinner = gtk_spinner_new ();
	self->status = gtk_label_new (_("Select a volume to measure."));
	gtk_label_set_xalign (GTK_LABEL (self->status), 0);
	gtk_label_set_line_wrap (GTK_LABEL (self->status), TRUE);
	gtk_box_pack_start (GTK_BOX (summary), self->spinner, FALSE, FALSE, 0);
	gtk_box_pack_start (GTK_BOX (summary), self->status, TRUE, TRUE, 0);
	gtk_box_pack_start (GTK_BOX (self), summary, FALSE, FALSE, 0);
	self->area = gtk_drawing_area_new ();
	gtk_widget_set_size_request (self->area, 200, 360);
	gtk_widget_set_hexpand (self->area, TRUE);
	gtk_widget_set_vexpand (self->area, TRUE);
	gtk_widget_set_can_focus (self->area, TRUE);
	gtk_widget_set_has_tooltip (self->area, TRUE);
	atk_object_set_name (gtk_widget_get_accessible (self->area), _("Disk usage heatmap"));
	atk_object_set_description (gtk_widget_get_accessible (self->area),
		_("Rectangles represent file sizes. Use arrow keys to select, Enter to open, and Backspace to zoom out."));
	gtk_widget_add_events (self->area, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
	g_signal_connect (self->area, "draw", G_CALLBACK (treemap_draw), self);
	g_signal_connect (self->area, "button-press-event", G_CALLBACK (treemap_button), self);
	g_signal_connect (self->area, "key-press-event", G_CALLBACK (treemap_key), self);
	g_signal_connect (self->area, "query-tooltip", G_CALLBACK (treemap_tooltip), self);
	gtk_box_pack_start (GTK_BOX (self), self->area, TRUE, TRUE, 0);
	self->details = gtk_label_new ("");
	gtk_label_set_xalign (GTK_LABEL (self->details), 0);
	gtk_label_set_ellipsize (GTK_LABEL (self->details), PANGO_ELLIPSIZE_MIDDLE);
	gtk_box_pack_start (GTK_BOX (self), self->details, FALSE, FALSE, 0);
	GtkWidget *legend = gtk_flow_box_new ();
	const char *kinds[NEMO_FILE_COLOR_COUNT] = {
		_("Other"), _("Folders"), _("Images"), _("Video"), _("Audio"),
		_("Archives"), _("Documents"), _("Source code"), _("Applications")
	};
	gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (legend), GTK_SELECTION_NONE);
	gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (legend), NEMO_FILE_COLOR_COUNT);
	gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (legend), 1);
	for (guint i = 0; i < NEMO_FILE_COLOR_COUNT; i++) {
		GtkWidget *item = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 3);
		GtkWidget *swatch = gtk_drawing_area_new ();
		gtk_widget_set_size_request (swatch, 14, 16);
		g_signal_connect (swatch, "draw", G_CALLBACK (treemap_legend_draw), GUINT_TO_POINTER (i));
		gtk_box_pack_start (GTK_BOX (item), swatch, FALSE, FALSE, 0);
		gtk_box_pack_start (GTK_BOX (item), gtk_label_new (kinds[i]), FALSE, FALSE, 0);
		gtk_container_add (GTK_CONTAINER (legend), item);
	}
	gtk_box_pack_start (GTK_BOX (self), legend, FALSE, FALSE, 0);
	GtkWidget *note = gtk_label_new (
		_("File sizes, including hidden files. Links and other mounted filesystems are excluded. Colors indicate file types."));
	gtk_label_set_line_wrap (GTK_LABEL (note), TRUE);
	gtk_label_set_xalign (GTK_LABEL (note), 0);
	gtk_style_context_add_class (gtk_widget_get_style_context (note), "dim-label");
	gtk_box_pack_start (GTK_BOX (self), note, FALSE, FALSE, 0);
	treemap_update_labels (self);
}

GtkWidget *
nemo_disk_treemap_new (void)
{
	return g_object_new (NEMO_TYPE_DISK_TREEMAP, NULL);
}
