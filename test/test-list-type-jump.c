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
    g_assert_true (key_press (fixture.window, "Right"));
    WAIT_FOR (selected (fixture.slot->content_view, "misscurve2.txt"));
    g_assert_true (key_press (fixture.window, "Up"));
    WAIT_FOR (selected (fixture.slot->content_view, "misscurve.txt"));
    g_assert_true (key_press (fixture.window, "Left"));
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
    begin_jump (&fixture);
    g_settings_set_string (nemo_keybinding_settings, "type-jump-next", "F8");
    g_assert_true (key_press (fixture.window, "F8"));
    WAIT_FOR (selected (fixture.slot->content_view, "misscurve.txt"));
    g_assert_true (key_press (fixture.window, "Escape"));
    WAIT_FOR (!nemo_view_get_type_jump_active (fixture.slot->content_view));
    g_assert_null (filename_attributes (tree, "misscurve.txt"));
    /* Normal row navigation resumes, independently of the configured match key. */
    g_assert_true (key_press (fixture.window, "Down"));
    WAIT_FOR (selected (fixture.slot->content_view, "misscurve2.txt"));
    g_assert_true (key_press (fixture.window, "Down"));
    WAIT_FOR (selected (fixture.slot->content_view, "missile.txt"));
    list_fixture_clear (&fixture);
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
    g_settings_set_string (nemo_keybinding_settings, "type-jump-next-alt", "");
    g_assert_true (key_press (fixture.window, "Right"));
    for (guint i = 0; i < 50; i++)
        iterate ();
    g_assert_true (selected (fixture.slot->content_view, "curveycase.txt"));
    g_assert_true (nemo_view_get_type_jump_active (fixture.slot->content_view));
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE, "substring");
    WAIT_FOR (!nemo_view_get_type_jump_active (fixture.slot->content_view));
    g_assert_null (filename_attributes (tree, "curveycase.txt"));

    const char *keys[] = {
        "type-jump-next", "type-jump-previous", "type-jump-next-alt",
        "type-jump-previous-alt", "type-jump-next-secondary", "type-jump-previous-secondary"
    };
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
        { "/list-type-jump/prefix-mode-settings", test_prefix_mode_and_bindings },
        { "/list-type-jump/timeout-destroy", test_timeout_and_destroy },
        { "/list-type-jump/file-colors", test_file_type_colors },
        { "/list-type-jump/grey-details", test_grey_details },
    };
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
        g_test_add_data_func (cases[i].path, &cases[i], run_case);
    return g_test_run ();
}
