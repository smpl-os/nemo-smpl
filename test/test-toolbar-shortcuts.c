#include <config.h>
#include "window-test-fixture.h"
#include "../src/nemo-actions.h"
#include "../src/nemo-keybindings.h"
#include "../src/nemo-toolbar.h"
#include "../src/nemo-notebook.h"

static char *rendered_tooltip;
static gboolean send_control_arrow (NemoWindow *window, guint key);

void __real_gtk_tooltip_set_text (GtkTooltip *tooltip, const char *text);
void
__wrap_gtk_tooltip_set_text (GtkTooltip *tooltip, const char *text)
{
    g_free (rendered_tooltip);
    rendered_tooltip = g_strdup (text);
    if (tooltip != NULL)
        __real_gtk_tooltip_set_text (tooltip, text);
}

typedef struct { const char *action; GtkWidget *button; } ButtonSearch;

static void
find_button (GtkWidget *widget, gpointer data)
{
    ButtonSearch *search = data;
    if (GTK_IS_BUTTON (widget)) {
        GtkAction *action = gtk_activatable_get_related_action (GTK_ACTIVATABLE (widget));
        if (action != NULL && g_str_equal (gtk_action_get_name (action), search->action))
            search->button = widget;
    }
    if (search->button == NULL && GTK_IS_CONTAINER (widget))
        gtk_container_forall (GTK_CONTAINER (widget), find_button, data);
}

static GtkWidget *
button_for (Fixture *fixture, const char *action)
{
    ButtonSearch search = { action, NULL };
    find_button (fixture->slot->pane->tool_bar, &search);
    g_assert_nonnull (search.button);
    return search.button;
}

static const char *
query_tooltip (GtkWidget *button)
{
    g_clear_pointer (&rendered_tooltip, g_free);
    gboolean handled = FALSE;
    g_signal_emit_by_name (button, "query-tooltip", 0, 0, FALSE, NULL, &handled);
    g_assert_true (handled);
    g_assert_nonnull (rendered_tooltip);
    return rendered_tooltip;
}

static void
assert_shortcut (const char *text, const char *accelerator, gboolean present)
{
    guint key;
    GdkModifierType mods;
    gtk_accelerator_parse (accelerator, &key, &mods);
    g_autofree char *label = gtk_accelerator_get_label (key, mods);
    if ((strstr (text, label) != NULL) != present)
        g_printerr ("Tooltip '%s' %s shortcut '%s'\n", text, present ? "must contain" : "must omit", label);
    g_assert_cmpint (strstr (text, label) != NULL, ==, present);
}

static void
test_live_tooltips (void)
{
    Fixture fixture = fixture_new ();
    GtkWidget *back = button_for (&fixture, NEMO_ACTION_BACK);
    const char *text = query_tooltip (back);
    g_assert_nonnull (strstr (text, "\nShortcut: "));
    assert_shortcut (text, "<Alt>Left", TRUE);
    assert_shortcut (text, "<Control>Left", TRUE);
    text = query_tooltip (button_for (&fixture, NEMO_ACTION_FORWARD));
    assert_shortcut (text, "<Alt>Right", TRUE);
    assert_shortcut (text, "<Control>Right", TRUE);
    text = query_tooltip (button_for (&fixture, NEMO_ACTION_UP));
    assert_shortcut (text, "<Alt>Up", TRUE);
    assert_shortcut (text, "<Control>Up", TRUE);
    assert_shortcut (text, "BackSpace", TRUE);

    g_settings_set_string (nemo_keybinding_settings, "go-back", "<Control><Shift>b");
    text = query_tooltip (back);
    assert_shortcut (text, "<Control><Shift>b", TRUE);
    assert_shortcut (text, "<Alt>Left", FALSE);
    g_settings_set_string (nemo_keybinding_settings, "go-back-alt", "");
    text = query_tooltip (back);
    assert_shortcut (text, "<Control>Left", FALSE);
    GtkAction *action = gtk_activatable_get_related_action (GTK_ACTIVATABLE (back));
    gtk_action_set_tooltip (action, "Previous test folder");
    text = query_tooltip (back);
    g_assert_true (g_str_has_prefix (text, "Previous test folder\n"));
    assert_shortcut (text, "<Control><Shift>b", TRUE);
    g_settings_set_string (nemo_keybinding_settings, "go-back", "");
    g_assert_cmpstr (query_tooltip (back), ==, "Previous test folder");

    text = query_tooltip (button_for (&fixture, NEMO_ACTION_TOGGLE_LOCATION));
    assert_shortcut (text, "<Control>l", TRUE);
    g_settings_set_string (nemo_keybinding_settings, "edit-location", "<Control><Alt>l");
    text = query_tooltip (button_for (&fixture, NEMO_ACTION_TOGGLE_LOCATION));
    assert_shortcut (text, "<Control><Alt>l", TRUE);

    gtk_accel_map_change_entry ("<Actions>/ShellActions/Forward", GDK_KEY_f,
                                GDK_CONTROL_MASK | GDK_MOD1_MASK, TRUE);
    text = query_tooltip (button_for (&fixture, NEMO_ACTION_FORWARD));
    assert_shortcut (text, "<Control><Alt>f", TRUE);
    assert_shortcut (text, "<Alt>Right", FALSE);

    GtkWidget *home = button_for (&fixture, NEMO_ACTION_HOME);
    g_settings_set_string (nemo_keybinding_settings, "go-home", "");
    action = gtk_activatable_get_related_action (GTK_ACTIVATABLE (home));
    gtk_action_set_tooltip (action, NULL);
    g_assert_true (gtk_widget_get_has_tooltip (home));
    g_assert_cmpstr (query_tooltip (home), ==, "Home");

    GtkAction *up = gtk_activatable_get_related_action (
        GTK_ACTIVATABLE (button_for (&fixture, NEMO_ACTION_UP)));
    gtk_action_set_accel_path (up, "<Actions>/ShellActions/Up");
    text = query_tooltip (button_for (&fixture, NEMO_ACTION_UP));
    guint key;
    GdkModifierType mods;
    gtk_accelerator_parse ("<Alt>Up", &key, &mods);
    g_autofree char *label = gtk_accelerator_get_label (key, mods);
    const char *first = strstr (text, label);
    g_assert_nonnull (first);
    g_assert_null (strstr (first + strlen (label), label));
    fixture_clear (&fixture);
}

static GtkAction *
find_action (NemoWindow *window, const char *name)
{
    for (GList *l = gtk_ui_manager_get_action_groups (nemo_window_get_ui_manager (window)); l; l = l->next) {
        GtkAction *action = gtk_action_group_get_action (l->data, name);
        if (action != NULL)
            return action;
    }
    return NULL;
}

static void
select_file (NemoView *view, GFile *location)
{
    NemoFile *file = nemo_file_get (location);
    GList selection = { .data = file };
    nemo_view_set_selection (view, &selection);
    nemo_file_unref (file);
}

static void
test_control_navigation (void)
{
    Fixture fixture = fixture_new ();
    GtkAction *down = find_action (fixture.window, NEMO_ACTION_OPEN_SELECTED_FOLDER);
    g_assert_nonnull (down);
    WAIT_FOR (!gtk_action_get_sensitive (find_action (fixture.window, NEMO_ACTION_BACK_ALTERNATE)));
    g_assert_false (key_press (fixture.window, "<Control>Left"));
    send_control_arrow (fixture.window, GDK_KEY_Left);
    g_assert_null (fixture.slot->pending_location);
    select_file (fixture.slot->content_view, fixture.containing);
    WAIT_FOR (gtk_action_get_sensitive (down));
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_ALWAYS_USE_BROWSER, FALSE);
    g_assert_true (send_control_arrow (fixture.window, GDK_KEY_Down));
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    g_assert_cmpuint (g_list_length (gtk_application_get_windows (GTK_APPLICATION (application))), ==, 1);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_ALWAYS_USE_BROWSER, TRUE);
    g_assert_true (send_control_arrow (fixture.window, GDK_KEY_Up));
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    g_assert_true (send_control_arrow (fixture.window, GDK_KEY_Left));
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    g_assert_true (send_control_arrow (fixture.window, GDK_KEY_Right));
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    g_assert_true (key_press (fixture.window, "<Alt>Left"));
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    select_file (fixture.slot->content_view, fixture.item);
    down = find_action (fixture.window, NEMO_ACTION_OPEN_SELECTED_FOLDER);
    WAIT_FOR (!gtk_action_get_sensitive (down));
    g_assert_false (key_press (fixture.window, "<Control>Down"));
    g_assert_true (slot_at (fixture.slot, fixture.containing));

    g_settings_set_string (nemo_keybinding_settings, "go-up-secondary", "<Control><Alt>u");
    g_assert_false (key_press (fixture.window, "<Control>Up"));
    g_assert_true (key_press (fixture.window, "<Control><Alt>u"));
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    g_settings_reset (nemo_keybinding_settings, "go-up-secondary");
    g_assert_false (key_press (fixture.window, "<Control><Alt>u"));

    select_file (fixture.slot->content_view, fixture.containing);
    down = find_action (fixture.window, NEMO_ACTION_OPEN_SELECTED_FOLDER);
    WAIT_FOR (gtk_action_get_sensitive (down));
    g_settings_set_string (nemo_keybinding_settings, "go-down", "<Control><Alt>j");
    g_assert_false (key_press (fixture.window, "<Control>Down"));
    g_assert_true (key_press (fixture.window, "<Control><Alt>j"));
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    g_assert_true (send_control_arrow (fixture.window, GDK_KEY_Up));
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    fixture_clear (&fixture);
}

static gboolean
send_control_arrow (NemoWindow *window, guint key)
{
    GdkKeymapKey *entries = NULL;
    gint count;
    GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (window));
    g_assert_true (gdk_keymap_get_entries_for_keyval (gdk_keymap_get_for_display (display), key, &entries, &count));
    GdkEvent *event = gdk_event_new (GDK_KEY_PRESS);
    event->key.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (window)));
    event->key.keyval = key;
    event->key.state = GDK_CONTROL_MASK;
    event->key.hardware_keycode = entries[0].keycode;
    event->key.group = entries[0].group;
    gdk_event_set_device (event, gdk_seat_get_keyboard (gdk_display_get_default_seat (display)));
    gboolean handled = gtk_widget_event (GTK_WIDGET (window), event);
    gdk_event_free (event);
    g_free (entries);
    return handled;
}

static void
test_editing_keeps_control_arrows (void)
{
    Fixture fixture = fixture_new ();
    select_file (fixture.slot->content_view, fixture.containing);
    GtkAction *down = find_action (fixture.window, NEMO_ACTION_OPEN_SELECTED_FOLDER);
    WAIT_FOR (gtk_action_get_sensitive (down));
    g_assert_true (key_press (fixture.window, "<Control>l"));
    GtkWidget *entry = gtk_window_get_focus (GTK_WINDOW (fixture.window));
    g_assert_true (GTK_IS_ENTRY (entry));
    gtk_entry_set_text (GTK_ENTRY (entry), "alpha beta");
    gtk_editable_set_position (GTK_EDITABLE (entry), 5);
    const guint keys[] = { GDK_KEY_Left, GDK_KEY_Right, GDK_KEY_Up, GDK_KEY_Down };
    for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
        g_assert_true (send_control_arrow (fixture.window, keys[i]));
        g_assert_null (fixture.slot->pending_location);
        g_assert_true (g_file_equal (fixture.slot->location, fixture.origin));
        if (i < 2)
            g_assert_cmpint (gtk_editable_get_position (GTK_EDITABLE (entry)), ==, i == 0 ? 0 : 5);
    }
    fixture_clear (&fixture);
}

static void
test_existing_binding_wins (void)
{
    g_autofree char *preview_up = g_settings_get_string (nemo_keybinding_settings, "preview-scroll-up");
    g_autofree char *preview_down = g_settings_get_string (nemo_keybinding_settings, "preview-scroll-down");
    g_autofree char *new_tab = g_settings_get_string (nemo_keybinding_settings, "new-tab");
    g_autofree char *new_folder = g_settings_get_string (nemo_keybinding_settings, "new-folder");
    g_assert_cmpstr (preview_up, ==, "");
    g_assert_cmpstr (preview_down, ==, "<Alt>j");
    g_assert_cmpstr (new_tab, ==, "<Alt>Page_Up");
    g_assert_cmpstr (new_folder, ==, "<Alt>Page_Down");
    g_autofree char *binding = g_settings_get_string (nemo_keybinding_settings, "go-back-alt");
    g_assert_cmpstr (binding, ==, "");
    Fixture fixture = fixture_new ();
    GtkAccelKey key;
    g_assert_true (gtk_accel_map_lookup_entry ("<Actions>/ShellActions/BackAlternate", &key));
    g_assert_cmpuint (key.accel_key, ==, 0);
    g_assert_true (gtk_accel_map_lookup_entry ("<Actions>/ShellActions/New Window", &key));
    g_assert_cmpuint (key.accel_key, ==, GDK_KEY_Left);
    g_assert_cmpuint (key.accel_mods, ==, GDK_CONTROL_MASK);
    assert_shortcut (query_tooltip (button_for (&fixture, NEMO_ACTION_BACK)), "<Control>Left", FALSE);
    fixture_clear (&fixture);
}

static void
test_preview_scroll_bindings (void)
{
    const char *keys[] = { "preview-scroll-up", "preview-scroll-down" };
    const char *defaults[] = { "<Alt>Page_Up", "<Alt>Page_Down" };
    const char *labels[] = { "Scroll Preview Up", "Scroll Preview Down" };
    for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
        const NemoKeybindingEntry *entry = NULL;
        for (gint j = 0; j < nemo_keybinding_entries_count; j++)
            if (g_str_equal (nemo_keybinding_entries[j].settings_key, keys[i]))
                entry = &nemo_keybinding_entries[j];
        g_assert_nonnull (entry);
        g_assert_cmpstr (entry->category, ==, "Preview");
        g_assert_cmpstr (entry->description, ==, labels[i]);
        g_assert_cmpstr (entry->default_accel, ==, defaults[i]);
        g_assert_null (entry->accel_path);
        g_assert_null (entry->binding_set_name);
        GVariant *schema_default = g_settings_get_default_value (nemo_keybinding_settings, keys[i]);
        g_assert_cmpstr (g_variant_get_string (schema_default, NULL), ==, defaults[i]);
        g_variant_unref (schema_default);
        g_autofree char *value = g_settings_get_string (nemo_keybinding_settings, keys[i]);
        g_assert_cmpstr (value, ==, defaults[i]);
        nemo_keybindings_set_for_action (keys[i], "<Alt>j");
        g_clear_pointer (&value, g_free);
        value = g_settings_get_string (nemo_keybinding_settings, keys[i]);
        g_assert_cmpstr (value, ==, "<Alt>j");
        nemo_keybindings_set_for_action (keys[i], "");
        g_clear_pointer (&value, g_free);
        value = g_settings_get_string (nemo_keybinding_settings, keys[i]);
        g_assert_cmpstr (value, ==, "");
        g_settings_reset (nemo_keybinding_settings, keys[i]);
        g_clear_pointer (&value, g_free);
        value = g_settings_get_string (nemo_keybinding_settings, keys[i]);
        g_assert_cmpstr (value, ==, defaults[i]);
    }
}

typedef struct {
    const char *path;
    GTestFunc run;
    gboolean conflict;
    const char *view;
} ToolbarCase;

static void
divider_allocated (GtkWidget *widget, GtkAllocation *allocation, gboolean *ready)
{
    if (allocation->width > 650)
        *ready = TRUE;
}

static void
test_split_divider (void)
{
    GtkCssProvider *theme = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (theme,
        "separator { min-width: 0; min-height: 0; background-color: transparent; border: none; }", -1, NULL);
    gtk_style_context_add_provider_for_screen (gdk_screen_get_default (), GTK_STYLE_PROVIDER (theme),
                                               GTK_STYLE_PROVIDER_PRIORITY_USER);
    Fixture fixture = fixture_new ();
    nemo_window_split_view_on (fixture.window);
    GtkPaned *paned = GTK_PANED (fixture.window->details->split_view_hpane);
    g_assert_true (gtk_paned_get_wide_handle (paned));
    g_assert_true (gtk_style_context_has_class (gtk_widget_get_style_context (GTK_WIDGET (paned)),
                                               "nemo-pane-divider"));
    WAIT_FOR (gtk_paned_get_child2 (paned) != NULL &&
              gtk_widget_get_allocated_width (gtk_paned_get_child2 (paned)) > 0);
    GdkWindow *handle = gtk_paned_get_handle_window (paned);
    g_assert_nonnull (handle);
    WAIT_FOR (gdk_window_get_width (handle) >= 5);
    GtkStyleContext *separator = gtk_style_context_new ();
    GtkWidgetPath *path = gtk_widget_path_copy (gtk_widget_get_path (GTK_WIDGET (paned)));
    gtk_widget_path_append_type (path, GTK_TYPE_SEPARATOR);
    gtk_widget_path_iter_set_object_name (path, -1, "separator");
    gtk_style_context_set_path (separator, path);
    gtk_style_context_set_parent (separator, gtk_widget_get_style_context (GTK_WIDGET (paned)));
    GdkRGBA color;
    gtk_style_context_get_background_color (separator, GTK_STATE_FLAG_NORMAL, &color);
    g_assert_cmpfloat (color.alpha, ==, 0.0);
    cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 5, 32);
    cairo_t *cr = cairo_create (surface);
    gtk_render_background (separator, cr, 0, 0, 5, 32);
    cairo_destroy (cr);
    cairo_surface_flush (surface);
    const guint8 *pixels = cairo_image_surface_get_data (surface);
    guint painted_columns = 0;
    for (guint x = 0; x < 5; x++)
        if (pixels[16 * cairo_image_surface_get_stride (surface) + x * 4 +
                   (G_BYTE_ORDER == G_LITTLE_ENDIAN ? 3 : 0)] != 0)
            painted_columns++;
    g_assert_cmpuint (painted_columns, ==, 1);
    cairo_surface_destroy (surface);
    gtk_widget_path_unref (path);
    g_object_unref (separator);
    gboolean allocated = FALSE;
    gulong allocation_handler = g_signal_connect_after (paned, "size-allocate",
                                                        G_CALLBACK (divider_allocated), &allocated);
    gtk_window_resize (GTK_WINDOW (fixture.window), 1200, 720);
    WAIT_FOR (allocated);
    g_signal_handler_disconnect (paned, allocation_handler);
    gtk_paned_set_position (paned, gtk_widget_get_allocated_width (GTK_WIDGET (paned)) / 3);
    iterate ();
    g_assert_cmpint (gtk_paned_get_position (paned), <,
                     gtk_widget_get_allocated_width (GTK_WIDGET (paned)) / 2);
    fixture_clear (&fixture);
    gtk_style_context_remove_provider_for_screen (gdk_screen_get_default (), GTK_STYLE_PROVIDER (theme));
    g_object_unref (theme);
}

static void
assert_zen_panes (NemoWindow *window)
{
    for (GList *l = window->details->panes; l != NULL; l = l->next) {
        NemoWindowPane *pane = l->data;
        g_assert_true (gtk_widget_get_parent (pane->tool_bar) == GTK_WIDGET (pane));
        g_assert_true (gtk_widget_get_visible (pane->tool_bar));
        WAIT_FOR (gtk_widget_get_mapped (pane->tool_bar));
        GtkWidget *location = nemo_toolbar_get_show_location_entry (NEMO_TOOLBAR (pane->tool_bar)) ?
            pane->location_bar : pane->path_bar;
        WAIT_FOR (gtk_widget_get_mapped (location));
        g_assert_false (gtk_notebook_get_show_tabs (GTK_NOTEBOOK (pane->notebook)));
        g_assert_false (gtk_widget_get_visible (pane->pane_location_label));
    }
}

static void
test_zen_layout (void)
{
    Fixture fixture = fixture_new ();
    g_settings_set_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_MENU_BAR, TRUE);
    g_settings_set_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_STATUS_BAR, TRUE);
    g_settings_set_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_TOOLBAR, TRUE);
    nemo_window_split_view_on (fixture.window);
    nemo_window_preview_pane_on (fixture.window);
    NemoWindowSlot *other = nemo_window_get_extra_slot (fixture.window);
    WAIT_FOR (slot_at (other, fixture.origin));
    GtkWidget *transfer = gtk_label_new ("Transfer fixture");
    GtkWidget *area = nemo_window_get_transfer_area (fixture.window);
    gtk_container_add (GTK_CONTAINER (area), transfer);
    gtk_widget_show (transfer);
    gtk_widget_show_all (area);
    gtk_widget_show (area);
    WAIT_FOR (gtk_widget_get_mapped (fixture.window->details->preview_pane));
    gint preview_width = gtk_widget_get_allocated_width (fixture.window->details->preview_pane);
    gboolean sidebar = g_settings_get_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_SIDEBAR);
    g_assert_true (key_press (fixture.window, "<Alt>z"));
    g_assert_true (nemo_window_get_zen_mode (fixture.window));
    g_assert_false (gtk_widget_get_mapped (fixture.window->details->menubar));
    g_assert_false (gtk_widget_get_mapped (fixture.window->details->nemo_status_bar));
    g_assert_false (gtk_widget_get_mapped (fixture.window->details->sidebar));
    g_assert_false (gtk_widget_get_mapped (fixture.window->details->preview_pane));
    g_assert_false (gtk_widget_get_mapped (area));
    g_assert_true (gtk_widget_get_visible (area));
    g_assert_true (gtk_widget_get_parent (transfer) == area);
    assert_zen_panes (fixture.window);
    g_assert_false (gtk_widget_get_mapped (button_for (&fixture, NEMO_ACTION_BACK)));
    g_assert_true (key_press (fixture.window, "<Control>l"));
    g_assert_true (GTK_IS_ENTRY (gtk_window_get_focus (GTK_WINDOW (fixture.window))));
    nemo_window_set_active_slot (fixture.window, other);
    nemo_view_grab_focus (other->content_view);
    g_assert_true (key_press (fixture.window, "<Control>t"));
    WAIT_FOR (gtk_notebook_get_n_pages (GTK_NOTEBOOK (other->pane->notebook)) == 2);
    assert_zen_panes (fixture.window);
    nemo_window_split_view_off (fixture.window);
    assert_zen_panes (fixture.window);
    nemo_window_split_view_on (fixture.window);
    assert_zen_panes (fixture.window);
    gint width = gtk_widget_get_allocated_width (GTK_WIDGET (fixture.window)) + 150;
    gtk_window_resize (GTK_WINDOW (fixture.window), width, 720);
    WAIT_FOR (gtk_widget_get_allocated_width (GTK_WIDGET (fixture.window)) >= width);
    g_assert_true (key_press (fixture.window, "<Alt>z"));
    g_assert_false (nemo_window_get_zen_mode (fixture.window));
    WAIT_FOR (gtk_widget_get_mapped (fixture.window->details->preview_pane));
    WAIT_FOR (ABS (gtk_widget_get_allocated_width (fixture.window->details->preview_pane) - preview_width) <= 2);
    g_assert_true (gtk_widget_get_mapped (fixture.window->details->menubar));
    g_assert_true (gtk_widget_get_mapped (fixture.window->details->nemo_status_bar));
    WAIT_FOR (gtk_widget_get_mapped (area));
    g_assert_true (g_settings_get_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_MENU_BAR));
    g_assert_true (g_settings_get_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_STATUS_BAR));
    g_assert_true (g_settings_get_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_TOOLBAR));
    g_assert_cmpint (g_settings_get_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_SIDEBAR), ==, sidebar);
    for (GList *l = fixture.window->details->panes; l != NULL; l = l->next) {
        NemoWindowPane *pane = l->data;
        g_assert_true (gtk_widget_get_parent (pane->tool_bar) == fixture.window->details->toolbar_holder);
        g_assert_cmpint (gtk_notebook_get_show_tabs (GTK_NOTEBOOK (pane->notebook)), ==,
                         gtk_notebook_get_n_pages (GTK_NOTEBOOK (pane->notebook)) > 1);
    }
    gtk_widget_destroy (transfer);
    gtk_widget_hide (area);
    fixture_clear (&fixture);
}

static void
test_zen_hidden_preferences_and_shortcut (void)
{
    Fixture fixture = fixture_new ();
    g_settings_set_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_MENU_BAR, FALSE);
    g_settings_set_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_STATUS_BAR, FALSE);
    g_settings_set_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_TOOLBAR, FALSE);
    g_assert_true (key_press (fixture.window, "<Alt>z"));
    assert_zen_panes (fixture.window);
    g_assert_true (key_press (fixture.window, "<Control>l"));
    g_assert_true (GTK_IS_ENTRY (gtk_window_get_focus (GTK_WINDOW (fixture.window))));
    g_settings_set_string (nemo_keybinding_settings, "toggle-zen-mode", "<Control><Alt>z");
    g_assert_false (key_press (fixture.window, "<Alt>z"));
    g_assert_true (key_press (fixture.window, "<Control><Alt>z"));
    g_assert_false (nemo_window_get_zen_mode (fixture.window));
    g_assert_false (gtk_widget_get_mapped (fixture.slot->pane->tool_bar));
    g_assert_false (g_settings_get_boolean (nemo_window_state, NEMO_WINDOW_STATE_START_WITH_STATUS_BAR));
    nemo_window_set_zen_mode (fixture.window, TRUE);
    g_settings_set_string (nemo_keybinding_settings, "toggle-zen-mode", "");
    GtkWidget *exit_item = gtk_ui_manager_get_widget (nemo_window_get_ui_manager (fixture.window),
                                                      "/background/Zen Mode");
    g_assert_nonnull (exit_item);
    gtk_menu_item_activate (GTK_MENU_ITEM (exit_item));
    g_assert_false (nemo_window_get_zen_mode (fixture.window));
    fixture_clear (&fixture);
}

static void
test_zen_window_scope (void)
{
    Fixture first = fixture_new ();
    Fixture second = fixture_new ();
    g_assert_true (key_press (first.window, "<Alt>z"));
    g_assert_true (nemo_window_get_zen_mode (first.window));
    g_assert_false (nemo_window_get_zen_mode (second.window));
    g_assert_true (gtk_widget_get_mapped (second.window->details->toolbar_holder));
    g_assert_true (gtk_widget_get_mapped (second.window->details->sidebar));
    g_assert_true (gtk_widget_get_parent (first.slot->pane->tool_bar) == GTK_WIDGET (first.slot->pane));
    g_assert_true (gtk_widget_get_parent (second.slot->pane->tool_bar) == second.window->details->toolbar_holder);
    fixture_clear (&first);
    fixture_clear (&second);
}

static void
run_case (gconstpointer data)
{
    const ToolbarCase *test = data;
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 20000000, 0);
        g_test_trap_assert_passed ();
        return;
    }
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    if (test->conflict) {
        nemo_global_preferences_init ();
        g_settings_set_string (nemo_keybinding_settings, "new-window", "<Control>Left");
        g_settings_set_string (nemo_keybinding_settings, "new-tab", "<Alt>Page_Up");
        g_settings_set_string (nemo_keybinding_settings, "new-folder", "<Alt>Page_Down");
        g_settings_set_string (nemo_keybinding_settings, "preview-scroll-down", "<Alt>j");
    }
    application = nemo_main_application_get_singleton ();
    GError *error = NULL;
    g_assert_true (g_application_register (G_APPLICATION (application), NULL, &error));
    g_assert_no_error (error);
    if (test->view != NULL) {
        g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_DEFAULT_FOLDER_VIEWER, test->view);
        g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_IGNORE_VIEW_METADATA, TRUE);
    }
    test->run ();
    g_clear_pointer (&rendered_tooltip, g_free);
    g_object_unref (application);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    gtk_test_init (&argc, &argv, NULL);
    static const ToolbarCase cases[] = {
        { "/toolbar/live-shortcut-tooltips", test_live_tooltips, FALSE, NULL },
        { "/toolbar/control-arrow-navigation", test_control_navigation, FALSE, "icon-view" },
        { "/toolbar/control-arrow-list", test_control_navigation, FALSE, "list-view" },
        { "/toolbar/control-arrow-compact", test_control_navigation, FALSE, "compact-view" },
        { "/toolbar/control-arrow-editing", test_editing_keeps_control_arrows, FALSE, NULL },
        { "/toolbar/existing-shortcut-preserved", test_existing_binding_wins, TRUE, NULL },
        { "/toolbar/preview-scroll-bindings", test_preview_scroll_bindings, FALSE, NULL },
        { "/toolbar/split-pane-divider", test_split_divider, FALSE, NULL },
        { "/toolbar/zen-layout", test_zen_layout, FALSE, NULL },
        { "/toolbar/zen-hidden-preferences-shortcut", test_zen_hidden_preferences_and_shortcut, FALSE, NULL },
        { "/toolbar/zen-window-scope", test_zen_window_scope, FALSE, NULL },
    };
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
        g_test_add_data_func (cases[i].path, &cases[i], run_case);
    return g_test_run ();
}
