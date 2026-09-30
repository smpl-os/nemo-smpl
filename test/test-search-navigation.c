/* Real windows, views, search workers and shortcuts on the isolated desktop. */
#include <config.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include "../src/nemo-main-application.h"
#include "../src/nemo-window-private.h"
#include "../src/nemo-actions.h"
#include "../src/nemo-query-editor.h"
#include "../src/nemo-keybindings.h"
#include "../src/nemo-list-view.h"
#include "../src/nemo-icon-view.h"
#include "../src/nemo-paged-viewer.h"
#include <libnemo-private/nemo-global-preferences.h>
#include <libnemo-private/nemo-query.h>
#include <libnemo-private/nemo-search-directory.h>

#include "window-test-fixture.h"

static void
start_search (Fixture *fixture, gboolean wait)
{
    g_settings_set_string (nemo_keybinding_settings, "search", "<Alt>F7");
    nemo_window_set_active_slot (fixture->window, fixture->slot);
    nemo_view_grab_focus (fixture->slot->content_view);
    nemo_keybindings_apply_all ();
    g_assert_true (key_press (fixture->window, "<Alt>F7"));
    WAIT_FOR (nemo_query_editor_get_active (fixture->slot->query_editor));
    NemoQuery *query = nemo_query_new ();
    g_autofree char *uri = g_file_get_uri (fixture->origin);
    nemo_query_set_location (query, uri);
    nemo_query_set_file_pattern (query, "*needle*");
    nemo_query_set_recurse (query, TRUE);
    nemo_query_editor_set_query (fixture->slot->query_editor, query);
    g_signal_emit_by_name (fixture->slot->query_editor, "changed", query, TRUE);
    g_object_unref (query);
    if (!wait)
        return;
    WAIT_FOR (fixture->slot->location != NULL &&
              g_file_has_uri_scheme (fixture->slot->location, "x-nemo-search") &&
              fixture->slot->content_view != NULL &&
              !nemo_view_get_loading (fixture->slot->content_view));
    NemoFile *file = nemo_file_get (fixture->item);
    GList selection = { .data = file };
    nemo_view_set_selection (fixture->slot->content_view, &selection);
    nemo_file_unref (file);
    nemo_view_grab_focus (fixture->slot->content_view);
}

static void
test_escape_and_backspace (void)
{
    Fixture fixture = fixture_new ();
    const char *keys[] = { "Escape", "BackSpace" };
    for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
        start_search (&fixture, TRUE);
        if (i == 1) {
            nemo_window_slot_set_content_view (fixture.slot, NEMO_LIST_VIEW_ID);
            WAIT_FOR (NEMO_IS_LIST_VIEW (fixture.slot->content_view) &&
                      !nemo_view_get_loading (fixture.slot->content_view));
            nemo_view_grab_focus (fixture.slot->content_view);
        }
        g_assert_true (key_press (fixture.window, keys[i]));
        WAIT_FOR (slot_at (fixture.slot, fixture.origin));
        g_assert_false (nemo_query_editor_get_active (fixture.slot->query_editor));
        g_assert_false (gtk_widget_get_visible (fixture.slot->no_search_results_box));
    }
    start_search (&fixture, FALSE);
    g_assert_true (nemo_window_slot_cancel_search (fixture.slot));
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    fixture_clear (&fixture);
}

static void
test_search_entry_editing (void)
{
    Fixture fixture = fixture_new ();
    start_search (&fixture, TRUE);
    gtk_widget_grab_focus (GTK_WIDGET (fixture.slot->query_editor));
    GtkWidget *entry = gtk_window_get_focus (GTK_WINDOW (fixture.window));
    g_assert_true (GTK_IS_ENTRY (entry));
    gtk_entry_set_text (GTK_ENTRY (entry), "abcd");
    gtk_editable_set_position (GTK_EDITABLE (entry), 4);
    g_assert_true (key_press (fixture.window, "BackSpace"));
    WAIT_FOR (g_strcmp0 (gtk_entry_get_text (GTK_ENTRY (entry)), "abc") == 0);
    g_assert_true (g_file_has_uri_scheme (fixture.slot->location, "x-nemo-search"));
    g_assert_true (nemo_query_editor_get_active (fixture.slot->query_editor));
    g_assert_true (key_press (fixture.window, "Escape"));
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    fixture_clear (&fixture);
}

static GtkAction *
find_action (NemoWindow *window, const char *name)
{
    for (GList *l = gtk_ui_manager_get_action_groups (nemo_window_get_ui_manager (window));
         l != NULL; l = l->next) {
        GtkAction *action = gtk_action_group_get_action (l->data, name);
        if (action != NULL)
            return action;
    }
    return NULL;
}

static void
assert_selected_item (NemoWindowSlot *slot, GFile *item)
{
    GList *selection = nemo_view_get_selection (slot->content_view);
    g_assert_cmpuint (g_list_length (selection), ==, 1);
    GFile *selected = nemo_file_get_activation_location (selection->data);
    g_assert_true (g_file_equal (selected, item));
    g_object_unref (selected);
    nemo_file_list_free (selection);
}

static void
test_containing_folder_shortcuts (void)
{
    Fixture fixture = fixture_new ();
    start_search (&fixture, TRUE);
    GtkAction *other_action = find_action (fixture.window, NEMO_ACTION_OPEN_CONTAINING_FOLDER_OTHER_PANE);
    g_assert_nonnull (other_action);
    WAIT_FOR (gtk_action_get_visible (other_action) && gtk_action_get_sensitive (other_action));
    gtk_ui_manager_ensure_update (nemo_window_get_ui_manager (fixture.window));
    g_assert_cmpuint (g_slist_length (gtk_action_get_proxies (other_action)), >=, 2);
    GFile *search_location = g_object_ref (fixture.slot->location);
    g_assert_true (key_press (fixture.window, "<Control><Alt><Shift>o"));
    WAIT_FOR (nemo_window_split_view_showing (fixture.window));
    NemoWindowSlot *other = nemo_window_get_extra_slot (fixture.window);
    g_assert_nonnull (other);
    WAIT_FOR (slot_at (other, fixture.containing));
    g_assert_true (g_file_equal (fixture.slot->location, search_location));
    g_assert_true (nemo_query_editor_get_active (fixture.slot->query_editor));
    g_assert_true (nemo_window_get_active_slot (fixture.window) == fixture.slot);
    assert_selected_item (fixture.slot, fixture.item);
    assert_selected_item (other, fixture.item);

    g_settings_set_string (nemo_keybinding_settings, "open-containing-folder", "<Control><Alt>p");
    nemo_keybindings_apply_all ();
    nemo_view_grab_focus (fixture.slot->content_view);
    g_assert_true (key_press (fixture.window, "<Control><Alt>p"));
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    assert_selected_item (fixture.slot, fixture.item);
    g_assert_false (nemo_query_editor_get_active (fixture.slot->query_editor));
    g_settings_reset (nemo_keybinding_settings, "open-containing-folder");
    GtkAccelKey binding;
    g_assert_true (gtk_accel_map_lookup_entry ("<Actions>/DirViewActions/OpenContainingFolder", &binding));
    g_assert_cmpuint (binding.accel_key, ==, GDK_KEY_o);
    g_assert_cmpuint (binding.accel_mods, ==, GDK_CONTROL_MASK | GDK_MOD1_MASK);
    g_object_unref (search_location);
    fixture_clear (&fixture);
}

static void
test_other_pane_from_right (void)
{
    Fixture fixture = fixture_new ();
    NemoWindowSlot *left = fixture.slot;
    start_search (&fixture, TRUE);
    nemo_window_split_view_on (fixture.window);
    fixture.slot = nemo_window_get_extra_slot (fixture.window);
    nemo_window_slot_open_location (fixture.slot, fixture.origin, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    start_search (&fixture, TRUE);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_ALWAYS_USE_BROWSER, FALSE);
    GtkAction *action = find_action (fixture.window, NEMO_ACTION_OPEN_CONTAINING_FOLDER_OTHER_PANE);
    WAIT_FOR (action != NULL && gtk_action_get_visible (action) && gtk_action_get_sensitive (action));
    gtk_action_activate (action);
    WAIT_FOR (slot_at (left, fixture.containing));
    g_assert_true (g_file_has_uri_scheme (fixture.slot->location, "x-nemo-search"));
    g_assert_true (nemo_window_get_active_slot (fixture.window) == fixture.slot);
    g_assert_cmpuint (g_list_length (gtk_application_get_windows (GTK_APPLICATION (application))), ==, 1);
    assert_selected_item (left, fixture.item);
    g_assert_true (key_press (fixture.window, "Escape"));
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    fixture_clear (&fixture);
}

static void
test_registered_shortcuts (void)
{
    const char *keys[] = { "open-containing-folder", "open-containing-folder-other-pane" };
    for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
        const NemoKeybindingEntry *entry = NULL;
        for (gint j = 0; j < nemo_keybinding_entries_count; j++)
            if (g_str_equal (nemo_keybinding_entries[j].settings_key, keys[i]))
                entry = &nemo_keybinding_entries[j];
        g_assert_nonnull (entry);
        GVariant *value = g_settings_get_default_value (nemo_keybinding_settings, keys[i]);
        g_assert_cmpstr (g_variant_get_string (value, NULL), ==, entry->default_accel);
        g_variant_unref (value);
    }
}

static GtkWidget *
find_descendant (GtkWidget *widget, GType type)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE (widget, type))
        return widget;
    if (!GTK_IS_CONTAINER (widget))
        return NULL;
    GList *children = gtk_container_get_children (GTK_CONTAINER (widget));
    GtkWidget *found = NULL;
    for (GList *l = children; l != NULL && found == NULL; l = l->next)
        found = find_descendant (l->data, type);
    g_list_free (children);
    return found;
}

static void
test_preview_navigation (void)
{
    Fixture fixture = fixture_new ();
    GFile *next = g_file_get_child (fixture.containing, "z-next.txt");
    g_autofree char *path = g_file_get_path (fixture.item);
    g_autofree char *next_path = g_file_get_path (next);
    GString *text = g_string_new (NULL);
    for (guint i = 0; i < 300; i++)
        g_string_append_printf (text, "Line %u of the preview navigation fixture\n", i);
    g_assert_true (g_file_set_contents (path, text->str, text->len, NULL));
    g_assert_true (g_file_set_contents (next_path, "next file\n", -1, NULL));
    g_string_free (text, TRUE);
    nemo_window_slot_open_location (fixture.slot, fixture.containing,
                                   NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    nemo_window_slot_set_content_view (fixture.slot, NEMO_LIST_VIEW_ID);
    WAIT_FOR (NEMO_IS_LIST_VIEW (fixture.slot->content_view) &&
              !nemo_view_get_loading (fixture.slot->content_view));
    NemoFile *file = nemo_file_get (fixture.item);
    GList selection = { .data = file };
    nemo_view_set_selection (fixture.slot->content_view, &selection);
    nemo_view_grab_focus (fixture.slot->content_view);
    GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (fixture.window));
    nemo_window_preview_pane_on (fixture.window);
    GtkWidget *viewer = find_descendant (fixture.window->details->preview_pane,
                                         NEMO_TYPE_PAGED_VIEWER);
    g_assert_nonnull (viewer);
    GtkWidget *scrollbar = find_descendant (viewer, GTK_TYPE_SCROLLBAR);
    g_assert_nonnull (scrollbar);
    GtkAdjustment *adjustment = gtk_range_get_adjustment (GTK_RANGE (scrollbar));
    WAIT_FOR (gtk_adjustment_get_upper (adjustment) > gtk_adjustment_get_page_size (adjustment));
    g_assert_true (gtk_window_get_focus (GTK_WINDOW (fixture.window)) == focus);

    GdkEventKey page = {
        .type = GDK_KEY_PRESS,
        .window = gtk_widget_get_window (GTK_WIDGET (fixture.window)),
        .keyval = GDK_KEY_Page_Down,
        .state = GDK_MOD1_MASK,
    };
    g_assert_true (GTK_WIDGET_GET_CLASS (fixture.window)->key_press_event (
        GTK_WIDGET (fixture.window), &page));
    WAIT_FOR (gtk_adjustment_get_value (adjustment) > 0);
    g_assert_true (gtk_window_get_focus (GTK_WINDOW (fixture.window)) == focus);
    assert_selected_item (fixture.slot, fixture.item);
    page.keyval = GDK_KEY_Page_Up;
    g_assert_true (GTK_WIDGET_GET_CLASS (fixture.window)->key_press_event (
        GTK_WIDGET (fixture.window), &page));
    WAIT_FOR (gtk_adjustment_get_value (adjustment) == 0);

    g_assert_true (key_press (fixture.window, "Down"));
    WAIT_FOR (gtk_adjustment_get_upper (adjustment) <= gtk_adjustment_get_page_size (adjustment));
    assert_selected_item (fixture.slot, next);
    g_assert_true (gtk_window_get_focus (GTK_WINDOW (fixture.window)) == focus);
    g_assert_true (key_press (fixture.window, "Up"));
    WAIT_FOR (gtk_adjustment_get_upper (adjustment) > gtk_adjustment_get_page_size (adjustment));
    assert_selected_item (fixture.slot, fixture.item);
    g_assert_true (gtk_window_get_focus (GTK_WINDOW (fixture.window)) == focus);

    g_settings_set_string (nemo_keybinding_settings, "preview-scroll-down", "<Alt>j");
    page.keyval = GDK_KEY_j;
    g_assert_true (GTK_WIDGET_GET_CLASS (fixture.window)->key_press_event (
        GTK_WIDGET (fixture.window), &page));
    WAIT_FOR (gtk_adjustment_get_value (adjustment) > 0);
    assert_selected_item (fixture.slot, fixture.item);
    g_assert_true (gtk_window_get_focus (GTK_WINDOW (fixture.window)) == focus);
    g_settings_reset (nemo_keybinding_settings, "preview-scroll-down");

    nemo_window_preview_pane_off (fixture.window);
    nemo_file_unref (file);
    g_assert_true (g_file_delete (next, NULL, NULL));
    g_object_unref (next);
    fixture_clear (&fixture);
}

typedef struct {
    const char *path;
    GTestFunc run;
} NavigationCase;

static void
run_case (gconstpointer data)
{
    const NavigationCase *test = data;
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
    g_settings_set_boolean (nemo_search_preferences, NEMO_PREFERENCES_SEARCH_FILES_RECURSIVELY, TRUE);
    g_settings_set_boolean (nemo_search_preferences, NEMO_PREFERENCES_SEARCH_FILES_REGEX, FALSE);
    test->run ();
    g_object_unref (application);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    gtk_test_init (&argc, &argv, NULL);
    static const NavigationCase cases[] = {
        { "/search-navigation/escape-backspace", test_escape_and_backspace },
        { "/search-navigation/entry-editing", test_search_entry_editing },
        { "/search-navigation/containing-shortcuts", test_containing_folder_shortcuts },
        { "/search-navigation/other-pane-from-right", test_other_pane_from_right },
        { "/search-navigation/shortcut-settings", test_registered_shortcuts },
        { "/search-navigation/preview-navigation", test_preview_navigation },
    };
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
        g_test_add_data_func (cases[i].path, &cases[i], run_case);
    return g_test_run ();
}
