#include <config.h>
#include "window-test-fixture.h"
#include "../src/nemo-keybindings.h"
#include "../src/nemo-list-model.h"
#include "../src/nemo-list-view.h"
#include <libnemo-private/nemo-file-private.h>
#include <libnemo-private/nemo-progress-info.h>
#include <unistd.h>

static void
send_key (NemoWindow *window, guint key, GdkModifierType modifiers)
{
    GdkEvent *event = gdk_event_new (GDK_KEY_PRESS);
    event->key.window = g_object_ref (gtk_widget_get_window (GTK_WIDGET (window)));
    event->key.keyval = key;
    event->key.state = modifiers;
    GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (window));
    gdk_event_set_device (event, gdk_seat_get_keyboard (gdk_display_get_default_seat (display)));
    gtk_widget_event (GTK_WIDGET (window), event);
    gdk_event_free (event);
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
assert_column (NemoView *view, NemoFile *file, const char *expected)
{
    g_autofree char *value = nemo_file_get_string_attribute (file, "size");
    g_assert_cmpstr (value, ==, expected);
    g_clear_pointer (&value, g_free);
    value = nemo_file_get_string_attribute (file, "size_detail");
    g_assert_cmpstr (value, ==, expected);
    if (!NEMO_IS_LIST_VIEW (view))
        return;
    GtkTreeView *tree = NULL;
    find_tree (GTK_WIDGET (view), &tree);
    g_assert_nonnull (tree);
    NemoListModel *model = NEMO_LIST_MODEL (gtk_tree_view_get_model (tree));
    GtkTreeIter iter;
    g_assert_true (nemo_list_model_get_tree_iter_from_file (model, file, NULL, &iter));
    gint column = nemo_list_model_get_column_number (model, "size");
    g_assert_cmpint (column, >=, 0);
    g_clear_pointer (&value, g_free);
    gtk_tree_model_get (GTK_TREE_MODEL (model), &iter, column, &value, -1);
    g_assert_cmpstr (value, ==, expected);
}

static void
test_navigation_cancel (gconstpointer data)
{
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
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_DEFAULT_FOLDER_VIEWER, data);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_IGNORE_VIEW_METADATA, TRUE);
    Fixture fixture = fixture_new ();
    g_autoptr (GFile) second_location = g_file_get_child (fixture.origin, "second");
    g_assert_true (g_file_make_directory (second_location, NULL, &error));
    g_assert_no_error (error);
    NemoFile *folder = nemo_file_get (fixture.containing);
    NemoFile *second = nemo_file_get (second_location);
    WAIT_FOR (nemo_file_is_directory (folder) && nemo_file_is_directory (second));
    GList second_selection = { .data = second };
    GList selection = { .data = folder, .next = &second_selection };
    NemoView *view = fixture.slot->content_view;
    WAIT_FOR ((nemo_view_set_selection (view, &selection),
               nemo_view_get_selection_count (view) == 2));
    nemo_view_grab_focus (view);
    send_key (fixture.window, GDK_KEY_space, 0);
    g_assert_true (folder->details->calculated_size_pending);
    g_assert_true (second->details->calculated_size_pending);
    /* Leave before the initial filesystem-info callback has completed. */
    nemo_window_slot_open_location (fixture.slot, fixture.containing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    g_assert_false (folder->details->calculated_size_pending);
    g_assert_false (folder->details->show_calculated_size);
    g_assert_false (second->details->calculated_size_pending);
    g_assert_false (second->details->show_calculated_size);
    nemo_window_slot_open_location (fixture.slot, fixture.origin, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    view = fixture.slot->content_view;
    nemo_view_set_selection (view, &selection);
    g_assert_cmpuint (nemo_view_get_selection_count (view), ==, 2);
    nemo_view_grab_focus (view);
    send_key (fixture.window, GDK_KEY_space, 0);
    WAIT_FOR (!folder->details->calculated_size_pending && !second->details->calculated_size_pending);
    goffset size;
    g_assert_cmpint (nemo_file_get_deep_counts (folder, NULL, NULL, NULL, NULL, &size, TRUE),
                     ==, NEMO_REQUEST_DONE);
    g_assert_cmpint (size, ==, strlen ("temporary search fixture"));
    nemo_file_unref (folder);
    nemo_file_unref (second);
    g_assert_true (g_file_delete (second_location, NULL, NULL));
    fixture_clear (&fixture);
    g_object_unref (application);
}

typedef struct {
    guint ticks;
    gint64 last_tick;
    gint64 longest_gap;
} Heartbeat;

static gboolean
scan_heartbeat (gpointer data)
{
    Heartbeat *heartbeat = data;
    gint64 now = g_get_monotonic_time ();
    heartbeat->longest_gap = MAX (heartbeat->longest_gap, now - heartbeat->last_tick);
    heartbeat->last_tick = now;
    heartbeat->ticks++;
    return G_SOURCE_CONTINUE;
}

static void
test_large_scan (void)
{
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 90000000, 0);
        g_test_trap_assert_passed ();
        return;
    }
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    application = nemo_main_application_get_singleton ();
    GError *error = NULL;
    g_assert_true (g_application_register (G_APPLICATION (application), NULL, &error));
    g_assert_no_error (error);
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_DEFAULT_FOLDER_VIEWER, "list-view");
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_IGNORE_VIEW_METADATA, TRUE);
    Fixture fixture = fixture_new ();
    g_autofree char *folder_path = g_file_get_path (fixture.containing);
    const guint entries = 200000;
    for (guint i = 0; i < entries; i++) {
        g_autofree char *path = g_strdup_printf ("%s/file-%u", folder_path, i);
        g_assert_true (g_file_set_contents (path, "x", 1, NULL));
    }
    g_autofree char *hardlink = g_build_filename (folder_path, "hardlink", NULL);
    g_autofree char *first = g_build_filename (folder_path, "file-0", NULL);
    g_assert_cmpint (link (first, hardlink), ==, 0);
    NemoFile *folder = nemo_file_get (fixture.containing);
    WAIT_FOR (nemo_file_is_directory (folder));
    NemoView *view = fixture.slot->content_view;
    GList selection = { .data = folder };
    nemo_view_set_selection (view, &selection);
    nemo_view_grab_focus (view);
    Heartbeat heartbeat = { .last_tick = g_get_monotonic_time () };
    guint timer = g_timeout_add (5, scan_heartbeat, &heartbeat);
    send_key (fixture.window, GDK_KEY_space, 0);
    WAIT_FOR (folder->details->deep_file_count > 0);
    g_assert_true (folder->details->calculated_size_pending);
    /* Cancel an active enumeration, then reopen the original directory. */
    g_autoptr (GFile) parent = g_file_get_parent (fixture.origin);
    nemo_window_slot_open_location (fixture.slot, parent, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, parent));
    g_assert_false (folder->details->calculated_size_pending);
    nemo_window_slot_open_location (fixture.slot, fixture.origin, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    view = fixture.slot->content_view;
    nemo_view_set_selection (view, &selection);
    g_assert_cmpuint (nemo_view_get_selection_count (view), ==, 1);
    nemo_view_grab_focus (view);
    heartbeat = (Heartbeat) { .last_tick = g_get_monotonic_time () };
    send_key (fixture.window, GDK_KEY_space, 0);
    gint64 deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
    while (folder->details->calculated_size_pending && g_get_monotonic_time () < deadline)
        iterate ();
    g_assert_false (folder->details->calculated_size_pending);
    g_source_remove (timer);
    g_test_message ("200000-file scan: %u UI heartbeats, largest gap %.1f ms",
                    heartbeat.ticks, heartbeat.longest_gap / 1000.0);
    g_assert_cmpuint (heartbeat.ticks, >, 5);
    g_assert_cmpint (heartbeat.longest_gap, <, 250 * G_TIME_SPAN_MILLISECOND);
    goffset size;
    g_assert_cmpint (nemo_file_get_deep_counts (folder, NULL, NULL, NULL, NULL, &size, TRUE),
                     ==, NEMO_REQUEST_DONE);
    g_assert_cmpint (size, ==, entries + strlen ("temporary search fixture"));
    for (guint i = 0; i < entries; i++) {
        g_autofree char *path = g_strdup_printf ("%s/file-%u", folder_path, i);
        g_assert_cmpint (g_remove (path), ==, 0);
    }
    g_assert_cmpint (g_remove (hardlink), ==, 0);
    nemo_file_unref (folder);
    fixture_clear (&fixture);
    g_object_unref (application);
}

static void
test_sizes (gconstpointer data)
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
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_DEFAULT_FOLDER_VIEWER, data);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_IGNORE_VIEW_METADATA, TRUE);
    g_settings_set_enum (nemo_preferences, NEMO_PREFERENCES_SHOW_DIRECTORY_ITEM_COUNTS,
                         NEMO_SPEED_TRADEOFF_NEVER);
    Fixture fixture = fixture_new ();
    g_autoptr (GFile) empty_location = g_file_get_child (fixture.origin, "empty");
    g_assert_true (g_file_make_directory (empty_location, NULL, &error));
    g_assert_no_error (error);
    NemoFile *folder = nemo_file_get (fixture.containing);
    NemoFile *empty = nemo_file_get (empty_location);
    NemoView *view = fixture.slot->content_view;
    WAIT_FOR (nemo_file_is_directory (folder) && nemo_file_is_directory (empty));
    GList second = { .data = empty };
    GList selection = { .data = folder, .next = &second };
    WAIT_FOR ((nemo_view_set_selection (view, &selection),
               nemo_view_get_selection_count (view) == 2));
    nemo_view_grab_focus (view);
    send_key (fixture.window, GDK_KEY_space, 0);
    g_assert_true (folder->details->show_calculated_size);
    g_assert_true (empty->details->show_calculated_size);
    WAIT_FOR (!folder->details->calculated_size_pending && !empty->details->calculated_size_pending);
    goffset size;
    g_assert_cmpint (nemo_file_get_deep_counts (folder, NULL, NULL, NULL, NULL, &size, TRUE),
                     ==, NEMO_REQUEST_DONE);
    g_assert_cmpint (size, ==, strlen ("temporary search fixture"));
    g_autofree char *formatted = g_format_size_full (
        size, nemo_global_preferences_get_size_prefix_preference ());
    assert_column (view, folder, formatted);
    g_autofree char *zero = g_format_size_full (
        0, nemo_global_preferences_get_size_prefix_preference ());
    assert_column (view, empty, zero);
    g_assert_cmpint (nemo_file_compare_for_sort (empty, folder, NEMO_FILE_SORT_BY_SIZE,
                     FALSE, FALSE, FALSE, NULL), <, 0);

    /* Rebinding is live; Space no longer requests sizes after the change. */
    g_autofree char *item_path = g_file_get_path (fixture.item);
    g_assert_true (g_file_set_contents (item_path, "updated", -1, NULL));
    nemo_keybindings_set_for_action ("calculate-directory-sizes", "<Alt>F8");
    folder->details->show_calculated_size = FALSE;
    empty->details->show_calculated_size = FALSE;
    send_key (fixture.window, GDK_KEY_F8, GDK_MOD1_MASK);
    g_assert_true (folder->details->show_calculated_size);
    g_assert_true (empty->details->show_calculated_size);
    WAIT_FOR (!folder->details->calculated_size_pending && !empty->details->calculated_size_pending);
    g_assert_cmpint (nemo_file_get_deep_counts (folder, NULL, NULL, NULL, NULL, &size, TRUE),
                     ==, NEMO_REQUEST_DONE);
    g_assert_cmpint (size, ==, 7);
    g_clear_pointer (&formatted, g_free);
    formatted = g_format_size_full (size, nemo_global_preferences_get_size_prefix_preference ());
    assert_column (view, folder, formatted);

    if (geteuid () != 0) {
        g_autofree char *empty_path = g_file_get_path (empty_location);
        g_autofree char *blocked = g_build_filename (empty_path, "blocked", NULL);
        g_autofree char *hidden = g_build_filename (blocked, ".hidden", NULL);
        g_assert_cmpint (g_mkdir (blocked, 0700), ==, 0);
        g_assert_true (g_file_set_contents (hidden, "inaccessible", -1, NULL));
        GStatBuf statbuf;
        g_assert_cmpint (g_stat (blocked, &statbuf), ==, 0);
        g_assert_cmpint (g_chmod (blocked, 0000), ==, 0);
        nemo_file_calculate_directory_size (empty);
        WAIT_FOR (!empty->details->calculated_size_pending);
        guint unreadable;
        g_assert_cmpint (nemo_file_get_deep_counts (empty, NULL, NULL, &unreadable,
                         NULL, &size, TRUE), ==, NEMO_REQUEST_DONE);
        g_assert_cmpuint (unreadable, ==, 1);
        g_assert_cmpint (size, ==, statbuf.st_size);
        g_autofree char *partial = g_format_size_full (
            size, nemo_global_preferences_get_size_prefix_preference ());
        g_autofree char *expected = g_strdup_printf ("%s (incomplete)", partial);
        assert_column (view, empty, expected);
        g_assert_cmpint (g_chmod (blocked, 0700), ==, 0);
        g_assert_cmpint (g_remove (hidden), ==, 0);
        g_assert_cmpint (g_rmdir (blocked), ==, 0);
    }
    nemo_keybindings_set_for_action ("calculate-directory-sizes", "");
    folder->details->show_calculated_size = FALSE;
    empty->details->show_calculated_size = FALSE;
    send_key (fixture.window, GDK_KEY_F8, GDK_MOD1_MASK);
    g_assert_false (folder->details->show_calculated_size);
    g_assert_false (empty->details->show_calculated_size);

    send_key (fixture.window, GDK_KEY_Escape, 0);
    nemo_view_grab_focus (view);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_DOCK_FILE_TRANSFERS, TRUE);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_SHOW_SUCCESSFUL_TRANSFERS, TRUE);
    NemoProgressInfo *info = nemo_progress_info_new ();
    NemoProgressResult result = {
        .operation = NEMO_PROGRESS_OPERATION_COPY,
        .outcome = NEMO_PROGRESS_OUTCOME_SUCCESS,
        .completed_items = 1
    };
    nemo_progress_info_set_parent_window (info, GTK_WINDOW (fixture.window));
    nemo_progress_info_set_result (info, &result);
    nemo_progress_info_queue (info);
    nemo_progress_info_start (info);
    nemo_progress_info_finish (info);
    GtkWidget *area = nemo_window_get_transfer_area (fixture.window);
    WAIT_FOR (gtk_widget_get_mapped (area));
    nemo_view_grab_focus (view);
    send_key (fixture.window, GDK_KEY_c, GDK_MOD1_MASK);
    WAIT_FOR (!gtk_widget_get_visible (area));
    g_assert_true (gtk_widget_get_visible (GTK_WIDGET (fixture.window)));
    GCancellable *cancel = nemo_progress_info_get_cancellable (info);
    g_assert_false (g_cancellable_is_cancelled (cancel));
    g_object_unref (cancel);
    g_object_unref (info);

    g_settings_reset (nemo_keybinding_settings, "calculate-directory-sizes");
    nemo_window_show_location_entry (fixture.window);
    GtkWidget *entry = gtk_window_get_focus (GTK_WINDOW (fixture.window));
    g_assert_true (GTK_IS_ENTRY (entry));
    gtk_editable_select_region (GTK_EDITABLE (entry), 0, -1);
    send_key (fixture.window, GDK_KEY_space, 0);
    g_assert_cmpstr (gtk_entry_get_text (GTK_ENTRY (entry)), ==, " ");
    g_assert_false (folder->details->show_calculated_size);
    g_assert_false (empty->details->show_calculated_size);

    nemo_file_unref (folder);
    nemo_file_unref (empty);
    g_assert_true (g_file_delete (empty_location, NULL, NULL));
    fixture_clear (&fixture);
    g_object_unref (application);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    g_assert_cmpstr (g_getenv ("GSETTINGS_BACKEND"), ==, "memory");
    gtk_test_init (&argc, &argv, NULL);
    nemo_keybindings_init ();
    const NemoKeybindingEntry *entry = NULL;
    for (gint i = 0; i < nemo_keybinding_entries_count; i++)
        if (g_str_equal (nemo_keybinding_entries[i].settings_key, "calculate-directory-sizes"))
            entry = &nemo_keybinding_entries[i];
    g_assert_nonnull (entry);
    g_assert_null (entry->accel_path);
    g_assert_cmpstr (entry->default_accel, ==, "space");
    g_assert_cmpstr (entry->category, ==, "File Operations");
    g_autoptr (GVariant) value = g_settings_get_default_value (
        nemo_keybinding_settings, "calculate-directory-sizes");
    g_assert_cmpstr (g_variant_get_string (value, NULL), ==, "space");
    g_test_add_data_func ("/directory-sizes/list", "list-view", test_sizes);
    g_test_add_data_func ("/directory-sizes/icon", "icon-view", test_sizes);
    g_test_add_data_func ("/directory-sizes/compact", "compact-view", test_sizes);
    g_test_add_data_func ("/directory-sizes/cancel-list", "list-view", test_navigation_cancel);
    g_test_add_data_func ("/directory-sizes/cancel-icon", "icon-view", test_navigation_cancel);
    g_test_add_data_func ("/directory-sizes/cancel-compact", "compact-view", test_navigation_cancel);
    g_test_add_func ("/directory-sizes/large-tree-responsive", test_large_scan);
    return g_test_run ();
}
