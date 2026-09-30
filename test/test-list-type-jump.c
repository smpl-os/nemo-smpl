#include <config.h>
#include "window-test-fixture.h"
#include "../src/nemo-list-view.h"
#include "../src/nemo-keybindings.h"
#include "../src/nemo-mime-actions.h"

static const char *names[] = {
    "curveycase.txt", "misscurve.txt", "misscurve2.txt", "missile.txt", "z-last.txt"
};
static guint activations;

void
__wrap_nemo_mime_activate_files (GtkWindow *window, NemoWindowSlot *slot, GList *files,
                                 const char *directory, NemoWindowOpenFlags flags, gboolean confirm)
{
    activations++;
}

static void
find_tree (GtkWidget *widget, gpointer data)
{
    GtkTreeView **tree = data;
    if (GTK_IS_TREE_VIEW (widget))
        *tree = GTK_TREE_VIEW (widget);
    else if (*tree == NULL && GTK_IS_CONTAINER (widget))
        gtk_container_forall (GTK_CONTAINER (widget), find_tree, data);
}

static gboolean
named_iter (GtkTreeView *tree, const char *name, GtkTreeIter *iter)
{
    GtkTreeModel *model = gtk_tree_view_get_model (tree);
    gboolean valid = gtk_tree_model_get_iter_first (model, iter);
    while (valid) {
        char *candidate = NULL;
        gtk_tree_model_get (model, iter, gtk_tree_view_get_search_column (tree), &candidate, -1);
        gboolean match = g_strcmp0 (candidate, name) == 0;
        g_free (candidate);
        if (match)
            return TRUE;
        valid = gtk_tree_model_iter_next (model, iter);
    }
    return FALSE;
}

static gboolean
selected (NemoView *view, const char *name)
{
    GList *files = nemo_view_get_selection (view);
    gboolean matches = FALSE;
    if (g_list_length (files) == 1) {
        GFile *file = nemo_file_get_location (files->data);
        char *basename = g_file_get_basename (file);
        matches = g_strcmp0 (basename, name) == 0;
        g_free (basename);
        g_object_unref (file);
    }
    nemo_file_list_free (files);
    return matches;
}

static Fixture
list_fixture (GtkTreeView **tree)
{
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_DEFAULT_FOLDER_VIEWER, "list-view");
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_IGNORE_VIEW_METADATA, TRUE);
    Fixture fixture = fixture_new ();
    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        GFile *file = g_file_get_child (fixture.origin, names[i]);
        char *path = g_file_get_path (file);
        g_assert_true (g_file_set_contents (path, "type-to-jump fixture", -1, NULL));
        g_free (path);
        g_object_unref (file);
    }
    nemo_window_slot_set_content_view (fixture.slot, NEMO_LIST_VIEW_ID);
    WAIT_FOR (NEMO_IS_LIST_VIEW (fixture.slot->content_view) &&
              !nemo_view_get_loading (fixture.slot->content_view));
    find_tree (GTK_WIDGET (fixture.slot->content_view), tree);
    g_assert_nonnull (*tree);
    GtkTreeIter iter;
    WAIT_FOR (named_iter (*tree, "z-last.txt", &iter));
    nemo_view_grab_focus (fixture.slot->content_view);
    return fixture;
}

static void
list_fixture_clear (Fixture *fixture)
{
    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        GFile *file = g_file_get_child (fixture->origin, names[i]);
        g_assert_true (g_file_delete (file, NULL, NULL));
        g_object_unref (file);
    }
    fixture_clear (fixture);
}

static GtkEntry *
begin_jump (Fixture *fixture)
{
    g_assert_true (key_press (fixture->window, "c"));
    WAIT_FOR (nemo_view_get_type_jump_active (fixture->slot->content_view));
    GtkWidget *entry = gtk_window_get_focus (GTK_WINDOW (fixture->window));
    g_assert_true (GTK_IS_ENTRY (entry));
    WAIT_FOR (g_strcmp0 (gtk_entry_get_text (GTK_ENTRY (entry)), "c") == 0);
    g_assert_true (key_press (fixture->window, "u"));
    WAIT_FOR (g_strcmp0 (gtk_entry_get_text (GTK_ENTRY (entry)), "cu") == 0);
    return GTK_ENTRY (entry);
}

static GtkCellRenderer *
text_renderer (GtkTreeView *tree, const char *name, gboolean filename)
{
    GtkTreeIter iter;
    g_assert_true (named_iter (tree, name, &iter));
    GList *columns = gtk_tree_view_get_columns (tree);
    GtkCellRenderer *renderer = NULL;
    for (GList *l = columns; l != NULL; l = l->next) {
        GtkTreeViewColumn *column = l->data;
        gboolean name_column = gtk_tree_view_column_get_sort_column_id (column) == gtk_tree_view_get_search_column (tree);
        if (name_column != filename || !gtk_tree_view_column_get_visible (column))
            continue;
        gtk_tree_view_column_cell_set_cell_data (column, gtk_tree_view_get_model (tree), &iter, FALSE, FALSE);
        GList *cells = gtk_cell_layout_get_cells (GTK_CELL_LAYOUT (column));
        for (GList *c = cells; c != NULL; c = c->next)
            if (GTK_IS_CELL_RENDERER_TEXT (c->data))
                renderer = c->data;
        g_list_free (cells);
    }
    g_list_free (columns);
    g_assert_nonnull (renderer);
    return renderer;
}

static GtkCellRenderer *
filename_renderer (GtkTreeView *tree, const char *name)
{
    return text_renderer (tree, name, TRUE);
}

static PangoAttrList *
filename_attributes (GtkTreeView *tree, const char *name)
{
    PangoAttrList *attrs = NULL;
    g_object_get (filename_renderer (tree, name), "attributes", &attrs, NULL);
    return attrs;
}

static void
test_file_type_colors (void)
{
    GtkTreeView *tree = NULL;
    Fixture fixture = list_fixture (&tree);
    GtkCssProvider *provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (provider, "* { background-color: #202020; color: white; }", -1, NULL);
    gtk_style_context_add_provider (gtk_widget_get_style_context (GTK_WIDGET (tree)),
                                    GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_tree_selection_unselect_all (gtk_tree_view_get_selection (tree));
    GtkCellRenderer *renderer = filename_renderer (tree, "misscurve.txt");
    gboolean set;
    GdkRGBA *dark, *light;
    g_object_get (renderer, "foreground-set", &set, "foreground-rgba", &dark, NULL);
    g_assert_true (set);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, FALSE);
    g_object_get (filename_renderer (tree, "misscurve.txt"), "foreground-set", &set, NULL);
    g_assert_false (set);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, TRUE);
    gtk_css_provider_load_from_data (provider, "* { background-color: white; color: black; }", -1, NULL);
    g_object_get (filename_renderer (tree, "misscurve.txt"), "foreground-set", &set, "foreground-rgba", &light, NULL);
    g_assert_true (set);
    g_assert_false (gdk_rgba_equal (dark, light));
    gdk_rgba_free (dark);
    gdk_rgba_free (light);
    GtkTreeIter iter;
    g_assert_true (named_iter (tree, "misscurve.txt", &iter));
    gtk_tree_selection_select_iter (gtk_tree_view_get_selection (tree), &iter);
    g_object_get (filename_renderer (tree, "misscurve.txt"), "foreground-set", &set, NULL);
    g_assert_false (set);
    gtk_tree_selection_unselect_all (gtk_tree_view_get_selection (tree));
    begin_jump (&fixture);
    PangoAttrList *attrs = filename_attributes (tree, "misscurve.txt");
    g_assert_nonnull (attrs);
    pango_attr_list_unref (attrs);
    g_object_get (filename_renderer (tree, "misscurve.txt"), "foreground-set", &set, NULL);
    g_assert_true (set);
    g_object_unref (provider);
    list_fixture_clear (&fixture);
}

static void
test_grey_details (void)
{
    GtkTreeView *tree = NULL;
    Fixture fixture = list_fixture (&tree);
    GtkCssProvider *provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (provider, "* { background-color: black; color: #777; }", -1, NULL);
    gtk_style_context_add_provider (gtk_widget_get_style_context (GTK_WIDGET (tree)),
                                    GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_tree_selection_unselect_all (gtk_tree_view_get_selection (tree));
    gboolean set;
    GdkRGBA *color;
    g_object_get (text_renderer (tree, "misscurve.txt", FALSE),
                  "foreground-set", &set, "foreground-rgba", &color, NULL);
    g_assert_true (set);
    g_assert_cmpfloat (color->red, >=, .81);
    gdk_rgba_free (color);
    GtkTreeIter iter;
    g_assert_true (named_iter (tree, "misscurve.txt", &iter));
    gtk_tree_selection_select_iter (gtk_tree_view_get_selection (tree), &iter);
    g_object_get (text_renderer (tree, "misscurve.txt", FALSE), "foreground-set", &set, NULL);
    g_assert_false (set);
    gtk_tree_selection_unselect_all (gtk_tree_view_get_selection (tree));
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, FALSE);
    g_object_get (text_renderer (tree, "misscurve.txt", FALSE), "foreground-set", &set, NULL);
    g_assert_false (set);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, TRUE);
    gtk_css_provider_load_from_data (provider, "* { background-color: black; color: lime; }", -1, NULL);
    g_object_get (text_renderer (tree, "misscurve.txt", FALSE), "foreground-set", &set, NULL);
    g_assert_false (set);
    g_object_unref (provider);
    list_fixture_clear (&fixture);
}

static void
test_rank_and_navigation (void)
{
    GtkTreeView *tree = NULL;
    Fixture fixture = list_fixture (&tree);
    GtkEntry *entry = begin_jump (&fixture);
    WAIT_FOR (selected (fixture.slot->content_view, "curveycase.txt"));
    PangoAttrList *attrs = filename_attributes (tree, "misscurve.txt");
    g_assert_nonnull (attrs);
    PangoAttrIterator *iter = pango_attr_list_get_iterator (attrs);
    gboolean bold = FALSE;
    do {
        PangoAttribute *attr = pango_attr_iterator_get (iter, PANGO_ATTR_WEIGHT);
        if (attr != NULL && attr->start_index == 4 && attr->end_index == 6)
            bold = ((PangoAttrInt *) attr)->value == PANGO_WEIGHT_BOLD;
    } while (pango_attr_iterator_next (iter));
    g_assert_true (bold);
    pango_attr_iterator_destroy (iter);
    pango_attr_list_unref (attrs);
    g_assert_true (key_press (fixture.window, "Down"));
    WAIT_FOR (selected (fixture.slot->content_view, "misscurve.txt"));
    g_assert_true (key_press (fixture.window, "Down"));
    WAIT_FOR (selected (fixture.slot->content_view, "misscurve2.txt"));
    g_assert_true (key_press (fixture.window, "Up"));
    WAIT_FOR (selected (fixture.slot->content_view, "misscurve.txt"));
    g_assert_true (key_press (fixture.window, "Up"));
    WAIT_FOR (selected (fixture.slot->content_view, "curveycase.txt"));
    gtk_entry_set_text (entry, "curve2");
    WAIT_FOR (selected (fixture.slot->content_view, "misscurve2.txt"));
    gtk_entry_set_text (entry, "mcv");
    GList *selection = nemo_view_get_selection (fixture.slot->content_view);
    g_assert_null (selection);
    activations = 0;
    g_assert_true (key_press (fixture.window, "Return"));
    WAIT_FOR (!nemo_view_get_type_jump_active (fixture.slot->content_view));
    g_assert_cmpuint (activations, ==, 0);
    g_assert_null (filename_attributes (tree, "misscurve.txt"));
    list_fixture_clear (&fixture);
}

static void
test_custom_keys_and_exit (void)
{
    GtkTreeView *tree = NULL;
    Fixture fixture = list_fixture (&tree);
    GtkEntry *entry = begin_jump (&fixture);
    g_settings_set_string (nemo_keybinding_settings, "type-jump-next", "F8");
    g_assert_true (key_press (fixture.window, "F8"));
    WAIT_FOR (selected (fixture.slot->content_view, "misscurve.txt"));
    const guint unused[] = { GDK_KEY_Right, GDK_KEY_Left, GDK_KEY_g, GDK_KEY_G, GDK_KEY_F9 };
    const GdkModifierType modifiers[] = { 0, 0, GDK_CONTROL_MASK,
                                         GDK_CONTROL_MASK | GDK_SHIFT_MASK, 0 };
    g_settings_set_string (nemo_keybinding_settings, "type-jump-next-alt", "F9");
    for (guint i = 0; i < G_N_ELEMENTS (unused); i++) {
        GdkEvent *event = gdk_event_new (GDK_KEY_PRESS);
        event->key.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (entry)));
        event->key.keyval = unused[i];
        event->key.state = modifiers[i];
        GdkSeat *seat = gdk_display_get_default_seat (gtk_widget_get_display (GTK_WIDGET (entry)));
        gdk_event_set_device (event, gdk_seat_get_keyboard (seat));
        gtk_widget_event (GTK_WIDGET (entry), event);
        gdk_event_free (event);
        g_assert_true (selected (fixture.slot->content_view, "misscurve.txt"));
        g_assert_true (nemo_view_get_type_jump_active (fixture.slot->content_view));
    }
    g_settings_set_string (nemo_keybinding_settings, "type-jump-previous", "F7");
    g_assert_true (key_press (fixture.window, "F7"));
    WAIT_FOR (selected (fixture.slot->content_view, "curveycase.txt"));
    g_settings_set_string (nemo_keybinding_settings, "type-jump-next", "");
    g_assert_true (key_press (fixture.window, "F8"));
    for (guint i = 0; i < 50; i++)
        iterate ();
    g_assert_true (selected (fixture.slot->content_view, "curveycase.txt"));
    g_assert_true (key_press (fixture.window, "Escape"));
    WAIT_FOR (!nemo_view_get_type_jump_active (fixture.slot->content_view));
    g_assert_null (filename_attributes (tree, "misscurve.txt"));
    /* Normal row navigation resumes, independently of the configured match key. */
    g_assert_true (key_press (fixture.window, "Down"));
    WAIT_FOR (selected (fixture.slot->content_view, "misscurve.txt"));
    g_assert_true (key_press (fixture.window, "Down"));
    WAIT_FOR (selected (fixture.slot->content_view, "misscurve2.txt"));
    g_assert_true (key_press (fixture.window, "Down"));
    WAIT_FOR (selected (fixture.slot->content_view, "missile.txt"));
    list_fixture_clear (&fixture);
}

static GtkCellRenderer *
shortcut_cell (GtkTreeView *tree, const char *key, char **path)
{
    GtkTreeModel *model = gtk_tree_view_get_model (tree);
    GtkTreeIter category, action;
    gboolean valid = gtk_tree_model_get_iter_first (model, &category);
    while (valid) {
        gboolean child_valid = gtk_tree_model_iter_children (model, &action, &category);
        while (child_valid) {
            g_autofree char *settings_key = NULL;
            gtk_tree_model_get (model, &action, 4, &settings_key, -1);
            if (g_strcmp0 (settings_key, key) == 0) {
                GtkTreeViewColumn *column = gtk_tree_view_get_column (tree, 1);
                gtk_tree_view_column_cell_set_cell_data (column, model, &action, FALSE, FALSE);
                GList *cells = gtk_cell_layout_get_cells (GTK_CELL_LAYOUT (column));
                GtkCellRenderer *renderer = NULL;
                for (GList *cell = cells; cell != NULL; cell = cell->next) {
                    gboolean visible;
                    g_object_get (cell->data, "visible", &visible, NULL);
                    if (visible) {
                        g_assert_null (renderer);
                        renderer = cell->data;
                    }
                }
                g_assert_nonnull (renderer);
                g_list_free (cells);
                *path = gtk_tree_model_get_string_from_iter (model, &action);
                return renderer;
            }
            child_valid = gtk_tree_model_iter_next (model, &action);
        }
        valid = gtk_tree_model_iter_next (model, &category);
    }
    g_assert_not_reached ();
}

static void
assert_shortcut (GtkTreeView *tree, const char *settings_key, guint key, gboolean contextual)
{
    g_autofree char *path = NULL;
    GtkCellRenderer *renderer = shortcut_cell (tree, settings_key, &path);
    GtkCellRendererAccelMode mode;
    g_autofree char *text = NULL;
    g_autofree char *expected = gtk_accelerator_get_label (key, 0);
    g_object_get (renderer, "accel-mode", &mode, "text", &text, NULL);
    g_assert_cmpint (mode, ==, contextual ? GTK_CELL_RENDERER_ACCEL_MODE_OTHER
                                        : GTK_CELL_RENDERER_ACCEL_MODE_GTK);
    g_assert_cmpstr (text, ==, expected);
}

static void
test_shortcut_editor (void)
{
    GtkTreeView *tree = NULL;
    GtkWidget *editor = nemo_keybindings_create_editor ();
    g_object_ref_sink (editor);
    find_tree (editor, &tree);
    g_assert_nonnull (tree);
    assert_shortcut (tree, "type-jump-next", GDK_KEY_Down, TRUE);
    assert_shortcut (tree, "type-jump-previous", GDK_KEY_Up, TRUE);
    assert_shortcut (tree, "rename", GDK_KEY_F2, FALSE);
    assert_shortcut (tree, "type-jump-next", GDK_KEY_Down, TRUE);

    GList *children = gtk_container_get_children (GTK_CONTAINER (editor));
    GtkEntry *search = GTK_ENTRY (children->data);
    g_list_free (children);
    gtk_entry_set_text (search, "Next Matching Filename");
    g_signal_emit_by_name (search, "search-changed");
    g_autofree char *path = NULL;
    GtkCellRenderer *renderer = shortcut_cell (tree, "type-jump-next", &path);
    g_signal_emit_by_name (renderer, "accel-edited", path, GDK_KEY_Up, 0, 0);
    assert_shortcut (tree, "type-jump-next", GDK_KEY_Up, TRUE);
    g_autofree char *value = g_settings_get_string (nemo_keybinding_settings, "type-jump-next");
    g_assert_cmpstr (value, ==, "Up");
    g_signal_emit_by_name (renderer, "accel-edited", path, GDK_KEY_Down, 0, 0);
    assert_shortcut (tree, "type-jump-next", GDK_KEY_Down, TRUE);
    g_signal_emit_by_name (renderer, "accel-edited", path, GDK_KEY_Tab, 0, 0);
    assert_shortcut (tree, "type-jump-next", GDK_KEY_Down, TRUE);

    gtk_entry_set_text (search, "");
    g_signal_emit_by_name (search, "search-changed");
    GtkCellRenderer *contextual_renderer = renderer;
    g_clear_pointer (&path, g_free);
    renderer = shortcut_cell (tree, "rename", &path);
    GtkCellRendererAccelMode capture_mode;
    g_object_get (contextual_renderer, "accel-mode", &capture_mode, NULL);
    g_assert_cmpint (capture_mode, ==, GTK_CELL_RENDERER_ACCEL_MODE_OTHER);
    g_signal_emit_by_name (renderer, "accel-edited", path, GDK_KEY_Down, 0, 0);
    assert_shortcut (tree, "rename", GDK_KEY_F2, FALSE);
    g_clear_pointer (&value, g_free);
    value = g_settings_get_string (nemo_keybinding_settings, "rename");
    g_assert_cmpstr (value, ==, "F2");

    g_clear_pointer (&path, g_free);
    renderer = shortcut_cell (tree, "type-jump-previous", &path);
    g_signal_emit_by_name (renderer, "accel-cleared", path);
    g_clear_pointer (&value, g_free);
    value = g_settings_get_string (nemo_keybinding_settings, "type-jump-previous");
    g_assert_cmpstr (value, ==, "");
    gtk_widget_destroy (editor);
    g_object_unref (editor);

    /* Reopening preserves custom and disabled primary bindings, and neither
     * resurrects retired alternatives nor changes unrelated shortcuts. */
    g_settings_set_string (nemo_keybinding_settings, "type-jump-next", "F8");
    g_settings_set_string (nemo_keybinding_settings, "type-jump-next-alt", "F9");
    g_settings_set_string (nemo_keybinding_settings, "rename", "F7");
    editor = nemo_keybindings_create_editor ();
    g_object_ref_sink (editor);
    tree = NULL;
    find_tree (editor, &tree);
    assert_shortcut (tree, "type-jump-next", GDK_KEY_F8, TRUE);
    g_clear_pointer (&path, g_free);
    renderer = shortcut_cell (tree, "type-jump-previous", &path);
    guint key;
    g_object_get (renderer, "accel-key", &key, NULL);
    g_assert_cmpuint (key, ==, 0);
    assert_shortcut (tree, "rename", GDK_KEY_F7, FALSE);
    children = gtk_container_get_children (GTK_CONTAINER (editor));
    GtkWidget *button_box = g_list_last (children)->data;
    g_list_free (children);
    children = gtk_container_get_children (GTK_CONTAINER (button_box));
    g_signal_emit_by_name (children->data, "clicked");
    g_list_free (children);
    assert_shortcut (tree, "type-jump-next", GDK_KEY_Down, TRUE);
    assert_shortcut (tree, "type-jump-previous", GDK_KEY_Up, TRUE);
    assert_shortcut (tree, "rename", GDK_KEY_F2, FALSE);
    g_clear_pointer (&value, g_free);
    value = g_settings_get_string (nemo_keybinding_settings, "type-jump-next-alt");
    g_assert_cmpstr (value, ==, "F9");
    gtk_widget_destroy (editor);
    g_object_unref (editor);
}

static void
test_prefix_mode_and_bindings (void)
{
    GtkTreeView *tree = NULL;
    Fixture fixture = list_fixture (&tree);
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE, "prefix");
    begin_jump (&fixture);
    WAIT_FOR (selected (fixture.slot->content_view, "curveycase.txt"));
    g_assert_null (filename_attributes (tree, "misscurve.txt"));
    g_assert_true (key_press (fixture.window, "Right"));
    for (guint i = 0; i < 50; i++)
        iterate ();
    g_assert_true (selected (fixture.slot->content_view, "curveycase.txt"));
    g_assert_true (nemo_view_get_type_jump_active (fixture.slot->content_view));
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE, "substring");
    WAIT_FOR (!nemo_view_get_type_jump_active (fixture.slot->content_view));
    g_assert_null (filename_attributes (tree, "curveycase.txt"));

    const char *keys[] = {
        "type-jump-next", "type-jump-previous"
    };
    guint count = 0;
    for (gint j = 0; j < nemo_keybinding_entries_count; j++)
        if (g_str_equal (nemo_keybinding_entries[j].category, "Type to Jump"))
            count++;
    g_assert_cmpuint (count, ==, G_N_ELEMENTS (keys));
    for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
        const NemoKeybindingEntry *entry = NULL;
        for (gint j = 0; j < nemo_keybinding_entries_count; j++)
            if (g_str_equal (nemo_keybinding_entries[j].settings_key, keys[i]))
                entry = &nemo_keybinding_entries[j];
        g_assert_nonnull (entry);
        g_assert_cmpstr (entry->category, ==, "Type to Jump");
        GVariant *value = g_settings_get_default_value (nemo_keybinding_settings, keys[i]);
        g_assert_cmpstr (g_variant_get_string (value, NULL), ==, entry->default_accel);
        g_variant_unref (value);
    }
    list_fixture_clear (&fixture);
}

static void
test_timeout_and_destroy (void)
{
    GtkTreeView *tree = NULL;
    Fixture fixture = list_fixture (&tree);
    begin_jump (&fixture);
    WAIT_FOR (!nemo_view_get_type_jump_active (fixture.slot->content_view));
    g_assert_null (filename_attributes (tree, "misscurve.txt"));
    begin_jump (&fixture);
    list_fixture_clear (&fixture);
    for (guint i = 0; i < 100; i++)
        iterate ();
}

typedef struct { const char *path; GTestFunc run; } ListCase;

static void
run_case (gconstpointer data)
{
    const ListCase *test = data;
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 20000000, 0);
        g_test_trap_assert_passed ();
        return;
    }
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    application = nemo_main_application_get_singleton ();
    GError *error = NULL;
    g_assert_true (g_application_register (G_APPLICATION (application), NULL, &error));
    g_assert_no_error (error);
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE, "substring");
    test->run ();
    g_object_unref (application);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    gtk_test_init (&argc, &argv, NULL);
    static const ListCase cases[] = {
        { "/list-type-jump/ranking-navigation-highlight", test_rank_and_navigation },
        { "/list-type-jump/custom-keys-exit", test_custom_keys_and_exit },
        { "/list-type-jump/shortcut-editor", test_shortcut_editor },
        { "/list-type-jump/prefix-mode-settings", test_prefix_mode_and_bindings },
        { "/list-type-jump/timeout-destroy", test_timeout_and_destroy },
        { "/list-type-jump/file-colors", test_file_type_colors },
        { "/list-type-jump/grey-details", test_grey_details },
    };
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
        g_test_add_data_func (cases[i].path, &cases[i], run_case);
    return g_test_run ();
}
