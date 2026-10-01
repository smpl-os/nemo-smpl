/* Full Nemo-window routing tests. Dialogs are real; every chooser is cancelled. */
#include <config.h>
#include "window-test-fixture.h"
#include "../src/nemo-actions.h"
#include "../src/nemo-archive-dialog.h"
#include "../src/nemo-icon-view.h"
#include "../src/nemo-list-model.h"
#include "../src/nemo-list-view.h"

static struct {
    guint calls;
    GList *sources;
    GFile *directory;
    gboolean move;
} recorded;

void __real_nemo_archive_dialog_show (GtkWindow *, GList *, GFile *, gboolean);

void
__wrap_nemo_archive_dialog_show (GtkWindow *parent, GList *sources,
                                 GFile *directory, gboolean move)
{
    g_assert_cmpuint (recorded.calls++, ==, 0);
    for (GList *l = sources; l; l = l->next) {
        g_assert_false (g_file_has_uri_scheme (l->data, "nemo-parent"));
        recorded.sources = g_list_append (recorded.sources, g_object_ref (l->data));
    }
    recorded.directory = g_object_ref (directory);
    recorded.move = move;
    __real_nemo_archive_dialog_show (parent, sources, directory, move);
}

static GtkAction *
find_action (NemoWindow *window, const char *name)
{
    for (GList *l = gtk_ui_manager_get_action_groups (nemo_window_get_ui_manager (window)); l; l = l->next) {
        GtkAction *action = gtk_action_group_get_action (l->data, name);
        if (action)
            return action;
    }
    g_error ("Missing action: %s", name);
    return NULL;
}

static GtkWidget *
find_chooser (void)
{
    GList *windows = gtk_window_list_toplevels ();
    GtkWidget *chooser = NULL;
    for (GList *l = windows; l; l = l->next)
        if (GTK_IS_FILE_CHOOSER (l->data) && gtk_widget_get_visible (l->data)) {
            g_assert_null (chooser);
            chooser = l->data;
        }
    g_list_free (windows);
    return chooser;
}

static gboolean
chooser_at (GtkWidget *chooser, GFile *directory)
{
    g_autoptr (GFile) folder = gtk_file_chooser_get_current_folder_file (GTK_FILE_CHOOSER (chooser));
    return folder && g_file_equal (folder, directory);
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

static void
find_parent_icon (NemoIconData *data, gpointer user_data)
{
    NemoFile **parent = user_data;
    if (nemo_file_is_parent_entry (NEMO_FILE (data))) {
        g_assert_null (*parent);
        *parent = nemo_file_ref (NEMO_FILE (data));
    }
}

static NemoFile *
parent_entry (NemoView *view)
{
    NemoFile *parent = NULL;
    if (NEMO_IS_ICON_VIEW (view)) {
        nemo_icon_container_for_each (nemo_icon_view_get_icon_container (NEMO_ICON_VIEW (view)),
                                      find_parent_icon, &parent);
    } else {
        GtkTreeView *tree = NULL;
        find_tree (GTK_WIDGET (view), &tree);
        g_assert_nonnull (tree);
        GtkTreeModel *model = gtk_tree_view_get_model (tree);
        GtkTreeIter iter;
        gboolean valid = gtk_tree_model_get_iter_first (model, &iter);
        while (valid) {
            NemoFile *file = NULL;
            gtk_tree_model_get (model, &iter, NEMO_LIST_MODEL_FILE_COLUMN, &file, -1);
            if (nemo_file_is_parent_entry (file)) {
                g_assert_null (parent);
                parent = nemo_file_ref (file);
            }
            nemo_file_unref (file);
            valid = gtk_tree_model_iter_next (model, &iter);
        }
    }
    g_assert_nonnull (parent);
    return parent;
}

static void
send_key_event (NemoWindow *window, const char *accelerator)
{
    guint key;
    GdkModifierType modifiers;
    gtk_accelerator_parse (accelerator, &key, &modifiers);
    GdkKeymapKey *entries = NULL;
    gint count;
    GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (window));
    g_assert_true (gdk_keymap_get_entries_for_keyval (
        gdk_keymap_get_for_display (display), key, &entries, &count));
    GdkEvent *event = gdk_event_new (GDK_KEY_PRESS);
    event->key.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (window)));
    event->key.keyval = key;
    event->key.state = modifiers;
    event->key.hardware_keycode = entries[0].keycode;
    event->key.group = entries[0].group;
    gdk_event_set_device (event, gdk_seat_get_keyboard (gdk_display_get_default_seat (display)));
    /* Exercise NemoWindow's editable/rename guard, not just GtkAccelMap. */
    gtk_widget_event (GTK_WIDGET (window), event);
    gdk_event_free (event);
    g_free (entries);
}

static void
assert_menu_proxies (Fixture *fixture, GtkAction *action)
{
    const char *locations[] = {
        "/MenuBar/Edit/File Items Placeholder/ArchiveActions",
        "/selection/File Actions/ArchiveActions"
    };
    GtkUIManager *manager = nemo_window_get_ui_manager (fixture->window);
    gtk_ui_manager_ensure_update (manager);
    for (guint i = 0; i < G_N_ELEMENTS (locations); i++) {
        g_autofree char *path = g_strconcat (locations[i], "/", gtk_action_get_name (action), NULL);
        GtkWidget *item = gtk_ui_manager_get_widget (manager, path);
        g_assert_nonnull (item);
        g_assert_true (GTK_IS_MENU_ITEM (item));
        g_assert_true (gtk_activatable_get_related_action (GTK_ACTIVATABLE (item)) == action);
    }
}

static void
assert_and_cancel (Fixture *fixture, GFile *destination, gboolean move)
{
    WAIT_FOR (recorded.calls == 1);
    g_assert_cmpint (recorded.move, ==, move);
    g_assert_true (g_file_equal (recorded.directory, destination));
    g_assert_cmpuint (g_list_length (recorded.sources), ==, 1);
    g_assert_true (g_file_equal (recorded.sources->data, fixture->item));
    GtkWidget *chooser = find_chooser ();
    g_assert_nonnull (chooser);
    WAIT_FOR (chooser_at (chooser, destination));
    g_assert_cmpint (gtk_file_chooser_get_action (GTK_FILE_CHOOSER (chooser)), ==,
                     GTK_FILE_CHOOSER_ACTION_SAVE);
    g_assert_true (gtk_window_get_transient_for (GTK_WINDOW (chooser)) == GTK_WINDOW (fixture->window));
    g_autofree char *name = gtk_file_chooser_get_current_name (GTK_FILE_CHOOSER (chooser));
    g_assert_cmpstr (name, ==, "needle.txt.7z");
    gtk_dialog_response (GTK_DIALOG (chooser), GTK_RESPONSE_CANCEL);
    WAIT_FOR (find_chooser () == NULL);
    g_assert_true (g_file_query_exists (fixture->item, NULL));
    g_autoptr (GFile) archive = g_file_get_child (destination, "needle.txt.7z");
    g_assert_false (g_file_query_exists (archive, NULL));
    g_list_free_full (recorded.sources, g_object_unref);
    g_clear_object (&recorded.directory);
    memset (&recorded, 0, sizeof recorded);
}

typedef struct {
    const char *name;
    const char *view;
    gboolean split;
    gboolean move;
    gboolean rename;
    gboolean right_active;
} ActionCase;

static void
test_action (const ActionCase *test)
{
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_DEFAULT_FOLDER_VIEWER, test->view);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_IGNORE_VIEW_METADATA, TRUE);
    Fixture fixture = fixture_new ();
    nemo_window_slot_open_location (fixture.slot, fixture.containing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    if (test->split) {
        nemo_window_split_view_on (fixture.window);
        nemo_window_set_active_slot (fixture.window, fixture.slot);
        NemoWindowSlot *other = nemo_window_get_extra_slot (fixture.window);
        g_assert_nonnull (other);
        nemo_window_slot_open_location (other, fixture.origin, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
        WAIT_FOR (slot_at (other, fixture.origin));
        if (test->right_active) {
            nemo_window_slot_open_location (fixture.slot, fixture.origin, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
            WAIT_FOR (slot_at (fixture.slot, fixture.origin));
            nemo_window_slot_open_location (other, fixture.containing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
            WAIT_FOR (slot_at (other, fixture.containing));
            fixture.slot = other;
        }
        nemo_window_set_active_slot (fixture.window, fixture.slot);
    }
    NemoView *view = fixture.slot->content_view;
    nemo_view_grab_focus (view);
    g_assert_true (nemo_window_get_active_slot (fixture.window) == fixture.slot);
    NemoFile *parent = parent_entry (view);
    NemoFile *file = nemo_file_get (fixture.item);
    GtkAction *copy = find_action (fixture.window, NEMO_ACTION_ARCHIVE_CREATE);
    GtkAction *move = find_action (fixture.window, NEMO_ACTION_ARCHIVE_MOVE);
    assert_menu_proxies (&fixture, copy);
    assert_menu_proxies (&fixture, move);

    GList parent_only = { .data = parent };
    nemo_view_set_selection (view, &parent_only);
    nemo_view_update_menus (view);
    WAIT_FOR (!gtk_action_get_sensitive (copy) && !gtk_action_get_sensitive (move));
    gtk_action_activate (copy);
    gtk_action_activate (move);
    g_assert_cmpuint (recorded.calls, ==, 0);
    g_assert_null (find_chooser ());

    GList real_item = { .data = file };
    GList mixed = { .data = parent, .next = &real_item };
    nemo_view_set_selection (view, &mixed);
    nemo_view_update_menus (view);
    WAIT_FOR (gtk_action_get_sensitive (copy) && gtk_action_get_sensitive (move));
    if (test->rename) {
        gtk_action_activate (find_action (fixture.window, NEMO_ACTION_RENAME));
        WAIT_FOR (nemo_view_get_is_renaming (view));
        GtkWidget *entry = gtk_window_get_focus (GTK_WINDOW (fixture.window));
        g_assert_true (GTK_IS_EDITABLE (entry));
        g_autofree char *before = gtk_editable_get_chars (GTK_EDITABLE (entry), 0, -1);
        send_key_event (fixture.window, "<Alt>F5");
        send_key_event (fixture.window, "<Alt><Shift>F5");
        g_assert_true (nemo_view_get_is_renaming (view));
        g_assert_cmpuint (recorded.calls, ==, 0);
        g_assert_null (find_chooser ());
        g_autofree char *after = gtk_editable_get_chars (GTK_EDITABLE (entry), 0, -1);
        g_assert_cmpstr (after, ==, before);
        send_key_event (fixture.window, "Escape");
        WAIT_FOR (!nemo_view_get_is_renaming (view));
    }
    if (test->right_active) {
        g_autofree char *path = g_strconcat ("/selection/File Actions/ArchiveActions/",
            test->move ? NEMO_ACTION_ARCHIVE_MOVE : NEMO_ACTION_ARCHIVE_CREATE, NULL);
        GtkWidget *item = gtk_ui_manager_get_widget (nemo_window_get_ui_manager (fixture.window), path);
        g_assert_nonnull (item);
        gtk_menu_item_activate (GTK_MENU_ITEM (item));
    } else {
        g_assert_true (key_press (fixture.window, test->move ? "<Alt><Shift>F5" : "<Alt>F5"));
    }
    assert_and_cancel (&fixture, test->split ? fixture.origin : fixture.containing, test->move);
    nemo_file_unref (file);
    nemo_file_unref (parent);
    fixture_clear (&fixture);
}

static void
run_case (gconstpointer data)
{
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 25000000, 0);
        g_test_trap_assert_passed ();
        return;
    }
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    application = nemo_main_application_get_singleton ();
    GError *error = NULL;
    g_assert_true (g_application_register (G_APPLICATION (application), NULL, &error));
    g_assert_no_error (error);
    test_action (data);
    g_object_unref (application);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    g_assert_cmpstr (g_getenv ("GSETTINGS_BACKEND"), ==, "memory");
    g_assert_cmpstr (g_getenv ("GDK_BACKEND"), ==, "x11");
    g_assert_null (g_getenv ("WAYLAND_DISPLAY"));
    gtk_test_init (&argc, &argv, NULL);
    static const ActionCase cases[] = {
        { "/archive-actions/list/current-copy", "list-view", FALSE, FALSE, FALSE },
        { "/archive-actions/list/current-move", "list-view", FALSE, TRUE, FALSE },
        { "/archive-actions/list/other-copy", "list-view", TRUE, FALSE, FALSE },
        { "/archive-actions/list/other-move", "list-view", TRUE, TRUE, FALSE },
        { "/archive-actions/list/rename-guard", "list-view", FALSE, FALSE, TRUE },
        { "/archive-actions/list/right-active-copy-menu", "list-view", TRUE, FALSE, FALSE, TRUE },
        { "/archive-actions/list/right-active-move-menu", "list-view", TRUE, TRUE, FALSE, TRUE },
        { "/archive-actions/icon/current-copy", "icon-view", FALSE, FALSE, FALSE },
        { "/archive-actions/icon/current-move", "icon-view", FALSE, TRUE, FALSE },
        { "/archive-actions/icon/other-copy", "icon-view", TRUE, FALSE, FALSE },
        { "/archive-actions/icon/other-move", "icon-view", TRUE, TRUE, FALSE },
        { "/archive-actions/icon/rename-guard", "icon-view", FALSE, FALSE, TRUE },
        { "/archive-actions/icon/right-active-copy-menu", "icon-view", TRUE, FALSE, FALSE, TRUE },
        { "/archive-actions/icon/right-active-move-menu", "icon-view", TRUE, TRUE, FALSE, TRUE },
        { "/archive-actions/compact/current-copy", "compact-view", FALSE, FALSE, FALSE },
        { "/archive-actions/compact/current-move", "compact-view", FALSE, TRUE, FALSE },
        { "/archive-actions/compact/other-copy", "compact-view", TRUE, FALSE, FALSE },
        { "/archive-actions/compact/other-move", "compact-view", TRUE, TRUE, FALSE },
        { "/archive-actions/compact/rename-guard", "compact-view", FALSE, FALSE, TRUE },
        { "/archive-actions/compact/right-active-copy-menu", "compact-view", TRUE, FALSE, FALSE, TRUE },
        { "/archive-actions/compact/right-active-move-menu", "compact-view", TRUE, TRUE, FALSE, TRUE }
    };
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
        g_test_add_data_func (cases[i].name, &cases[i], run_case);
    return g_test_run ();
}
