/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <gtk/gtk.h>

/* Inspect the actual cached Pango layouts, including synthetic wrapping spaces. */
#include "../libnemo-private/nemo-icon-canvas-item.c"
#include <libnemo-private/nemo-smpl-prefs.h>

typedef NemoIconContainer TestIconContainer;
typedef NemoIconContainerClass TestIconContainerClass;
G_DEFINE_TYPE (TestIconContainer, test_icon_container, NEMO_TYPE_ICON_CONTAINER)

typedef struct {
	GtkWidget *window;
	NemoIconContainer *container;
	NemoIcon *icons[8];
} Fixture;

static void
fixture_get_text (NemoIconContainer *container, NemoIconData *data,
                  char **editable, char **additional,
                  gboolean *pinned, gboolean *unavailable, gboolean invisible)
{
	if (editable != NULL)
		*editable = g_strdup ((const char *) data);
	if (additional != NULL)
		*additional = g_strdup ("cur metadata");
	if (pinned != NULL)
		*pinned = FALSE;
	if (unavailable != NULL)
		*unavailable = FALSE;
}

static void
fixture_update_icon (NemoIconContainer *container, NemoIcon *icon, gboolean visible)
{
}

static gint
fixture_lines (NemoIconContainer *container)
{
	return 3;
}

static gint
fixture_pango_lines (NemoIconContainer *container)
{
	return -3;
}

static gint
fixture_additional_lines (NemoIconContainer *container)
{
	return 1;
}

static void
fixture_bounds (NemoIcon *icon, int *x1, int *y1, int *x2, int *y2,
                NemoIconCanvasItemBoundsUsage usage)
{
	*x1 = icon->item->details->x;
	*y1 = icon->item->details->y;
	*x2 = *x1 + 80;
	*y2 = *y1 + 80;
}

static void
test_icon_container_class_init (TestIconContainerClass *klass)
{
	klass->get_icon_text = fixture_get_text;
	klass->update_icon = fixture_update_icon;
	klass->get_max_layout_lines = fixture_lines;
	klass->get_max_layout_lines_for_pango = fixture_pango_lines;
	klass->get_additional_text_line_count = fixture_additional_lines;
	klass->icon_get_bounding_box = fixture_bounds;
}

static void
test_icon_container_init (TestIconContainer *container)
{
}

static void
drain_events (void)
{
	while (g_main_context_pending (NULL))
		g_main_context_iteration (NULL, FALSE);
}

static void
fixture_setup (Fixture *fixture, gconstpointer compact)
{
	static const char *names[] = {
		"misscurve.txt", "curva.txt", "curveycase.txt", "c_up_river.txt",
		"uncurl.txt", "curve-hidden.txt", "é_ß-曲.curve.7.txt", "CURVE-last.txt"
	};
	g_settings_set_enum (nemo_preferences, NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE,
	                     NEMO_INTERACTIVE_SEARCH_MODE_SUBSTRING);
	static const char *keys[] = {
		"type-jump-next", "type-jump-next-alt", "type-jump-next-secondary",
		"type-jump-previous", "type-jump-previous-alt", "type-jump-previous-secondary"
	};
	for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
		g_settings_reset (nemo_keybinding_settings, keys[i]);

	fixture->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	fixture->container = g_object_new (test_icon_container_get_type (), NULL);
	g_object_ref_sink (fixture->container);
	NemoIconContainer *container = fixture->container;
	if (compact != NULL) {
		container->details->layout_mode = NEMO_ICON_LAYOUT_T_B_L_R;
		container->details->label_position = NEMO_ICON_LABEL_POSITION_BESIDE;
	}
	gtk_window_set_default_size (GTK_WINDOW (fixture->window), 900, 300);
	gtk_container_add (GTK_CONTAINER (fixture->window), GTK_WIDGET (container));
	gtk_widget_show_all (fixture->window);
	gtk_window_present (GTK_WINDOW (fixture->window));
	gtk_widget_grab_focus (GTK_WIDGET (container));
	drain_events ();
	g_assert_true (gtk_widget_has_focus (GTK_WIDGET (container)));

	for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
		NemoIcon *icon = g_new0 (NemoIcon, 1);
		icon->data = (NemoIconData *) names[i];
		/* Unpositioned model icons never trigger deferred file/thumbnail I/O. */
		icon->x = icon->y = ICON_UNPOSITIONED_VALUE;
		icon->scale = 1.0;
		icon->item = NEMO_ICON_CANVAS_ITEM (eel_canvas_item_new (
			eel_canvas_root (EEL_CANVAS (container)), NEMO_TYPE_ICON_CANVAS_ITEM,
			"editable-text", names[i], "additional-text", "cur metadata", NULL));
		icon->item->user_data = icon;
		eel_canvas_item_move (EEL_CANVAS_ITEM (icon->item), 10 + 100 * i, 10);
		nemo_icon_canvas_item_set_is_visible (icon->item, TRUE);
		container->details->icons = g_list_append (container->details->icons, icon);
		g_hash_table_insert (container->details->icon_set, icon->data, icon);
		fixture->icons[i] = icon;
	}
	eel_canvas_item_hide (EEL_CANVAS_ITEM (fixture->icons[5]->item));
	drain_events ();
}

static void
fixture_teardown (Fixture *fixture, gconstpointer compact)
{
	gtk_widget_destroy (fixture->window);
	g_object_unref (fixture->container);
	drain_events ();
}

static gboolean press_key (Fixture *fixture, guint keyval, GdkModifierType modifiers);

static void
search_for (Fixture *fixture, const char *query)
{
	NemoIconContainer *container = fixture->container;
	if (!nemo_icon_container_get_type_jump_active (container)) {
		gtk_widget_grab_focus (GTK_WIDGET (container));
		g_assert_true (press_key (fixture, GDK_KEY_x, 0));
	}
	gtk_entry_set_text (GTK_ENTRY (container->details->search_entry), query);
	g_assert_true (nemo_icon_container_get_type_jump_active (container));
	g_assert_cmpstr (nemo_icon_container_get_type_jump_text (container, NULL), ==, query);
}

static void
assert_selected (Fixture *fixture, int index)
{
	for (guint i = 0; i < G_N_ELEMENTS (fixture->icons); i++)
		g_assert_cmpint (fixture->icons[i]->is_selected, ==, (int) i == index);
}

static gboolean
press_key (Fixture *fixture, guint keyval, GdkModifierType modifiers)
{
	GdkEvent *event = gdk_event_new (GDK_KEY_PRESS);
	event->key.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (fixture->container)));
	event->key.keyval = keyval;
	event->key.state = modifiers;
	event->key.send_event = TRUE;
	GdkKeymapKey *keys = NULL;
	int n_keys = 0;
	if (gdk_keymap_get_entries_for_keyval (gdk_keymap_get_for_display (
	    gtk_widget_get_display (fixture->window)), keyval, &keys, &n_keys)) {
		event->key.hardware_keycode = keys[0].keycode;
		event->key.group = keys[0].group;
		g_free (keys);
	}
	GdkSeat *seat = gdk_display_get_default_seat (gtk_widget_get_display (fixture->window));
	gdk_event_set_device (event, gdk_seat_get_keyboard (seat));
	gboolean handled = gtk_widget_event (GTK_WIDGET (fixture->container), event);
	gdk_event_free (event);
	return handled;
}

static void
test_ranked_navigation (Fixture *fixture, gconstpointer compact)
{
	search_for (fixture, "cur");
	assert_selected (fixture, 1);
	g_assert_true (press_key (fixture, GDK_KEY_Up, 0));
	assert_selected (fixture, 1);
	g_assert_true (press_key (fixture, GDK_KEY_Down, 0));
	assert_selected (fixture, 2);
	g_assert_true (press_key (fixture, GDK_KEY_Right, 0));
	assert_selected (fixture, 7);
	g_assert_true (press_key (fixture, GDK_KEY_KP_Right, 0));
	assert_selected (fixture, 0);
	g_assert_true (press_key (fixture, GDK_KEY_g, GDK_CONTROL_MASK));
	assert_selected (fixture, 4);
	g_assert_true (press_key (fixture, GDK_KEY_Down, 0));
	assert_selected (fixture, 6);
	g_assert_true (press_key (fixture, GDK_KEY_Down, 0));
	assert_selected (fixture, 6);
	g_assert_true (press_key (fixture, GDK_KEY_Left, 0));
	assert_selected (fixture, 4);
	g_assert_true (press_key (fixture, GDK_KEY_KP_Up, 0));
	assert_selected (fixture, 0);
	g_assert_true (press_key (fixture, GDK_KEY_G, GDK_CONTROL_MASK | GDK_SHIFT_MASK));
	assert_selected (fixture, 7);
}

static void
test_incremental_queries (Fixture *fixture, gconstpointer compact)
{
	search_for (fixture, "cu");
	assert_selected (fixture, 1);
	search_for (fixture, "CUR");
	assert_selected (fixture, 1);
	search_for (fixture, "curv");
	assert_selected (fixture, 1);
	search_for (fixture, "curvey");
	assert_selected (fixture, 2);
	search_for (fixture, "sscur");
	assert_selected (fixture, 0);
	search_for (fixture, "cvr");
	assert_selected (fixture, -1);
	g_assert_cmpint (fixture->container->details->selected_iter, ==, 0);
	press_key (fixture, GDK_KEY_Down, 0);
	assert_selected (fixture, -1);
	search_for (fixture, "curvey");
	search_for (fixture, "");
	assert_selected (fixture, -1);
	g_assert_cmpint (fixture->container->details->selected_iter, ==, 0);
	search_for (fixture, "curve-hidden");
	assert_selected (fixture, -1);
	g_settings_set_enum (nemo_preferences, NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE,
	                     NEMO_INTERACTIVE_SEARCH_MODE_PREFIX);
	search_for (fixture, "sscurve");
	assert_selected (fixture, -1);
	search_for (fixture, "cur");
	assert_selected (fixture, 1);
	press_key (fixture, GDK_KEY_Right, 0);
	press_key (fixture, GDK_KEY_Right, 0);
	press_key (fixture, GDK_KEY_Right, 0);
	assert_selected (fixture, 7);
}

static void
test_live_bindings (Fixture *fixture, gconstpointer compact)
{
	search_for (fixture, "cur");
	g_settings_set_string (nemo_keybinding_settings, "type-jump-next", "<Control>j");
	press_key (fixture, GDK_KEY_Down, 0);
	assert_selected (fixture, 1);
	g_assert_true (press_key (fixture, GDK_KEY_j, GDK_CONTROL_MASK));
	assert_selected (fixture, 2);
	g_settings_set_string (nemo_keybinding_settings, "type-jump-next-alt", "");
	press_key (fixture, GDK_KEY_Right, 0);
	assert_selected (fixture, 2);
	g_settings_set_string (nemo_keybinding_settings, "type-jump-previous", "<Control>k");
	g_assert_true (press_key (fixture, GDK_KEY_k, GDK_CONTROL_MASK));
	assert_selected (fixture, 1);
	g_settings_set_string (nemo_keybinding_settings, "type-jump-next", "");
	press_key (fixture, GDK_KEY_j, GDK_CONTROL_MASK);
	assert_selected (fixture, 1);
}

static void
assert_weight_span (PangoLayout *layout, guint start, guint end)
{
	PangoAttrIterator *iter = pango_attr_list_get_iterator (pango_layout_get_attributes (layout));
	guint count = 0;
	do {
		PangoAttribute *attr = pango_attr_iterator_get (iter, PANGO_ATTR_WEIGHT);
		if (attr != NULL) {
			g_assert_cmpuint (attr->start_index, ==, start);
			g_assert_cmpuint (attr->end_index, ==, end);
			g_assert_cmpint (((PangoAttrInt *) attr)->value, ==, PANGO_WEIGHT_BOLD);
			count++;
		}
	} while (pango_attr_iterator_next (iter));
	pango_attr_iterator_destroy (iter);
	g_assert_cmpuint (count, ==, end > start ? 1 : 0);
}

static PangoLayout *
filename_layout (NemoIconCanvasItem *item)
{
	return get_label_layout (&item->details->editable_text_layout, item, item->details->editable_text);
}

static void
test_highlight_layouts (Fixture *fixture, gconstpointer compact)
{
	NemoIconCanvasItem *item = fixture->icons[6]->item;
	search_for (fixture, "SS-曲.CU");
	assert_selected (fixture, 6);
	PangoLayout *layout = filename_layout (item);
	const char *display = pango_layout_get_text (layout);
	g_assert_cmpstr (display, ==, "é_" ZERO_WIDTH_SPACE "ß-" ZERO_WIDTH_SPACE
	                  "曲." ZERO_WIDTH_SPACE "curve.7." ZERO_WIDTH_SPACE "txt");
	assert_weight_span (layout, 6, 21);
	g_object_unref (layout);
	search_for (fixture, "CUR");
	layout = filename_layout (item);
	assert_weight_span (layout, 19, 22);
	g_object_unref (layout);
	layout = get_label_layout (&item->details->additional_text_layout, item, item->details->additional_text);
	assert_weight_span (layout, 0, 0);
	g_object_unref (layout);
	item->details->is_pinned = TRUE;
	nemo_icon_canvas_item_invalidate_label (item);
	layout = filename_layout (item);
	g_assert_cmpint (pango_font_description_get_weight (pango_layout_get_font_description (layout)),
	                 ==, PINNED_TEXT_WEIGHT);
	assert_weight_span (layout, 19, 22);
	g_object_unref (layout);
	item->details->fav_unavailable = TRUE;
	nemo_icon_canvas_item_invalidate_label (item);
	layout = filename_layout (item);
	g_assert_cmpint (pango_font_description_get_weight (pango_layout_get_font_description (layout)),
	                 ==, UNAVAILABLE_TEXT_WEIGHT);
	assert_weight_span (layout, 19, 22);
	g_object_unref (layout);
}

static void
assert_search_cleared (Fixture *fixture)
{
	NemoIconContainer *container = fixture->container;
	g_assert_false (nemo_icon_container_get_type_jump_active (container));
	g_assert_null (nemo_icon_container_get_type_jump_text (container, NULL));
	g_assert_cmpint (container->details->selected_iter, ==, 0);
	g_assert_cmpuint (container->details->typeselect_flush_timeout, ==, 0);
	PangoLayout *layout = filename_layout (fixture->icons[2]->item);
	assert_weight_span (layout, 0, 0);
	g_object_unref (layout);
}

static void
test_clearing (Fixture *fixture, gconstpointer compact)
{
	search_for (fixture, "cur");
	PangoLayout *layout = filename_layout (fixture->icons[2]->item);
	g_object_unref (layout);
	g_assert_true (press_key (fixture, GDK_KEY_Escape, 0));
	assert_search_cleared (fixture);
	search_for (fixture, "cur");
	gtk_widget_hide (fixture->container->details->search_window);
	assert_search_cleared (fixture);
	search_for (fixture, "cur");
	gtk_widget_hide (GTK_WIDGET (fixture->container));
	assert_search_cleared (fixture);
	gtk_widget_show (GTK_WIDGET (fixture->container));
	search_for (fixture, "cur");
	GdkEvent *event = gdk_event_new (GDK_BUTTON_PRESS);
	event->button.window = g_object_ref (gtk_widget_get_window (fixture->container->details->search_window));
	event->button.button = 1;
	event->button.send_event = TRUE;
	gtk_widget_event (fixture->container->details->search_window, event);
	gdk_event_free (event);
	assert_search_cleared (fixture);
	search_for (fixture, "cur");
	gtk_widget_destroy (fixture->container->details->search_window);
	g_assert_null (fixture->container->details->search_entry);
	assert_search_cleared (fixture);
	search_for (fixture, "cur");
	nemo_icon_container_clear (fixture->container);
	g_assert_false (nemo_icon_container_get_type_jump_active (fixture->container));
	g_assert_cmpuint (fixture->container->details->typeselect_flush_timeout, ==, 0);
}

static void
test_mode_change (Fixture *fixture, gconstpointer compact)
{
	search_for (fixture, "sscur");
	assert_selected (fixture, 0);
	PangoLayout *layout = filename_layout (fixture->icons[0]->item);
	assert_weight_span (layout, 2, 7);
	g_object_unref (layout);

	g_settings_set_enum (nemo_preferences, NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE,
	                     NEMO_INTERACTIVE_SEARCH_MODE_PREFIX);
	assert_search_cleared (fixture);
	layout = filename_layout (fixture->icons[0]->item);
	assert_weight_span (layout, 0, 0);
	g_object_unref (layout);
	search_for (fixture, "sscur");
	assert_selected (fixture, -1);
	search_for (fixture, "cur");
	assert_selected (fixture, 1);
	g_settings_set_enum (nemo_preferences, NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE,
	                     NEMO_INTERACTIVE_SEARCH_MODE_FILTER);
	assert_search_cleared (fixture);
	g_settings_set_enum (nemo_preferences, NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE,
	                     NEMO_INTERACTIVE_SEARCH_MODE_SUBSTRING);
	assert_search_cleared (fixture);
	search_for (fixture, "sscur");
	assert_selected (fixture, 0);
}

static void
test_destroy_active (Fixture *fixture, gconstpointer compact)
{
	NemoIconContainer *container = fixture->container;
	search_for (fixture, "cur");
	GtkWidget *menu = gtk_menu_new ();
	g_object_ref_sink (menu);
	g_signal_emit_by_name (container->details->search_entry, "populate-popup", menu);
	g_assert_cmpuint (container->details->typeselect_flush_timeout, ==, 0);
	gtk_widget_destroy (GTK_WIDGET (container));
	g_assert_false (nemo_icon_container_get_type_jump_active (container));
	g_assert_null (container->details->search_window);
	g_assert_null (container->details->search_entry);
	g_assert_cmpint (container->details->selected_iter, ==, 0);
	g_signal_emit_by_name (menu, "hide");
	g_assert_cmpuint (container->details->typeselect_flush_timeout, ==, 0);
	gtk_widget_destroy (menu);
	g_object_unref (menu);
}

static void
test_existing_arrows (Fixture *fixture, gconstpointer compact)
{
	search_for (fixture, "curvey");
	assert_selected (fixture, 2);
	press_key (fixture, GDK_KEY_Escape, 0);
	g_settings_set_string (nemo_keybinding_settings, "type-jump-next-alt", "");
	for (guint i = 0; i < G_N_ELEMENTS (fixture->icons); i++) {
		fixture->icons[i]->x = fixture->icons[i]->item->details->x;
		fixture->icons[i]->y = fixture->icons[i]->item->details->y;
	}
	g_assert_true (press_key (fixture, GDK_KEY_Right, 0));
	assert_selected (fixture, 3);
	g_assert_false (nemo_icon_container_get_type_jump_active (fixture->container));
}

static void
test_filter_highlights (Fixture *fixture, gconstpointer compact)
{
	NemoIconContainer *container = fixture->container;
	NemoIconCanvasItem *item = fixture->icons[3]->item;
	g_settings_set_enum (nemo_preferences, NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE,
	                     NEMO_INTERACTIVE_SEARCH_MODE_FILTER);
	container->details->filter_highlight_text = g_strdup ("cur");
	nemo_icon_canvas_item_invalidate_label (item);
	PangoLayout *layout = filename_layout (item);
	PangoAttrList *expected = nemo_fzy_match_attrs ("cur", pango_layout_get_text (layout));
	g_assert_nonnull (expected);
	PangoAttrIterator *iter = pango_attr_list_get_iterator (expected);
	guint count = 0;
	do {
		PangoAttribute *attr = pango_attr_iterator_get (iter, PANGO_ATTR_WEIGHT);
		if (attr != NULL) {
			PangoAttrIterator *actual = pango_attr_list_get_iterator (pango_layout_get_attributes (layout));
			while (TRUE) {
				int start, end;
				pango_attr_iterator_range (actual, &start, &end);
				if ((guint) end > attr->start_index)
					break;
				g_assert_true (pango_attr_iterator_next (actual));
			}
			PangoAttribute *weight = pango_attr_iterator_get (actual, PANGO_ATTR_WEIGHT);
			g_assert_nonnull (weight);
			g_assert_cmpuint (weight->start_index, ==, attr->start_index);
			g_assert_cmpuint (weight->end_index, ==, attr->end_index);
			pango_attr_iterator_destroy (actual);
			count++;
		}
	} while (pango_attr_iterator_next (iter));
	g_assert_cmpuint (count, ==, 3);
	pango_attr_iterator_destroy (iter);
	pango_attr_list_unref (expected);
	g_object_unref (layout);
	g_clear_pointer (&container->details->filter_highlight_text, g_free);
	nemo_icon_canvas_item_invalidate_label (item);
	layout = filename_layout (item);
	assert_weight_span (layout, 0, 0);
	g_object_unref (layout);
}

static void
test_timeout (Fixture *fixture, gconstpointer compact)
{
	search_for (fixture, "cur");
	gint64 deadline = g_get_monotonic_time () + 7 * G_TIME_SPAN_SECOND;
	while (nemo_icon_container_get_type_jump_active (fixture->container) &&
	       g_get_monotonic_time () < deadline) {
		g_main_context_iteration (NULL, FALSE);
		g_usleep (1000);
	}
	assert_search_cleared (fixture);
}

static GBytes *
render_filename (NemoIconCanvasItem *item)
{
	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 400, 200);
	cairo_t *cr = cairo_create (surface);
	cairo_set_source_rgb (cr, 0.125, 0.125, 0.125);
	cairo_paint (cr);
	EelIRect rect = { 80, 20, 128, 68 };
	draw_label_text (item, cr, rect);
	cairo_destroy (cr);
	cairo_surface_flush (surface);
	GBytes *bytes = g_bytes_new (cairo_image_surface_get_data (surface),
	                            cairo_image_surface_get_stride (surface) * 200);
	cairo_surface_destroy (surface);
	return bytes;
}

static void
test_file_colors (Fixture *fixture, gconstpointer compact)
{
	GtkCssProvider *provider = gtk_css_provider_new ();
	gtk_css_provider_load_from_data (provider,
		"* { background-color: #202020; color: white; }"
		"*:selected { background-color: #336699; color: white; }", -1, NULL);
	gtk_style_context_add_provider (gtk_widget_get_style_context (GTK_WIDGET (fixture->container)),
	                                GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	NemoIconCanvasItem *item = fixture->icons[1]->item;
	nemo_icon_canvas_item_set_file_color_kind (item, NEMO_FILE_COLOR_DOCUMENT);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, TRUE);
	GBytes *colored = render_filename (item);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, FALSE);
	GBytes *plain = render_filename (item);
	g_assert_false (g_bytes_equal (colored, plain));
	g_bytes_unref (colored);
	g_bytes_unref (plain);
	g_object_set (item, "highlighted-for-selection", TRUE, NULL);
	plain = render_filename (item);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, TRUE);
	colored = render_filename (item);
	g_assert_true (g_bytes_equal (colored, plain));
	g_bytes_unref (colored);
	g_bytes_unref (plain);
	PangoLayout *layout = filename_layout (item);
	PangoAttrIterator *attrs = pango_attr_list_get_iterator (pango_layout_get_attributes (layout));
	g_assert_null (pango_attr_iterator_get (attrs, PANGO_ATTR_FOREGROUND));
	pango_attr_iterator_destroy (attrs);
	g_object_unref (layout);
	g_object_unref (provider);
}

static guint8
peak_grey_channel (GBytes *pixels)
{
	gsize length;
	const guint8 *data = g_bytes_get_data (pixels, &length);
	guint8 peak = 0;
	for (gsize i = G_BYTE_ORDER == G_LITTLE_ENDIAN ? 0 : 1; i < length; i += 4)
		peak = MAX (peak, data[i]);
	return peak;
}

static void
test_grey_captions (Fixture *fixture, gconstpointer compact)
{
	GtkCssProvider *provider = gtk_css_provider_new ();
	gtk_css_provider_load_from_data (provider,
		"* { background-color: black; color: #888; }"
		".dim-label { color: rgba(128,128,128,0.6); }", -1, NULL);
	gtk_style_context_add_provider (gtk_widget_get_style_context (GTK_WIDGET (fixture->container)),
	                                GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	NemoIconCanvasItem *item = fixture->icons[1]->item;
	g_object_set (item, "editable-text", "", "additional-text", "Grey details", NULL);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, FALSE);
	GBytes *plain = render_filename (item);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, TRUE);
	GBytes *bright = render_filename (item);
	g_assert_cmpuint (peak_grey_channel (bright), >, 190);
	g_assert_cmpuint (peak_grey_channel (plain), <, 150);
	g_bytes_unref (plain);
	g_bytes_unref (bright);
	g_object_set (item, "highlighted-for-selection", TRUE, NULL);
	bright = render_filename (item);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, FALSE);
	plain = render_filename (item);
	g_assert_true (g_bytes_equal (plain, bright));
	g_bytes_unref (plain);
	g_bytes_unref (bright);
	g_object_set (item, "highlighted-for-selection", FALSE, NULL);
	gtk_css_provider_load_from_data (provider, "* { background-color: black; color: lime; }", -1, NULL);
	plain = render_filename (item);
	g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, TRUE);
	bright = render_filename (item);
	g_assert_true (g_bytes_equal (plain, bright));
	g_bytes_unref (plain);
	g_bytes_unref (bright);
	g_object_unref (provider);
}

int
main (int argc, char **argv)
{
	g_assert_cmpstr (g_getenv ("NEMO_TEST_ISOLATED"), ==, "1");
	gtk_test_init (&argc, &argv, NULL);
	nemo_global_preferences_init ();
	g_test_add ("/icon-type-jump/ranked", Fixture, NULL, fixture_setup, test_ranked_navigation, fixture_teardown);
	g_test_add ("/compact-type-jump/ranked", Fixture, GINT_TO_POINTER (1), fixture_setup, test_ranked_navigation, fixture_teardown);
	g_test_add ("/icon-type-jump/incremental", Fixture, NULL, fixture_setup, test_incremental_queries, fixture_teardown);
	g_test_add ("/icon-type-jump/live-bindings", Fixture, NULL, fixture_setup, test_live_bindings, fixture_teardown);
	g_test_add ("/icon-type-jump/highlights", Fixture, NULL, fixture_setup, test_highlight_layouts, fixture_teardown);
	g_test_add ("/compact-type-jump/highlights", Fixture, GINT_TO_POINTER (1), fixture_setup, test_highlight_layouts, fixture_teardown);
	g_test_add ("/icon-type-jump/clearing", Fixture, NULL, fixture_setup, test_clearing, fixture_teardown);
	g_test_add ("/icon-type-jump/mode-change", Fixture, NULL, fixture_setup, test_mode_change, fixture_teardown);
	g_test_add ("/compact-type-jump/mode-change", Fixture, GINT_TO_POINTER (1), fixture_setup, test_mode_change, fixture_teardown);
	g_test_add ("/icon-type-jump/destroy", Fixture, NULL, fixture_setup, test_destroy_active, fixture_teardown);
	g_test_add ("/icon-type-jump/existing-arrows", Fixture, NULL, fixture_setup, test_existing_arrows, fixture_teardown);
	g_test_add ("/compact-type-jump/existing-arrows", Fixture, GINT_TO_POINTER (1), fixture_setup, test_existing_arrows, fixture_teardown);
	g_test_add ("/icon-type-jump/filter-highlights", Fixture, NULL, fixture_setup, test_filter_highlights, fixture_teardown);
	g_test_add ("/icon-type-jump/timeout", Fixture, NULL, fixture_setup, test_timeout, fixture_teardown);
	g_test_add ("/icon-type-jump/file-colors", Fixture, NULL, fixture_setup, test_file_colors, fixture_teardown);
	g_test_add ("/compact-type-jump/file-colors", Fixture, GINT_TO_POINTER (1), fixture_setup, test_file_colors, fixture_teardown);
	g_test_add ("/icon-type-jump/grey-captions", Fixture, NULL, fixture_setup, test_grey_captions, fixture_teardown);
	g_test_add ("/compact-type-jump/grey-captions", Fixture, GINT_TO_POINTER (1), fixture_setup, test_grey_captions, fixture_teardown);
	return g_test_run ();
}
