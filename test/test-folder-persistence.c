#include <config.h>
#include "window-test-fixture.h"
#include "../src/nemo-actions.h"
#include "../src/nemo-pathbar.h"
#include "../src/nemo-query-editor.h"
#include "../src/nemo-window-bookmarks.h"
#include <libnemo-private/nemo-query.h>
#include <libxapp/xapp-favorites.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>

static GtkMenu *shown_menu;
static guint popup_count;

void
__wrap_eel_pop_up_context_menu (GtkMenu *menu, GdkEvent *event, GtkWidget *widget)
{
    shown_menu = menu;
    popup_count++;
}

static NemoWindowSlot *
pane_tab (NemoWindowPane *pane, gint index)
{
    return NEMO_WINDOW_SLOT (gtk_notebook_get_nth_page (GTK_NOTEBOOK (pane->notebook), index));
}

static void
add_tab (NemoWindowPane *pane, GFile *location)
{
    NemoWindowSlot *slot = nemo_window_pane_open_slot (pane, NEMO_WINDOW_OPEN_SLOT_APPEND);
    nemo_window_slot_open_location (slot, location, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (slot, location));
}

static void
reopen_fixture (Fixture *fixture, gboolean explicit_location)
{
    g_application_open (G_APPLICATION (application), &fixture->origin, 1,
                        explicit_location ? "" : "DEFAULT=0");
    GList *windows = gtk_application_get_windows (GTK_APPLICATION (application));
    g_assert_cmpuint (g_list_length (windows), ==, 1);
    NemoWindow *window = NEMO_WINDOW (windows->data);
    g_object_unref (fixture->window);
    fixture->window = g_object_ref (window);
    fixture->slot = nemo_window_get_active_slot (window);
}

static void
assert_saved_folder (const char *key, GFile *folder)
{
    g_auto(GStrv) uris = g_settings_get_strv (nemo_window_state, key);
    g_autofree char *expected = g_file_get_uri (folder);
    g_assert_cmpuint (g_strv_length (uris), ==, 1);
    g_assert_cmpstr (uris[0], ==, expected);
}

static void
test_restore_panes_and_tabs (void)
{
    g_assert_true (g_settings_get_boolean (nemo_preferences, NEMO_PREFERENCES_RESTORE_TABS_ON_STARTUP));
    Fixture fixture = fixture_new ();
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_RESTORE_TABS_ON_STARTUP, TRUE);
    NemoWindowPane *left = fixture.slot->pane;
    add_tab (left, fixture.containing);
    nemo_window_split_view_on (fixture.window);
    NemoWindowPane *right = nemo_window_get_extra_slot (fixture.window)->pane;
    nemo_window_slot_open_location (right->active_slot, fixture.containing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (right->active_slot, fixture.containing));
    add_tab (right, fixture.origin);
    gtk_notebook_set_current_page (GTK_NOTEBOOK (left->notebook), 1);
    gtk_notebook_set_current_page (GTK_NOTEBOOK (right->notebook), 0);
    nemo_window_set_active_pane (fixture.window, right);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_ALWAYS_USE_BROWSER, FALSE);
    nemo_window_close (fixture.window);
    g_assert_cmpint (g_settings_get_int (nemo_window_state, NEMO_WINDOW_STATE_SAVED_ACTIVE_PANE), ==, 1);
    reopen_fixture (&fixture, FALSE);
    WAIT_FOR (nemo_window_split_view_showing (fixture.window));
    GtkPaned *paned = GTK_PANED (fixture.window->details->split_view_hpane);
    left = NEMO_WINDOW_PANE (gtk_paned_get_child1 (paned));
    right = NEMO_WINDOW_PANE (gtk_paned_get_child2 (paned));
    g_assert_cmpint (gtk_notebook_get_n_pages (GTK_NOTEBOOK (left->notebook)), ==, 2);
    g_assert_cmpint (gtk_notebook_get_n_pages (GTK_NOTEBOOK (right->notebook)), ==, 2);
    WAIT_FOR (slot_at (pane_tab (left, 0), fixture.origin));
    WAIT_FOR (slot_at (pane_tab (left, 1), fixture.containing));
    WAIT_FOR (slot_at (pane_tab (right, 0), fixture.containing));
    WAIT_FOR (slot_at (pane_tab (right, 1), fixture.origin));
    g_assert_true (nemo_window_get_active_pane (fixture.window) == right);
    g_assert_cmpint (gtk_notebook_get_current_page (GTK_NOTEBOOK (left->notebook)), ==, 1);
    g_assert_cmpint (gtk_notebook_get_current_page (GTK_NOTEBOOK (right->notebook)), ==, 0);
    g_assert_cmpuint (g_list_length (gtk_application_get_windows (GTK_APPLICATION (application))), ==, 1);
    fixture_clear (&fixture);
}

static void
test_last_tab_close (void)
{
    Fixture fixture = fixture_new ();
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_RESTORE_TABS_ON_STARTUP, TRUE);
    nemo_window_pane_close_slot (fixture.slot->pane, fixture.slot);
    assert_saved_folder (NEMO_WINDOW_STATE_SAVED_TABS_LEFT, fixture.origin);
    reopen_fixture (&fixture, FALSE);
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    fixture_clear (&fixture);
}

static void
test_disabled_restore (void)
{
    Fixture fixture = fixture_new ();
    g_autofree char *old_uri = g_file_get_uri (fixture.containing);
    const char *old_uris[] = { old_uri, NULL };
    g_settings_set_strv (nemo_window_state, NEMO_WINDOW_STATE_SAVED_TABS_LEFT, old_uris);
    nemo_window_close (fixture.window);
    assert_saved_folder (NEMO_WINDOW_STATE_SAVED_TABS_LEFT, fixture.containing);
    reopen_fixture (&fixture, FALSE);
    GFile *home = g_file_new_for_path (g_get_home_dir ());
    WAIT_FOR (slot_at (fixture.slot, home));
    g_object_unref (home);
    fixture_clear (&fixture);
}

static void
test_explicit_location (void)
{
    Fixture fixture = fixture_new ();
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_RESTORE_TABS_ON_STARTUP, TRUE);
    nemo_window_slot_open_location (fixture.slot, fixture.containing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    nemo_window_close (fixture.window);
    reopen_fixture (&fixture, TRUE);
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    fixture_clear (&fixture);
}

static void
test_search_origin_saved (void)
{
    Fixture fixture = fixture_new ();
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_RESTORE_TABS_ON_STARTUP, TRUE);
    g_assert_true (key_press (fixture.window, "<Control>f"));
    NemoQuery *query = nemo_query_new ();
    g_autofree char *uri = g_file_get_uri (fixture.origin);
    nemo_query_set_location (query, uri);
    nemo_query_set_file_pattern (query, "*needle*");
    nemo_query_set_recurse (query, TRUE);
    nemo_query_editor_set_query (fixture.slot->query_editor, query);
    g_signal_emit_by_name (fixture.slot->query_editor, "changed", query, TRUE);
    g_object_unref (query);
    WAIT_FOR (fixture.slot->location != NULL &&
              g_file_has_uri_scheme (fixture.slot->location, "x-nemo-search"));
    nemo_window_close (fixture.window);
    assert_saved_folder (NEMO_WINDOW_STATE_SAVED_TABS_LEFT, fixture.origin);
    reopen_fixture (&fixture, FALSE);
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    fixture_clear (&fixture);
}

static void
show_breadcrumb_menu (NemoWindowPane *pane, GFile *location)
{
    GList *children = gtk_container_get_children (GTK_CONTAINER (pane->path_bar));
    GtkWidget *button = NULL;
    for (GList *l = children; l != NULL; l = l->next) {
        if (!GTK_IS_TOGGLE_BUTTON (l->data))
            continue;
        GFile *path = nemo_path_bar_get_path_for_button (NEMO_PATH_BAR (pane->path_bar), l->data);
        if (path != NULL && g_file_equal (path, location))
            button = l->data;
        g_clear_object (&path);
    }
    g_list_free (children);
    g_assert_nonnull (button);
    guint before = popup_count;
    GdkEvent *event = gdk_event_new (GDK_BUTTON_PRESS);
    event->button.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (pane->window)));
    event->button.button = GDK_BUTTON_SECONDARY;
    gboolean handled = FALSE;
    g_signal_emit_by_name (button, "button-press-event", &event->button, &handled);
    gdk_event_free (event);
    g_assert_true (handled);
    WAIT_FOR (popup_count > before);
}

static GtkWidget *
popup_item (const char *name)
{
    GtkWidget *item = NULL;
    GList *children = gtk_container_get_children (GTK_CONTAINER (shown_menu));
    for (GList *l = children; l != NULL; l = l->next) {
        if (!GTK_IS_ACTIVATABLE (l->data))
            continue;
        GtkAction *action = gtk_activatable_get_related_action (GTK_ACTIVATABLE (l->data));
        if (action != NULL && g_str_equal (gtk_action_get_name (action), name))
            item = l->data;
    }
    g_list_free (children);
    g_assert_nonnull (item);
    g_assert_true (gtk_widget_get_visible (item));
    return item;
}

static gboolean
is_favorite (GFile *file)
{
    g_autofree char *uri = g_file_get_uri (file);
    return xapp_favorites_find_by_uri (xapp_favorites_get_default (), uri) != NULL;
}

static void
test_breadcrumb_targets (void)
{
    Fixture fixture = fixture_new ();
    NemoWindowSlot *left = fixture.slot;
    nemo_window_split_view_on (fixture.window);
    NemoWindowSlot *right = nemo_window_get_extra_slot (fixture.window);
    nemo_window_slot_open_location (right, fixture.containing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (right, fixture.containing));
    nemo_window_set_active_slot (fixture.window, left);
    show_breadcrumb_menu (right->pane, fixture.origin);
    g_assert_true (nemo_window_get_active_slot (fixture.window) == right);
    g_assert_true (slot_at (left, fixture.origin));
    g_assert_true (slot_at (right, fixture.containing));
    gtk_menu_item_activate (GTK_MENU_ITEM (popup_item (NEMO_ACTION_LOCATION_BOOKMARK)));
    g_assert_true (nemo_window_location_is_bookmarked (fixture.window, fixture.origin));
    g_assert_false (nemo_window_location_is_bookmarked (fixture.window, fixture.containing));
    gtk_menu_item_activate (GTK_MENU_ITEM (popup_item (NEMO_ACTION_LOCATION_FAVORITE)));
    WAIT_FOR (is_favorite (fixture.origin));
    g_assert_false (is_favorite (fixture.containing));
    show_breadcrumb_menu (right->pane, fixture.origin);
    g_assert_false (gtk_widget_is_sensitive (popup_item (NEMO_ACTION_LOCATION_BOOKMARK)));
    g_assert_false (gtk_widget_is_sensitive (popup_item (NEMO_ACTION_LOCATION_FAVORITE)));
    fixture_clear (&fixture);
}

static void
wait_for_favorite_action (NemoWindow *window)
{
    gint64 deadline = g_get_monotonic_time () + 10000000;
    while (g_get_monotonic_time () < deadline) {
        GList *groups = gtk_ui_manager_get_action_groups (nemo_window_get_ui_manager (window));
        for (GList *l = groups; l != NULL; l = l->next) {
            GtkAction *action = gtk_action_group_get_action (l->data, NEMO_ACTION_ADD_FAVORITE);
            if (action != NULL && gtk_action_is_sensitive (action) && gtk_action_get_visible (action))
                return;
        }
        iterate ();
    }
    g_error ("Current-folder favorite action did not become available after navigation");
}

static void
test_current_folder_shortcuts (void)
{
    Fixture fixture = fixture_new ();
    NemoFile *file = nemo_file_get (fixture.containing);
    GList selection = { .data = file };
    nemo_view_set_selection (fixture.slot->content_view, &selection);
    nemo_file_unref (file);
    g_assert_true (key_press (fixture.window, "<Control>d"));
    g_assert_true (nemo_window_location_is_bookmarked (fixture.window, fixture.origin));
    NemoBookmarkList *list = fixture.window->details->bookmark_list;
    guint count = nemo_bookmark_list_length (list);
    NemoBookmark *bookmark = nemo_bookmark_list_item_at (list, count - 1);
    nemo_bookmark_set_custom_name (bookmark, "My folder");
    g_assert_true (key_press (fixture.window, "<Control>d"));
    g_assert_cmpuint (nemo_bookmark_list_length (list), ==, count);
    wait_for_favorite_action (fixture.window);
    g_assert_true (key_press (fixture.window, "<Control><Alt>b"));
    WAIT_FOR (is_favorite (fixture.origin));
    g_assert_false (is_favorite (fixture.containing));
    g_settings_set_string (nemo_keybinding_settings, "add-favorite", "<Control><Alt>j");
    g_settings_set_string (nemo_keybinding_settings, "add-bookmark", "<Control><Alt>k");
    nemo_window_slot_open_location (fixture.slot, fixture.containing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    g_assert_false (key_press (fixture.window, "<Control>d"));
    g_assert_false (key_press (fixture.window, "<Control><Alt>b"));
    g_assert_true (key_press (fixture.window, "<Control><Alt>k"));
    g_assert_true (nemo_window_location_is_bookmarked (fixture.window, fixture.containing));
    wait_for_favorite_action (fixture.window);
    g_assert_true (key_press (fixture.window, "<Control><Alt>j"));
    WAIT_FOR (is_favorite (fixture.containing));
    g_settings_set_string (nemo_keybinding_settings, "add-favorite", "");
    g_assert_false (key_press (fixture.window, "<Control><Alt>j"));
    fixture_clear (&fixture);
}

typedef struct { const char *path; GTestFunc run; } FolderCase;

static void
run_case (gconstpointer data)
{
    const FolderCase *test = data;
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 30000000, 0);
        g_test_trap_assert_passed ();
        return;
    }
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    application = nemo_main_application_get_singleton ();
    GError *error = NULL;
    g_assert_true (g_application_register (G_APPLICATION (application), NULL, &error));
    g_assert_no_error (error);
    test->run ();
    g_object_unref (application);
}

static int
wait_for_window_title (const char *expected)
{
    gtk_init (NULL, NULL);
    GdkDisplay *display = gdk_display_get_default ();
    Display *xdisplay = gdk_x11_display_get_xdisplay (display);
    gint64 deadline = g_get_monotonic_time () + 12000000;
    GString *observed = g_string_new (NULL);
    while (g_get_monotonic_time () < deadline) {
        g_string_set_size (observed, 0);
        Window root, parent, *windows = NULL;
        unsigned int count = 0;
        gboolean found = FALSE;
        g_assert_true (XQueryTree (xdisplay, DefaultRootWindow (xdisplay),
                                  &root, &parent, &windows, &count));
        /* Other processes may destroy a window between QueryTree and FetchName. */
        gdk_x11_display_error_trap_push (display);
        for (guint i = 0; i < count; i++) {
            char *title = NULL;
            if (XFetchName (xdisplay, windows[i], &title) && title != NULL) {
                found = g_str_equal (title, expected);
                g_string_append_printf (observed, "'%s' ", title);
            }
            if (title != NULL)
                XFree (title);
            if (found)
                break;
        }
        gdk_x11_display_error_trap_pop_ignored (display);
        XFree (windows);
        if (found) {
            g_string_free (observed, TRUE);
            return 0;
        }
        g_usleep (20000);
    }
    g_printerr ("Expected window title '%s'; observed: %s\n", expected, observed->str);
    g_string_free (observed, TRUE);
    return 1;
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    if (argc == 3 && g_str_equal (argv[1], "--wait-for-title"))
        return wait_for_window_title (argv[2]);
    gtk_test_init (&argc, &argv, NULL);
    static const FolderCase cases[] = {
        { "/folders/restore-panes-and-tabs", test_restore_panes_and_tabs },
        { "/folders/last-tab-close", test_last_tab_close },
        { "/folders/disabled-restore", test_disabled_restore },
        { "/folders/explicit-location", test_explicit_location },
        { "/folders/search-origin", test_search_origin_saved },
        { "/folders/breadcrumb-targets", test_breadcrumb_targets },
        { "/folders/current-folder-shortcuts", test_current_folder_shortcuts },
    };
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
        g_test_add_data_func (cases[i].path, &cases[i], run_case);
    return g_test_run ();
}
