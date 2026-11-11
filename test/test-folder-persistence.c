#include <config.h>
#include "window-test-fixture.h"
#include "../src/nemo-actions.h"
#include "../src/nemo-pathbar.h"
#include "../src/nemo-query-editor.h"
#include "../src/nemo-window-bookmarks.h"
#include "../src/nemo-overview.h"
#include "../src/nemo-disk-treemap.h"
#include "../src/nemo-quick-preview.h"
#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
GstElement *__real_gst_element_factory_make (const char *factory, const char *name);
#define gst_element_factory_make __real_gst_element_factory_make
#include "preview-media-fixture.h"
#undef gst_element_factory_make

static GPtrArray *player_pipelines;

GstElement *
__wrap_gst_element_factory_make (const char *factory, const char *name)
{
    GstElement *element = preview_media_factory_make (factory, name);
    if (element != NULL && player_pipelines != NULL && g_str_equal (factory, "playbin"))
        g_ptr_array_add (player_pipelines, gst_object_ref (element));
    return element;
}
#endif
#include <libnemo-private/nemo-query.h>
#include <libxapp/xapp-favorites.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>

static GtkMenu *shown_menu;
static guint popup_count;

GtkWidget *
__wrap_nemo_overview_new (void)
{
    /* Real Overview UI, with fixture volumes instead of scanning the host. */
    return g_object_new (NEMO_TYPE_OVERVIEW, NULL);
}

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
test_restore_overview (void)
{
    Fixture fixture = fixture_new ();
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_RESTORE_TABS_ON_STARTUP, TRUE);
    nemo_window_close (fixture.window);
    g_autofree char *origin_uri = g_file_get_uri (fixture.origin);
    const char *left_uris[] = { origin_uri, "overview:///", NULL };
    const char *right_uris[] = { "overview:///", NULL };
    g_settings_set_strv (nemo_window_state, NEMO_WINDOW_STATE_SAVED_TABS_LEFT, left_uris);
    g_settings_set_strv (nemo_window_state, NEMO_WINDOW_STATE_SAVED_TABS_RIGHT, right_uris);
    g_settings_set_boolean (nemo_window_state, NEMO_WINDOW_STATE_SAVED_SPLIT_VIEW, TRUE);
    g_settings_set_int (nemo_window_state, NEMO_WINDOW_STATE_SAVED_ACTIVE_TAB_LEFT, 1);
    g_settings_set_int (nemo_window_state, NEMO_WINDOW_STATE_SAVED_ACTIVE_PANE, 1);
    reopen_fixture (&fixture, FALSE);
    GList *panes = nemo_window_get_panes (fixture.window);
    NemoWindowPane *left = panes->data;
    NemoWindowPane *right = panes->next->data;
    WAIT_FOR (slot_at (pane_tab (left, 0), fixture.origin));
    WAIT_FOR (gtk_widget_get_mapped (GTK_WIDGET (fixture.window)));
    g_assert_cmpint (gtk_notebook_get_n_pages (GTK_NOTEBOOK (left->notebook)), ==, 2);
    g_assert_cmpint (gtk_notebook_get_n_pages (GTK_NOTEBOOK (right->notebook)), ==, 1);
    g_assert_true (pane_tab (left, 1)->visible);
    g_assert_true (pane_tab (right, 0)->visible);
    g_assert_true (g_file_has_uri_scheme (pane_tab (left, 1)->location, "overview"));
    g_assert_true (g_file_has_uri_scheme (pane_tab (right, 0)->location, "overview"));
    g_assert_null (pane_tab (right, 0)->pending_location);
    g_assert_true (nemo_window_get_active_pane (fixture.window) == right);
    nemo_window_close (fixture.window);
    g_auto(GStrv) saved = g_settings_get_strv (nemo_window_state, NEMO_WINDOW_STATE_SAVED_TABS_LEFT);
    g_assert_cmpstr (saved[0], ==, origin_uri);
    g_assert_cmpstr (saved[1], ==, "overview:///");
    reopen_fixture (&fixture, FALSE);
    WAIT_FOR (gtk_widget_get_mapped (GTK_WIDGET (fixture.window)));
    nemo_window_slot_open_location (fixture.slot, fixture.origin, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    fixture_clear (&fixture);
}

static void
test_overview_history (void)
{
    Fixture fixture = fixture_new ();
    g_autoptr(GFile) overview = g_file_new_for_uri ("overview:///");
    nemo_window_slot_open_location (fixture.slot, overview, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    g_assert_true (g_file_equal (fixture.slot->location, overview));
    g_assert_null (fixture.slot->pending_location);
    g_assert_null (fixture.slot->content_view);
    nemo_window_slot_open_location (fixture.slot, fixture.containing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    nemo_window_back_or_forward (fixture.window, TRUE, 0, 0);
    g_assert_true (g_file_equal (fixture.slot->location, overview));
    nemo_window_back_or_forward (fixture.window, TRUE, 0, 0);
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    nemo_window_back_or_forward (fixture.window, FALSE, 0, 0);
    g_assert_true (g_file_equal (fixture.slot->location, overview));
    nemo_window_back_or_forward (fixture.window, FALSE, 0, 0);
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    fixture_clear (&fixture);
}

static GtkWidget *
overview_find_widget (GtkWidget *root, GType type, const char *button_label)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE (root, type) &&
        (button_label == NULL ||
         g_strcmp0 (gtk_button_get_label (GTK_BUTTON (root)), button_label) == 0))
        return root;
    if (!GTK_IS_CONTAINER (root))
        return NULL;
    GList *children = gtk_container_get_children (GTK_CONTAINER (root));
    GtkWidget *found = NULL;
    for (GList *l = children; l != NULL && found == NULL; l = l->next)
        found = overview_find_widget (l->data, type, button_label);
    g_list_free (children);
    return found;
}

static void
test_overview_heatmap_navigation (void)
{
    Fixture fixture = fixture_new ();
    NemoWindowSlot *left = fixture.slot;
    nemo_window_split_view_on (fixture.window);
    fixture.slot = nemo_window_get_extra_slot (fixture.window);
    nemo_window_slot_open_location (fixture.slot, fixture.origin, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    g_autoptr (GFile) location = g_file_new_for_uri ("overview:///");
    nemo_window_slot_open_location (fixture.slot, location, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    nemo_window_set_active_pane (fixture.window, left->pane);
    GtkWidget *overview = overview_find_widget (fixture.slot->view_overlay, NEMO_TYPE_OVERVIEW, NULL);
    g_assert_nonnull (overview);
    GtkWidget *stack = overview_find_widget (overview, GTK_TYPE_STACK, NULL);
    GtkWidget *combo = overview_find_widget (overview, GTK_TYPE_COMBO_BOX_TEXT, NULL);
    NemoDiskTreemap *map = NEMO_DISK_TREEMAP (
        overview_find_widget (overview, NEMO_TYPE_DISK_TREEMAP, NULL));
    GtkWidget *area = overview_find_widget (GTK_WIDGET (map), GTK_TYPE_DRAWING_AREA, NULL);
    GtkWidget *up = overview_find_widget (GTK_WIDGET (map), GTK_TYPE_BUTTON, "Zoom Out");
    g_autofree char *root = g_file_get_path (fixture.origin);
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), root, "Fixture");
    gtk_combo_box_set_active (GTK_COMBO_BOX (combo), 0);
    gtk_stack_set_visible_child_name (GTK_STACK (stack), "heatmap");
    WAIT_FOR (!nemo_disk_treemap_is_loading (map));
    g_assert_true (nemo_disk_treemap_has_result (map));
    WAIT_FOR (gtk_widget_get_mapped (area));
    cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
        gtk_widget_get_allocated_width (area), gtk_widget_get_allocated_height (area));
    cairo_t *cr = cairo_create (surface);
    gtk_widget_draw (area, cr);
    cairo_destroy (cr);
    cairo_surface_destroy (surface);
    GdkEvent *event = gdk_event_new (GDK_2BUTTON_PRESS);
    event->button.window = g_object_ref (gtk_widget_get_window (area));
    event->button.button = 1;
    event->button.x = event->button.y = 10;
    g_assert_true (gtk_widget_event (area, event));
    gdk_event_free (event);
    g_assert_true (gtk_widget_get_sensitive (up));
    g_assert_true (gtk_widget_has_focus (area));
    g_assert_true (key_press (fixture.window, "BackSpace"));
    WAIT_FOR (!gtk_widget_get_sensitive (up));
    g_assert_true (g_file_equal (fixture.slot->location, location));
    g_assert_true (key_press (fixture.window, "Right"));
    g_assert_true (key_press (fixture.window, "Return"));
    WAIT_FOR (gtk_widget_get_sensitive (up));
    g_assert_true (key_press (fixture.window, "Right"));
    g_assert_true (key_press (fixture.window, "Return"));
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    g_assert_true (g_file_equal (left->location, fixture.origin));
    g_assert_true (nemo_window_get_active_slot (fixture.window) == fixture.slot);
    GList *selection = nemo_view_get_selection (fixture.slot->content_view);
    g_assert_cmpuint (g_list_length (selection), ==, 1);
    g_autoptr (GFile) selected = nemo_file_get_location (selection->data);
    g_assert_true (g_file_equal (selected, fixture.item));
    nemo_file_list_free (selection);
    fixture_clear (&fixture);
}

static void
assert_no_error_dialog (void)
{
    GList *windows = gtk_window_list_toplevels ();
    for (GList *l = windows; l != NULL; l = l->next)
        g_assert_false (GTK_IS_MESSAGE_DIALOG (l->data) &&
                        gtk_widget_get_visible (GTK_WIDGET (l->data)));
    g_list_free (windows);
}

static void
record_open_error (NemoWindow *window, GError *error, gpointer user_data)
{
    GError **result = user_data;
    g_assert_nonnull (error);
    *result = g_error_copy (error);
}

static void
test_restore_missing_folder (void)
{
    Fixture fixture = fixture_new ();
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_RESTORE_TABS_ON_STARTUP, TRUE);
    nemo_window_close (fixture.window);
    g_autofree char *path = g_build_filename (g_getenv ("NEMO_TEST_PROFILE"), "unplugged", "folder", NULL);
    g_autoptr(GFile) missing = g_file_new_for_path (path);
    g_autofree char *uri = g_file_get_uri (missing);
    const char *uris[] = { uri, NULL };
    g_settings_set_strv (nemo_window_state, NEMO_WINDOW_STATE_SAVED_TABS_LEFT, uris);
    reopen_fixture (&fixture, FALSE);
    g_autoptr(GFile) home = g_file_new_for_path (g_get_home_dir ());
    WAIT_FOR (slot_at (fixture.slot, home));
    g_assert_true (gtk_widget_get_visible (GTK_WIDGET (fixture.window)));
    assert_no_error_dialog ();
    nemo_window_close (fixture.window);
    assert_saved_folder (NEMO_WINDOW_STATE_SAVED_TABS_LEFT, home);
    reopen_fixture (&fixture, FALSE);
    WAIT_FOR (slot_at (fixture.slot, home));
    assert_no_error_dialog ();
    /* A later explicit request must still report its error, not restore Home. */
    g_autoptr(GError) error = NULL;
    nemo_window_slot_open_location_full (fixture.slot, missing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT,
                                        NULL, record_open_error, &error);
    WAIT_FOR (error != NULL);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    WAIT_FOR (slot_at (fixture.slot, home));
    fixture_clear (&fixture);
}

static void
test_restore_missing_pane_tabs (void)
{
    Fixture fixture = fixture_new ();
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_RESTORE_TABS_ON_STARTUP, TRUE);
    nemo_window_close (fixture.window);
    g_autofree char *missing_path = g_build_filename (g_getenv ("NEMO_TEST_PROFILE"), "unplugged", NULL);
    g_autoptr(GFile) missing = g_file_new_for_path (missing_path);
    g_autofree char *missing_uri = g_file_get_uri (missing);
    g_autofree char *origin_uri = g_file_get_uri (fixture.origin);
    g_autofree char *containing_uri = g_file_get_uri (fixture.containing);
    /* A formerly saved directory can also have been replaced by a file. */
    g_autofree char *item_uri = g_file_get_uri (fixture.item);
    g_autofree char *not_directory_uri = g_strconcat (item_uri, "/child", NULL);
    const char *left_uris[] = { missing_uri, origin_uri, NULL };
    const char *right_uris[] = { containing_uri, not_directory_uri, missing_uri, NULL };
    g_settings_set_strv (nemo_window_state, NEMO_WINDOW_STATE_SAVED_TABS_LEFT, left_uris);
    g_settings_set_strv (nemo_window_state, NEMO_WINDOW_STATE_SAVED_TABS_RIGHT, right_uris);
    g_settings_set_boolean (nemo_window_state, NEMO_WINDOW_STATE_SAVED_SPLIT_VIEW, TRUE);
    g_settings_set_int (nemo_window_state, NEMO_WINDOW_STATE_SAVED_ACTIVE_TAB_LEFT, 1);
    g_settings_set_int (nemo_window_state, NEMO_WINDOW_STATE_SAVED_ACTIVE_TAB_RIGHT, 2);
    g_settings_set_int (nemo_window_state, NEMO_WINDOW_STATE_SAVED_ACTIVE_PANE, 1);
    reopen_fixture (&fixture, FALSE);
    GtkPaned *paned = GTK_PANED (fixture.window->details->split_view_hpane);
    NemoWindowPane *left = NEMO_WINDOW_PANE (gtk_paned_get_child1 (paned));
    NemoWindowPane *right = NEMO_WINDOW_PANE (gtk_paned_get_child2 (paned));
    g_autoptr(GFile) home = g_file_new_for_path (g_get_home_dir ());
    WAIT_FOR (slot_at (pane_tab (left, 0), home));
    WAIT_FOR (slot_at (pane_tab (left, 1), fixture.origin));
    WAIT_FOR (slot_at (pane_tab (right, 0), fixture.containing));
    WAIT_FOR (slot_at (pane_tab (right, 1), home));
    WAIT_FOR (slot_at (pane_tab (right, 2), home));
    g_assert_cmpint (gtk_notebook_get_n_pages (GTK_NOTEBOOK (left->notebook)), ==, 2);
    g_assert_cmpint (gtk_notebook_get_n_pages (GTK_NOTEBOOK (right->notebook)), ==, 3);
    g_assert_cmpint (gtk_notebook_get_current_page (GTK_NOTEBOOK (left->notebook)), ==, 1);
    g_assert_cmpint (gtk_notebook_get_current_page (GTK_NOTEBOOK (right->notebook)), ==, 2);
    g_assert_true (nemo_window_get_active_pane (fixture.window) == right);
    assert_no_error_dialog ();
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

#ifdef HAVE_GSTREAMER
static gboolean
pipeline_playing (guint index)
{
    if (player_pipelines->len <= index)
        return FALSE;
    GstState state;
    gst_element_get_state (g_ptr_array_index (player_pipelines, index), &state, NULL, 0);
    return state == GST_STATE_PLAYING;
}

static GList *
preview_windows (void)
{
    GList *windows = gtk_window_list_toplevels (), *previews = NULL;
    for (GList *l = windows; l != NULL; l = l->next)
        if (NEMO_IS_QUICK_PREVIEW (l->data) && gtk_widget_get_visible (l->data))
            previews = g_list_prepend (previews, l->data);
    g_list_free (windows);
    return previews;
}

static void
activate_f3 (Fixture *fixture, GFile *location)
{
    NemoFile *file = nemo_file_get (location);
    GList selection = { .data = file };
    nemo_view_set_selection (fixture->slot->content_view, &selection);
    nemo_view_grab_focus (fixture->slot->content_view);
    g_assert_true (gtk_accel_groups_activate (G_OBJECT (fixture->window), GDK_KEY_F3, 0));
    nemo_file_unref (file);
}

static void
test_detached_media_players (void)
{
    gst_init (NULL, NULL);
    Fixture fixture = fixture_new ();
    g_autofree char *encoded = preview_media_encode_fixture (
        "audiotestsrc num-buffers=200 samplesperbuffer=4800 ! audio/x-raw,rate=48000 ! "
        "wavenc ! filesink name=output", "wav");
    g_autofree char *directory = g_file_get_path (fixture.containing);
    g_autofree char *first_path = g_build_filename (directory, "a.wav", NULL);
    g_autofree char *second_path = g_build_filename (directory, "b.wav", NULL);
    g_assert_cmpint (g_rename (encoded, first_path), ==, 0);
    GFile *first = g_file_new_for_path (first_path);
    GFile *second = g_file_new_for_path (second_path);
    g_assert_true (g_file_copy (first, second, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL));
    nemo_window_slot_open_location (fixture.slot, fixture.containing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    player_pipelines = g_ptr_array_new_with_free_func ((GDestroyNotify) gst_object_unref);
    activate_f3 (&fixture, first);
    WAIT_FOR (pipeline_playing (0));
    activate_f3 (&fixture, second);
    WAIT_FOR (pipeline_playing (1));
    g_assert_cmpuint (player_pipelines->len, ==, 2);
    GList *players = preview_windows ();
    g_assert_cmpuint (g_list_length (players), ==, 2);
    for (GList *l = players; l != NULL; l = l->next) {
        g_assert_null (gtk_window_get_transient_for (l->data));
        g_assert_false (gtk_window_get_skip_taskbar_hint (l->data));
        g_assert_cmpint (gtk_window_get_type_hint (l->data), ==, GDK_WINDOW_TYPE_HINT_NORMAL);
        g_assert_cmpfloat (gtk_widget_get_opacity (l->data), ==, 1);
    }
    activate_f3 (&fixture, fixture.item);
    NemoQuickPreview *document = nemo_quick_preview_get_instance ();
    WAIT_FOR (gtk_widget_get_visible (GTK_WIDGET (document)));
    g_assert_null (g_list_find (players, document));
    g_assert_true (pipeline_playing (0));
    g_assert_true (pipeline_playing (1));
    gint64 duration;
    GstElement *first_pipeline = g_ptr_array_index (player_pipelines, 0);
    g_assert_true (gst_element_query_duration (first_pipeline, GST_FORMAT_TIME, &duration));
    g_assert_true (gst_element_seek_simple (first_pipeline, GST_FORMAT_TIME,
        GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, duration - GST_SECOND / 5));
    WAIT_FOR (pipeline_playing (2));
    g_autofree char *uri = NULL;
    g_autofree char *expected = g_file_get_uri (second);
    g_object_get (g_ptr_array_index (player_pipelines, 2), "uri", &uri, NULL);
    g_assert_cmpstr (uri, ==, expected);
    g_assert_true (gtk_widget_get_visible (GTK_WIDGET (document)));
    gtk_widget_destroy (GTK_WIDGET (fixture.window));
    g_assert_null (gtk_application_get_windows (GTK_APPLICATION (application)));
    for (GList *l = players; l != NULL; l = l->next)
        g_assert_true (gtk_widget_get_visible (l->data));
    g_assert_true (pipeline_playing (1));
    g_assert_true (pipeline_playing (2));
    for (GList *l = players; l != NULL; l = l->next)
        gtk_widget_destroy (l->data);
    g_list_free (players);
    nemo_quick_preview_dismiss (document);
    g_ptr_array_unref (player_pipelines);
    player_pipelines = NULL;
    g_assert_true (g_file_delete (first, NULL, NULL));
    g_assert_true (g_file_delete (second, NULL, NULL));
    g_object_unref (first);
    g_object_unref (second);
    fixture_clear (&fixture);
}
#endif

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
wait_for_window_title (const char *expected, guint expected_count)
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
        guint found = 0;
        g_assert_true (XQueryTree (xdisplay, DefaultRootWindow (xdisplay),
                                  &root, &parent, &windows, &count));
        /* Other processes may destroy a window between QueryTree and FetchName. */
        gdk_x11_display_error_trap_push (display);
        for (guint i = 0; i < count; i++) {
            char *title = NULL;
            XWindowAttributes attributes;
            if (XFetchName (xdisplay, windows[i], &title) && title != NULL) {
                if (g_str_equal (title, expected) &&
                    XGetWindowAttributes (xdisplay, windows[i], &attributes) &&
                    attributes.map_state == IsViewable)
                    found++;
                g_string_append_printf (observed, "'%s' ", title);
            }
            if (title != NULL)
                XFree (title);
        }
        gdk_x11_display_error_trap_pop_ignored (display);
        XFree (windows);
        if (found == expected_count) {
            g_string_free (observed, TRUE);
            return 0;
        }
        g_usleep (20000);
    }
    g_printerr ("Expected %u mapped windows titled '%s'; observed: %s\n",
                expected_count, expected, observed->str);
    g_string_free (observed, TRUE);
    return 1;
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    if ((argc == 3 || argc == 4) && g_str_equal (argv[1], "--wait-for-title")) {
        guint count = argc == 4 ? g_ascii_strtoull (argv[3], NULL, 10) : 1;
        g_assert_cmpuint (count, >, 0);
        return wait_for_window_title (argv[2], count);
    }
    gtk_test_init (&argc, &argv, NULL);
    static const FolderCase cases[] = {
        { "/folders/restore-panes-and-tabs", test_restore_panes_and_tabs },
        { "/folders/restore-overview", test_restore_overview },
        { "/folders/overview-history", test_overview_history },
        { "/folders/overview-heatmap-navigation", test_overview_heatmap_navigation },
        { "/folders/last-tab-close", test_last_tab_close },
        { "/folders/restore-missing-folder", test_restore_missing_folder },
        { "/folders/restore-missing-pane-tabs", test_restore_missing_pane_tabs },
        { "/folders/disabled-restore", test_disabled_restore },
        { "/folders/explicit-location", test_explicit_location },
        { "/folders/search-origin", test_search_origin_saved },
        { "/folders/breadcrumb-targets", test_breadcrumb_targets },
        { "/folders/current-folder-shortcuts", test_current_folder_shortcuts },
#ifdef HAVE_GSTREAMER
        { "/folders/detached-f3-media-players", test_detached_media_players },
#endif
    };
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
        g_test_add_data_func (cases[i].path, &cases[i], run_case);
    return g_test_run ();
}
