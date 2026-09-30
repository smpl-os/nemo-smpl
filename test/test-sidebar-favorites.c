#include <config.h>
#include "window-test-fixture.h"
#include "../src/nemo-places-sidebar.c"

static guint collection_opens;
static char *launched_uri;
static NemoWindowOpenFlags launched_flags;
static gboolean capture_launch;

gboolean __real_eel_vfs_supports_uri_scheme (const char *scheme);
gboolean
__wrap_eel_vfs_supports_uri_scheme (const char *scheme)
{
    return g_str_equal (scheme, "favorites") || __real_eel_vfs_supports_uri_scheme (scheme);
}

void __real_nemo_window_slot_open_location_full (NemoWindowSlot *slot, GFile *location,
    NemoWindowOpenFlags flags, GList *selection, NemoWindowGoToCallback callback, gpointer data);
void
__wrap_nemo_window_slot_open_location_full (NemoWindowSlot *slot, GFile *location,
    NemoWindowOpenFlags flags, GList *selection, NemoWindowGoToCallback callback, gpointer data)
{
    if (g_file_has_uri_scheme (location, "favorites"))
        collection_opens++;
    else
        __real_nemo_window_slot_open_location_full (slot, location, flags, selection, callback, data);
}

void __real_nemo_mime_activate_file (GtkWindow *window, NemoWindowSlot *slot, NemoFile *file,
                                    const char *directory, NemoWindowOpenFlags flags);
void
__wrap_nemo_mime_activate_file (GtkWindow *window, NemoWindowSlot *slot, NemoFile *file,
                               const char *directory, NemoWindowOpenFlags flags)
{
    if (!capture_launch) {
        __real_nemo_mime_activate_file (window, slot, file, directory, flags);
        return;
    }
    g_free (launched_uri);
    launched_uri = nemo_file_get_uri (file);
    launched_flags = flags;
}

static void
add_favorite (GFile *file)
{
    g_autofree char *uri = g_file_get_uri (file);
    xapp_favorites_add (xapp_favorites_get_default (), uri);
    WAIT_FOR (xapp_favorites_find_by_uri (xapp_favorites_get_default (), uri) != NULL);
}

static gboolean
find_favorites (NemoPlacesSidebar *sidebar, GtkTreeIter *result)
{
    GtkTreeModel *model = sidebar->store_filter;
    GtkTreeIter section, child;
    if (!gtk_tree_model_get_iter_first (model, &section))
        return FALSE;
    if (gtk_tree_model_iter_children (model, &child, &section)) {
        do {
            gint type;
            gtk_tree_model_get (model, &child, PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type, -1);
            if (type == PLACES_FAVORITES) {
                *result = child;
                return TRUE;
            }
        } while (gtk_tree_model_iter_next (model, &child));
    }
    return FALSE;
}

static guint
favorite_count (NemoPlacesSidebar *sidebar)
{
    GtkTreeIter root;
    return find_favorites (sidebar, &root) ? gtk_tree_model_iter_n_children (sidebar->store_filter, &root) : 0;
}

static GtkTreePath *
favorites_path (NemoPlacesSidebar *sidebar)
{
    GtkTreeIter iter;
    g_assert_true (find_favorites (sidebar, &iter));
    return gtk_tree_model_get_path (sidebar->store_filter, &iter);
}

static NemoPlacesSidebar *
find_sidebar (GtkWidget *widget)
{
    if (NEMO_IS_PLACES_SIDEBAR (widget))
        return NEMO_PLACES_SIDEBAR (widget);
    NemoPlacesSidebar *result = NULL;
    if (GTK_IS_CONTAINER (widget)) {
        GList *children = gtk_container_get_children (GTK_CONTAINER (widget));
        for (GList *l = children; result == NULL && l != NULL; l = l->next)
            result = find_sidebar (l->data);
        g_list_free (children);
    }
    return result;
}

static NemoPlacesSidebar *
fixture_sidebar (Fixture *fixture)
{
    NemoPlacesSidebar *sidebar = find_sidebar (GTK_WIDGET (fixture->window));
    g_assert_nonnull (sidebar);
    return sidebar;
}

static void
test_favorites_contents (void)
{
    Fixture fixture = fixture_new ();
    NemoPlacesSidebar *sidebar = fixture_sidebar (&fixture);
    add_favorite (fixture.containing);
    add_favorite (fixture.item);
    add_favorite (fixture.origin);
    WAIT_FOR (favorite_count (sidebar) == 3);
    GtkTreeIter parent, item;
    g_assert_true (find_favorites (sidebar, &parent));
    g_assert_true (gtk_tree_model_iter_children (sidebar->store_filter, &item, &parent));
    char *previous = NULL;
    guint folders = 0, files = 0;
    do {
        char *uri, *name, *tooltip;
        gint type, index;
        GIcon *icon;
        gtk_tree_model_get (sidebar->store_filter, &item,
            PLACES_SIDEBAR_COLUMN_URI, &uri, PLACES_SIDEBAR_COLUMN_NAME, &name,
            PLACES_SIDEBAR_COLUMN_TOOLTIP, &tooltip, PLACES_SIDEBAR_COLUMN_GICON, &icon,
            PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type, PLACES_SIDEBAR_COLUMN_INDEX, &index, -1);
        g_assert_cmpint (type, ==, PLACES_FAVORITE);
        g_assert_cmpint (index, ==, -1);
        g_assert_nonnull (icon);
        g_assert_nonnull (tooltip);
        if (previous != NULL)
            g_assert_cmpint (g_utf8_collate (previous, name), <=, 0);
        GFile *location = g_file_new_for_uri (uri);
        GtkTreePath *path = gtk_tree_model_get_path (sidebar->store_filter, &item);
        GtkTreePath *child_path = gtk_tree_model_filter_convert_path_to_child_path (
            GTK_TREE_MODEL_FILTER (sidebar->store_filter), path);
        g_assert_false (gtk_tree_drag_source_row_draggable (GTK_TREE_DRAG_SOURCE (sidebar->store), child_path));
        gtk_tree_path_free (child_path);
        GdkRectangle rect;
        gtk_tree_view_get_background_area (sidebar->tree_view, path,
                                           sidebar->name_column, &rect);
        GtkTreePath *drop_path = NULL;
        GtkTreeViewDropPosition position;
        sidebar->drag_data_received = TRUE;
        sidebar->drag_data_info = TEXT_URI_LIST;
        gboolean can_drop = compute_drop_position (sidebar->tree_view,
            rect.x + rect.width / 2, rect.y + rect.height / 2, &drop_path, &position, sidebar);
        g_assert_cmpint (can_drop, ==, !g_file_equal (location, fixture.item));
        if (can_drop)
            g_assert_cmpint (position, ==, GTK_TREE_VIEW_DROP_INTO_OR_BEFORE);
        sidebar->drag_data_received = FALSE;
        gtk_tree_path_free (drop_path);
        gtk_tree_path_free (path);
        if (g_file_equal (location, fixture.item))
            files++;
        else {
            g_assert_true (g_file_equal (location, fixture.origin) ||
                           g_file_equal (location, fixture.containing));
            folders++;
        }
        g_free (previous);
        previous = name;
        g_free (uri);
        g_free (tooltip);
        g_object_unref (icon);
        g_object_unref (location);
    } while (gtk_tree_model_iter_next (sidebar->store_filter, &item));
    g_free (previous);
    g_assert_cmpuint (folders, ==, 2);
    g_assert_cmpuint (files, ==, 1);

    g_autofree char *uri = g_file_get_uri (fixture.item);
    xapp_favorites_remove (xapp_favorites_get_default (), uri);
    WAIT_FOR (favorite_count (sidebar) == 2);
    g_autofree char *origin_uri = g_file_get_uri (fixture.origin);
    g_autofree char *folder_uri = g_file_get_uri (fixture.containing);
    xapp_favorites_remove (xapp_favorites_get_default (), origin_uri);
    xapp_favorites_remove (xapp_favorites_get_default (), folder_uri);
    WAIT_FOR (!find_favorites (sidebar, &parent));
    add_favorite (fixture.containing);
    WAIT_FOR (favorite_count (sidebar) == 1);
    fixture_clear (&fixture);
}

static void
test_expansion_and_navigation (void)
{
    Fixture fixture = fixture_new ();
    NemoPlacesSidebar *sidebar = fixture_sidebar (&fixture);
    add_favorite (fixture.containing);
    add_favorite (fixture.item);
    WAIT_FOR (favorite_count (sidebar) == 2);
    GtkTreePath *path = favorites_path (sidebar);
    g_assert_true (gtk_tree_view_row_expanded (sidebar->tree_view, path));
    GtkTreeIter root, next;
    gtk_tree_model_get_iter (sidebar->store_filter, &root, path);
    next = root;
    g_assert_true (find_next_row (sidebar, &next));
    gint type;
    gtk_tree_model_get (sidebar->store_filter, &next, PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type, -1);
    g_assert_cmpint (type, ==, PLACES_FAVORITE);
    g_assert_true (find_prev_row (sidebar, &next));
    gtk_tree_model_get (sidebar->store_filter, &next, PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type, -1);
    g_assert_cmpint (type, ==, PLACES_FAVORITES);
    for (guint i = 0; i < 3; i++)
        g_assert_true (find_next_row (sidebar, &next));
    gtk_tree_model_get (sidebar->store_filter, &next, PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type, -1);
    g_assert_cmpint (type, !=, PLACES_FAVORITE);
    g_assert_true (find_prev_row (sidebar, &next));
    gtk_tree_model_get (sidebar->store_filter, &next, PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type, -1);
    g_assert_cmpint (type, ==, PLACES_FAVORITE);

    gtk_tree_view_collapse_row (sidebar->tree_view, path);
    g_assert_false (g_settings_get_boolean (nemo_window_state, NEMO_WINDOW_STATE_FAVORITES_EXPANDED));
    g_assert_true (g_settings_get_boolean (nemo_window_state, NEMO_WINDOW_STATE_MY_COMPUTER_EXPANDED));
    add_favorite (fixture.origin);
    WAIT_FOR (favorite_count (sidebar) == 3);
    g_assert_false (gtk_tree_view_row_expanded (sidebar->tree_view, path));
    update_places (sidebar);
    g_assert_false (gtk_tree_view_row_expanded (sidebar->tree_view, path));
    gtk_tree_model_get_iter (sidebar->store_filter, &next, path);
    g_assert_true (find_next_row (sidebar, &next));
    gtk_tree_model_get (sidebar->store_filter, &next, PLACES_SIDEBAR_COLUMN_ROW_TYPE, &type, -1);
    g_assert_cmpint (type, !=, PLACES_FAVORITE);

    GtkWidget *other = nemo_places_sidebar_new (fixture.window);
    g_object_ref_sink (other);
    g_assert_false (NEMO_PLACES_SIDEBAR (other)->favorites_expanded);
    gtk_widget_destroy (other);
    g_object_unref (other);

    gtk_tree_view_expand_row (sidebar->tree_view, path, FALSE);
    GtkTreePath *computer = gtk_tree_path_new_from_indices (0, -1);
    gtk_tree_view_collapse_row (sidebar->tree_view, computer);
    update_places (sidebar);
    g_assert_false (gtk_tree_view_row_expanded (sidebar->tree_view, computer));
    gtk_tree_view_expand_row (sidebar->tree_view, computer, FALSE);
    g_assert_true (gtk_tree_view_row_expanded (sidebar->tree_view, path));
    gtk_tree_path_free (computer);
    gtk_tree_path_free (path);
    fixture_clear (&fixture);
}

static void
test_place_icons (void)
{
    g_settings_set_boolean (nemo_desktop_preferences, NEMO_PREFERENCES_SHOW_DESKTOP, TRUE);
    Fixture fixture = fixture_new ();
    NemoPlacesSidebar *sidebar = fixture_sidebar (&fixture);
    add_favorite (fixture.containing);
    WAIT_FOR (favorite_count (sidebar) == 1);
    g_autofree char *desktop = nemo_get_desktop_directory ();
    g_autofree char *desktop_uri = g_filename_to_uri (desktop, NULL, NULL);
    GtkTreeIter section, child;
    g_assert_true (gtk_tree_model_get_iter_first (sidebar->store_filter, &section));
    g_assert_true (gtk_tree_model_iter_children (sidebar->store_filter, &child, &section));
    guint found = 0;
    do {
        g_autofree char *uri = NULL;
        GIcon *icon = NULL;
        gtk_tree_model_get (sidebar->store_filter, &child,
                            PLACES_SIDEBAR_COLUMN_URI, &uri,
                            PLACES_SIDEBAR_COLUMN_GICON, &icon, -1);
        const char *expected = g_strcmp0 (uri, desktop_uri) == 0 ? NEMO_ICON_SYMBOLIC_DESKTOP :
                               g_strcmp0 (uri, "favorites:///") == 0 ? NEMO_ICON_SYMBOLIC_FOLDER_FAVORITES : NULL;
        if (expected != NULL) {
            g_assert_true (G_IS_THEMED_ICON (icon));
            g_assert_cmpstr (g_themed_icon_get_names (G_THEMED_ICON (icon))[0], ==, expected);
            found++;
        }
        g_clear_object (&icon);
    } while (gtk_tree_model_iter_next (sidebar->store_filter, &child));
    g_assert_cmpuint (found, ==, 2);
    fixture_clear (&fixture);
}

static void
test_activation (void)
{
    Fixture fixture = fixture_new ();
    NemoPlacesSidebar *sidebar = fixture_sidebar (&fixture);
    g_assert_cmpint (gtk_tree_view_get_n_columns (sidebar->tree_view), ==, 2);
    g_assert_true (gtk_tree_view_get_column (sidebar->tree_view, 0) == sidebar->name_column);
    g_assert_true (gtk_tree_view_get_expander_column (sidebar->tree_view) == sidebar->name_column);
    add_favorite (fixture.containing);
    add_favorite (fixture.item);
    WAIT_FOR (favorite_count (sidebar) == 2);
    GtkTreePath *path = favorites_path (sidebar);
    GtkTreeViewColumn *column = gtk_tree_view_get_expander_column (sidebar->tree_view);
    GdkRectangle rect, cell;
    gtk_tree_view_get_background_area (sidebar->tree_view, path, column, &rect);
    gtk_tree_view_get_cell_area (sidebar->tree_view, path, column, &cell);
    gint expander_size;
    gtk_widget_style_get (GTK_WIDGET (sidebar->tree_view), "expander-size", &expander_size, NULL);
    gboolean rtl = gtk_widget_get_direction (GTK_WIDGET (sidebar->tree_view)) == GTK_TEXT_DIR_RTL;
    GdkEventButton event = { .type = GDK_BUTTON_RELEASE, .button = GDK_BUTTON_PRIMARY,
        .window = gtk_tree_view_get_bin_window (sidebar->tree_view),
        .x = rtl ? cell.x + cell.width + expander_size / 2 : cell.x - expander_size / 2,
        .y = rect.y + rect.height / 2 };
    g_test_message ("Expander background %d,%d %dx%d, cell %d,%d %dx%d, size %d",
                    rect.x, rect.y, rect.width, rect.height, cell.x, cell.y, cell.width, cell.height, expander_size);
    GtkTreePath *hit = NULL;
    g_assert_true (gtk_tree_view_get_path_at_pos (sidebar->tree_view, event.x, event.y,
                                                 &hit, NULL, NULL, NULL));
    g_assert_cmpint (gtk_tree_path_compare (hit, path), ==, 0);
    gtk_tree_path_free (hit);
    bookmarks_button_release_event_cb (GTK_WIDGET (sidebar->tree_view), &event, sidebar);
    g_assert_cmpuint (collection_opens, ==, 0);
    g_assert_null (fixture.slot->pending_location);
    GdkEvent *click = gdk_event_new (GDK_BUTTON_PRESS);
    click->button.window = g_object_ref (event.window);
    click->button.button = GDK_BUTTON_PRIMARY;
    click->button.x = event.x;
    click->button.y = event.y;
    GdkDevice *pointer = gdk_seat_get_pointer (
        gdk_display_get_default_seat (gtk_widget_get_display (GTK_WIDGET (sidebar->tree_view))));
    gdk_event_set_device (click, pointer);
    GdkEvent *motion = gdk_event_new (GDK_MOTION_NOTIFY);
    motion->motion.window = g_object_ref (event.window);
    motion->motion.x = event.x;
    motion->motion.y = event.y;
    gdk_event_set_device (motion, pointer);
    gtk_widget_event (GTK_WIDGET (sidebar->tree_view), motion);
    gdk_event_free (motion);
    gtk_widget_event (GTK_WIDGET (sidebar->tree_view), click);
    click->type = GDK_BUTTON_RELEASE;
    gtk_widget_event (GTK_WIDGET (sidebar->tree_view), click);
    gdk_event_free (click);
    WAIT_FOR (!gtk_tree_view_row_expanded (sidebar->tree_view, path));
    g_assert_cmpuint (collection_opens, ==, 0);
    g_assert_null (fixture.slot->pending_location);
    gtk_tree_view_expand_row (sidebar->tree_view, path, FALSE);

    GtkTreeIter parent, item;
    gtk_tree_model_get_iter (sidebar->store_filter, &parent, path);
    gtk_tree_view_get_cell_area (sidebar->tree_view, path, column, &cell);
    event.x = cell.x + cell.width / 2;
    bookmarks_button_release_event_cb (GTK_WIDGET (sidebar->tree_view), &event, sidebar);
    g_assert_cmpuint (collection_opens, ==, 1);
    gtk_tree_view_set_cursor (sidebar->tree_view, path, NULL, FALSE);
    gtk_widget_grab_focus (GTK_WIDGET (sidebar->tree_view));
    g_assert_true (key_press (fixture.window, "Return"));
    WAIT_FOR (collection_opens == 2);
    gtk_tree_path_free (path);
    g_assert_true (gtk_tree_model_iter_children (sidebar->store_filter, &item, &parent));
    do {
        g_autofree char *uri = NULL;
        gtk_tree_model_get (sidebar->store_filter, &item, PLACES_SIDEBAR_COLUMN_URI, &uri, -1);
        GFile *file = g_file_new_for_uri (uri);
        if (g_file_equal (file, fixture.item)) {
            capture_launch = TRUE;
            open_selected_bookmark (sidebar, sidebar->store_filter, &item, NEMO_WINDOW_OPEN_FLAG_NEW_TAB);
            capture_launch = FALSE;
            g_assert_cmpstr (launched_uri, ==, uri);
            g_assert_cmpint (launched_flags, ==, NEMO_WINDOW_OPEN_FLAG_NEW_TAB);
            g_assert_null (fixture.slot->pending_location);
        }
        g_object_unref (file);
    } while (gtk_tree_model_iter_next (sidebar->store_filter, &item));
    g_assert_nonnull (launched_uri);
    g_free (launched_uri);
    launched_uri = NULL;
    g_assert_true (find_favorites (sidebar, &parent));
    gtk_tree_model_iter_children (sidebar->store_filter, &item, &parent);
    do {
        g_autofree char *uri = NULL;
        gtk_tree_model_get (sidebar->store_filter, &item, PLACES_SIDEBAR_COLUMN_URI, &uri, -1);
        GFile *file = g_file_new_for_uri (uri);
        gboolean folder = g_file_equal (file, fixture.containing);
        g_object_unref (file);
        if (folder) {
            open_selected_bookmark (sidebar, sidebar->store_filter, &item, 0);
            break;
        }
    } while (gtk_tree_model_iter_next (sidebar->store_filter, &item));
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    fixture_clear (&fixture);
}

typedef struct { const char *path; GTestFunc run; gboolean rtl; } FavoriteCase;

static void
run_case (gconstpointer data)
{
    const FavoriteCase *test = data;
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 30000000, 0);
        g_test_trap_assert_passed ();
        return;
    }
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    if (test->rtl)
        gtk_widget_set_default_direction (GTK_TEXT_DIR_RTL);
    application = nemo_main_application_get_singleton ();
    GError *error = NULL;
    g_assert_true (g_application_register (G_APPLICATION (application), NULL, &error));
    g_assert_no_error (error);
    test->run ();
    g_object_unref (application);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    gtk_test_init (&argc, &argv, NULL);
    static const FavoriteCase cases[] = {
        { "/sidebar/favorites-contents", test_favorites_contents, FALSE },
        { "/sidebar/favorites-expansion-navigation", test_expansion_and_navigation, FALSE },
        { "/sidebar/favorites-activation", test_activation, FALSE },
        { "/sidebar/favorites-activation-rtl", test_activation, TRUE },
        { "/sidebar/distinct-place-icons", test_place_icons, FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
        g_test_add_data_func (cases[i].path, &cases[i], run_case);
    return g_test_run ();
}
