#include <config.h>
#include "window-test-fixture.h"
#include "../src/nemo-actions.h"
#include "../src/nemo-icon-view.h"
#include "../src/nemo-list-model.h"
#include <libnemo-private/nemo-file-undo-manager.h>
#include <libnemo-private/nemo-progress-info-manager.h>
#include <libnemo-private/nemo-transfer-safety.h>
#include <sys/stat.h>

static gint move_calls, second_waiting, resume_move;
static NemoProgressInfo *progress;

gboolean __real_nemo_transfer_native_move (NemoTransferGuard *, GFile *, GFile *,
                                          gboolean, gboolean *, NemoTransferUndo **,
                                          GCancellable *, GError **);

gboolean
__wrap_nemo_transfer_native_move (NemoTransferGuard *guard, GFile *source, GFile *destination,
                                  gboolean overwrite, gboolean *published, NemoTransferUndo **undo,
                                  GCancellable *cancel, GError **error)
{
    if (g_atomic_int_add (&move_calls, 1) == 1) {
        g_atomic_int_set (&second_waiting, TRUE);
        gint64 deadline = g_get_monotonic_time () + 10000000;
        while (!g_atomic_int_get (&resume_move) && g_get_monotonic_time () < deadline)
            g_usleep (1000);
        g_assert_true (g_atomic_int_get (&resume_move));
    }
    return __real_nemo_transfer_native_move (guard, source, destination, overwrite,
                                              published, undo, cancel, error);
}

int __wrap_nemo_inhibit_power_manager (const char *message) { return -1; }

static void
progress_created (NemoProgressInfoManager *manager, NemoProgressInfo *info, gpointer unused)
{
    g_assert_null (progress);
    progress = g_object_ref (info);
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

typedef struct { GFile *location; gboolean found; } Membership;

static void
find_icon (NemoIconData *data, gpointer user_data)
{
    Membership *member = user_data;
    g_autoptr (GFile) location = nemo_file_get_location (NEMO_FILE (data));
    member->found |= g_file_equal (member->location, location);
}

static gboolean
view_contains (NemoView *view, GFile *location)
{
    if (NEMO_IS_ICON_VIEW (view)) {
        Membership member = { .location = location };
        nemo_icon_container_for_each (nemo_icon_view_get_icon_container (NEMO_ICON_VIEW (view)),
                                      find_icon, &member);
        return member.found;
    }
    GtkTreeView *tree = NULL;
    find_tree (GTK_WIDGET (view), &tree);
    g_assert_nonnull (tree);
    GtkTreeModel *model = gtk_tree_view_get_model (tree);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first (model, &iter);
    while (valid) {
        NemoFile *file = NULL;
        gtk_tree_model_get (model, &iter, NEMO_LIST_MODEL_FILE_COLUMN, &file, -1);
        g_autoptr (GFile) candidate = nemo_file_get_location (file);
        nemo_file_unref (file);
        if (g_file_equal (location, candidate))
            return TRUE;
        valid = gtk_tree_model_iter_next (model, &iter);
    }
    return FALSE;
}

static gboolean
confirm_move (gpointer unused)
{
    GList *windows = gtk_window_list_toplevels ();
    for (GList *l = windows; l; l = l->next)
        if (GTK_IS_MESSAGE_DIALOG (l->data) && gtk_widget_get_visible (l->data)) {
            GtkMessageType type;
            g_object_get (l->data, "message-type", &type, NULL);
            if (type != GTK_MESSAGE_QUESTION) {
                char *text = NULL, *secondary = NULL;
                g_object_get (l->data, "text", &text, "secondary-text", &secondary, NULL);
                g_error ("Unexpected move dialog: %s; %s", text, secondary);
            }
            gtk_dialog_response (l->data, GTK_RESPONSE_OK);
        }
    g_list_free (windows);
    return G_SOURCE_CONTINUE;
}

static void
remove_fixture_tree (GFile *file)
{
    if (g_file_query_file_type (file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_DIRECTORY) {
        g_autoptr (GFileEnumerator) children = g_file_enumerate_children (
            file, G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
        g_assert_nonnull (children);
        GFileInfo *info;
        while ((info = g_file_enumerator_next_file (children, NULL, NULL))) {
            g_autoptr (GFile) child = g_file_get_child (file, g_file_info_get_name (info));
            remove_fixture_tree (child);
            g_object_unref (info);
        }
        g_assert_true (g_file_enumerator_close (children, NULL, NULL));
    }
    g_assert_true (g_file_delete (file, NULL, NULL));
}

static void
test_move_feedback (gconstpointer data)
{
    const char *mode = data;
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 20000000, 0);
        g_test_trap_assert_passed ();
        return;
    }
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    application = nemo_main_application_get_singleton ();
    g_autoptr (GError) error = NULL;
    g_assert_true (g_application_register (G_APPLICATION (application), NULL, &error));
    g_assert_no_error (error);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_IGNORE_VIEW_METADATA, TRUE);
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_DEFAULT_FOLDER_VIEWER, mode);
    Fixture fixture = fixture_new ();
    /* Native transfer qualification must use the bind-mounted build filesystem,
     * not the isolated desktop's /tmp (which may be tmpfs or overlayfs). */
    g_autofree char *root = g_dir_make_tmp ("move-feedback-XXXXXX", &error);
    g_assert_no_error (error);
    g_autoptr (GFile) origin = g_file_new_for_path (root);
    g_autoptr (GFile) containing = g_file_get_child (origin, "B");
    g_assert_true (g_file_make_directory (containing, NULL, &error));
    g_assert_no_error (error);
    nemo_window_slot_open_location (fixture.slot, origin, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, origin));
    nemo_window_split_view_on (fixture.window);
    NemoWindowSlot *target_slot = nemo_window_get_extra_slot (fixture.window);
    nemo_window_slot_open_location (target_slot, containing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (target_slot, containing));
    nemo_window_set_active_slot (fixture.window, fixture.slot);
    WAIT_FOR (slot_at (fixture.slot, origin));
    NemoView *source_view = fixture.slot->content_view;
    NemoView *target_view = target_slot->content_view;
    GList *uris = NULL, *selection = NULL;
    GFile *sources[3], *destinations[3];
    struct stat identities[3];
    for (guint i = 0; i < 3; i++) {
        g_autofree char *name = g_strdup_printf (
            "(%u) - \320\237\320\276\320\266\320\270\321\200\320\260\321\202\320\265\320\273\321\214 \320\267\320\262\321\221\320\267\320\264..mp4", i);
        sources[i] = g_file_get_child (origin, name);
        destinations[i] = g_file_get_child (containing, name);
        g_autofree char *path = g_file_get_path (sources[i]);
        g_assert_true (g_file_set_contents (path, "Owned move feedback fixture", -1, NULL));
        g_assert_cmpint (lstat (path, &identities[i]), ==, 0);
        WAIT_FOR (view_contains (source_view, sources[i]));
        uris = g_list_append (uris, g_file_get_uri (sources[i]));
        selection = g_list_append (selection, nemo_file_get (sources[i]));
    }
    nemo_view_set_selection (source_view, selection);
    nemo_file_list_free (selection);
    NemoProgressInfoManager *manager = nemo_progress_info_manager_new ();
    g_signal_connect (manager, "new-progress-info", G_CALLBACK (progress_created), NULL);
    guint response = g_timeout_add (10, confirm_move, NULL);
    if (g_str_equal (mode, "list-view")) {
        GtkAction *move = NULL;
        for (GList *l = gtk_ui_manager_get_action_groups (nemo_window_get_ui_manager (fixture.window));
             l && !move; l = l->next)
            move = gtk_action_group_get_action (l->data, NEMO_ACTION_MOVE_TO_NEXT_PANE);
        g_assert_nonnull (move);
        WAIT_FOR (gtk_action_is_sensitive (move));
        gtk_accel_map_change_entry ("<Actions>/ShellActions/SplitViewNextPane", 0, 0, TRUE);
        g_assert_true (gtk_accel_map_change_entry (
            "<Actions>/DirViewActions/Move to next pane", GDK_KEY_F6, 0, TRUE));
        g_assert_true (key_press (fixture.window, "F6"));
    } else {
        g_autofree char *target = g_file_get_uri (containing);
        nemo_view_drop_items (source_view, uris, NULL, target, GDK_ACTION_MOVE, 0, 0);
    }
    WAIT_FOR (g_atomic_int_get (&second_waiting));
    g_assert_nonnull (progress);
    g_assert_false (nemo_progress_info_get_is_finished (progress));
    g_autofree char *details = nemo_progress_info_get_details (progress);
    g_autofree char *second_name = g_file_get_basename (sources[1]);
    g_assert_nonnull (strstr (details, second_name));
    WAIT_FOR (!view_contains (source_view, sources[0]));
    WAIT_FOR (view_contains (target_view, destinations[0]));
    g_assert_true (view_contains (source_view, sources[1]));
    g_assert_true (view_contains (source_view, sources[2]));
    g_atomic_int_set (&resume_move, TRUE);
    WAIT_FOR (nemo_progress_info_get_is_finished (progress));
    g_source_remove (response);
    NemoProgressResult result;
    g_assert_true (nemo_progress_info_get_result (progress, &result));
    g_assert_cmpint (result.outcome, ==, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_cmpuint (result.atomic_moves, ==, 3);
    for (guint i = 0; i < 3; i++) {
        WAIT_FOR (!view_contains (source_view, sources[i]));
        WAIT_FOR (view_contains (target_view, destinations[i]));
        g_assert_false (g_file_query_exists (sources[i], NULL));
        g_autofree char *path = g_file_get_path (destinations[i]);
        struct stat installed;
        g_assert_cmpint (lstat (path, &installed), ==, 0);
        g_assert_cmpuint (installed.st_ino, ==, identities[i].st_ino);
        g_assert_cmpuint (installed.st_dev, ==, identities[i].st_dev);
        g_object_unref (sources[i]);
        g_object_unref (destinations[i]);
    }
    GList *remaining = nemo_view_get_selection_for_file_transfer (source_view);
    for (GList *l = remaining; l; l = l->next) {
        g_autoptr (GFile) location = nemo_file_get_location (l->data);
        g_autoptr (GFile) parent = g_file_get_parent (location);
        g_assert_true (g_file_equal (parent, origin));
        g_assert_true (g_file_query_exists (location, NULL));
    }
    nemo_file_list_free (remaining);
    nemo_file_undo_manager_set_action (NULL);
    fixture_clear (&fixture);
    remove_fixture_tree (origin);
    g_list_free_full (uris, g_free);
    g_clear_object (&progress);
    g_object_unref (manager);
    g_object_unref (application);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    gtk_test_init (&argc, &argv, NULL);
    g_test_add_data_func ("/move-feedback/keyboard-list", "list-view", test_move_feedback);
    g_test_add_data_func ("/move-feedback/drag-icons", "icon-view", test_move_feedback);
    g_test_add_data_func ("/move-feedback/drag-compact", "compact-view", test_move_feedback);
    return g_test_run ();
}
