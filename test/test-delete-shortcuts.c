#include <config.h>
#include "window-test-fixture.h"
#include "../src/nemo-keybindings.h"
#include "../src/nemo-actions.h"
#include <libnemo-private/nemo-file-operations.h>
#include <libnemo-private/nemo-progress-info-manager.h>

static guint delete_calls, trash_calls;
static gboolean completed, cancelled;
static gboolean expect_confirmation, confirmed;
static GFile *expected_file;
static NemoDeleteCallback trash_callback;
static gpointer trash_callback_data;

void __real_nemo_file_operations_delete (GList *, GtkWindow *, NemoDeleteCallback, gpointer);
void __real_nemo_file_operations_trash_or_delete (GList *, GtkWindow *, NemoDeleteCallback, gpointer);

int __wrap_nemo_inhibit_power_manager (const char *message) { return -1; }

static void
delete_done (GHashTable *debuting, gboolean user_cancel, gpointer unused)
{
    completed = TRUE;
    cancelled = user_cancel;
}

void
__wrap_nemo_file_operations_delete (GList *files, GtkWindow *parent,
                                    NemoDeleteCallback callback, gpointer data)
{
    g_assert_null (callback);
    g_assert_cmpuint (g_list_length (files), ==, 1);
    g_assert_true (g_file_equal (files->data, expected_file));
    delete_calls++;
    __real_nemo_file_operations_delete (files, parent, delete_done, NULL);
}

static void
trash_done (GHashTable *debuting, gboolean user_cancel, gpointer unused)
{
    trash_callback (debuting, user_cancel, trash_callback_data);
    delete_done (debuting, user_cancel, NULL);
}

void
__wrap_nemo_file_operations_trash_or_delete (GList *files, GtkWindow *parent,
                                            NemoDeleteCallback callback, gpointer data)
{
    g_assert_nonnull (callback);
    g_assert_cmpuint (g_list_length (files), ==, 1);
    g_assert_true (g_file_equal (files->data, expected_file));
    trash_callback = callback;
    trash_callback_data = data;
    trash_calls++;
    __real_nemo_file_operations_trash_or_delete (files, parent, trash_done, NULL);
}

static void
send_key (NemoWindow *window, const char *accelerator)
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
    gtk_widget_event (GTK_WIDGET (window), event);
    gdk_event_free (event);
    g_free (entries);
}

static GtkWidget *
find_confirmation (void)
{
    GList *windows = gtk_window_list_toplevels ();
    GtkWidget *dialog = NULL;
    for (GList *l = windows; l != NULL; l = l->next) {
        if (GTK_IS_MESSAGE_DIALOG (l->data) && gtk_widget_get_mapped (l->data)) {
            g_assert_null (dialog);
            dialog = l->data;
        }
    }
    g_list_free (windows);
    return dialog;
}

static gboolean
operation_enabled (NemoWindow *window, const char *name)
{
    for (GList *l = gtk_ui_manager_get_action_groups (nemo_window_get_ui_manager (window));
         l != NULL; l = l->next) {
        GtkAction *action = gtk_action_group_get_action (l->data, name);
        if (action != NULL)
            return gtk_action_is_sensitive (action) && gtk_action_is_visible (action);
    }
    return FALSE;
}

typedef struct {
    const char *view;
    const char *key;
    gboolean cancel;
    gboolean rebind;
    gboolean trash;
} DeleteCase;

static gboolean
respond_confirmation (gpointer data)
{
    const DeleteCase *test = data;
    GtkWidget *dialog = find_confirmation ();
    if (dialog == NULL)
        return G_SOURCE_CONTINUE;
    g_assert_true (expect_confirmation);
    g_assert_cmpuint (delete_calls, ==, !test->trash);
    g_assert_cmpuint (trash_calls, ==, test->trash);
    g_assert_true (gtk_window_get_modal (GTK_WINDOW (dialog)));
    g_assert_true (g_file_query_exists (expected_file, NULL));
    confirmed = TRUE;
    gtk_dialog_response (GTK_DIALOG (dialog), test->cancel ? GTK_RESPONSE_CANCEL : 1);
    return G_SOURCE_CONTINUE;
}

static gboolean
record_view_binding (NemoView *view, gpointer data)
{
    (*(guint *) data)++;
    return TRUE;
}

typedef struct {
    const char *action;
    const char *shortcut;
    const char *conflicting_default;
    const char *signal;
} CustomCase;

static void
test_custom_binding_preserved (gconstpointer data)
{
    const CustomCase *test = data;
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 25000000, 0);
        g_test_trap_assert_passed ();
        return;
    }
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    GSettings *settings = g_settings_new ("org.nemo.keybindings");
    g_assert_true (g_settings_set_string (settings, test->action, test->shortcut));
    application = nemo_main_application_get_singleton ();
    g_assert_true (g_application_register (G_APPLICATION (application), NULL, NULL));
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_DEFAULT_FOLDER_VIEWER, "list-view");
    Fixture fixture = fixture_new ();
    g_autofree char *alternate = g_settings_get_string (settings, test->conflicting_default);
    g_assert_cmpstr (alternate, ==, "");
    g_autofree char *custom = g_settings_get_string (settings, test->action);
    g_assert_cmpstr (custom, ==, test->shortcut);
    nemo_keybindings_apply_all ();
    guint bindings = 0;
    guint key;
    GdkModifierType modifiers;
    gtk_accelerator_parse (test->shortcut, &key, &modifiers);
    g_signal_connect (fixture.slot->content_view, test->signal, G_CALLBACK (record_view_binding), &bindings);
    g_assert_true (gtk_bindings_activate (G_OBJECT (fixture.slot->content_view),
                                         key, modifiers));
    g_assert_cmpuint (bindings, ==, 1);
    g_assert_cmpuint (trash_calls, ==, 0);
    g_assert_cmpuint (delete_calls, ==, 0);
    g_assert_true (g_file_query_exists (fixture.item, NULL));
    fixture_clear (&fixture);
    g_object_unref (settings);
    g_object_unref (application);
}

static void
test_shortcut (gconstpointer data)
{
    const DeleteCase *test = data;
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 25000000, 0);
        g_test_trap_assert_passed ();
        return;
    }
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    if (test->trash) {
        GSettings *settings = g_settings_new ("org.nemo.keybindings");
        g_assert_true (g_settings_set_string (settings, "trash", "F8"));
        g_object_unref (settings);
    }
    application = nemo_main_application_get_singleton ();
    g_assert_true (g_application_register (G_APPLICATION (application), NULL, NULL));
    g_settings_set_string (nemo_preferences, NEMO_PREFERENCES_DEFAULT_FOLDER_VIEWER, test->view);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_IGNORE_VIEW_METADATA, TRUE);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_ENABLE_DELETE, !test->trash);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_CONFIRM_TRASH, TRUE);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_CONFIRM_MOVE_TO_TRASH, TRUE);
    Fixture fixture = fixture_new ();
    nemo_window_slot_open_location (fixture.slot, fixture.containing, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, fixture.containing));
    NemoView *view = fixture.slot->content_view;
    NemoFile *file = nemo_file_get (fixture.item);
    GList selection = { .data = file };
    nemo_view_set_selection (view, &selection);
    nemo_view_grab_focus (view);
    expected_file = fixture.item;
    g_autofree char *original_contents = NULL;
    gsize original_length;
    if (test->trash)
        g_assert_true (g_file_load_contents (fixture.item, NULL, &original_contents,
                                             &original_length, NULL, NULL));
    WAIT_FOR (operation_enabled (fixture.window, test->trash ? NEMO_ACTION_TRASH : NEMO_ACTION_DELETE));
    guint response = g_timeout_add (10, respond_confirmation, (gpointer) test);

    GtkWidget *entry = gtk_entry_new ();
    gtk_box_pack_start (GTK_BOX (fixture.slot), entry, FALSE, FALSE, 0);
    gtk_widget_show (entry);
    gtk_entry_set_text (GTK_ENTRY (entry), "editable filename");
    gtk_widget_grab_focus (entry);
    send_key (fixture.window, test->key);
    iterate ();
    g_assert_cmpuint (delete_calls, ==, 0);
    g_assert_cmpuint (trash_calls, ==, 0);
    g_assert_null (find_confirmation ());
    gtk_widget_destroy (entry);
    nemo_view_grab_focus (view);

    if (test->rebind) {
        nemo_keybindings_set_for_action ("delete-permanently-alt", "<Control><Alt>F8");
        nemo_keybindings_set_for_action ("delete-permanently-alt", "");
        g_assert_false (gtk_bindings_activate (G_OBJECT (view), GDK_KEY_F8,
                                               GDK_CONTROL_MASK | GDK_MOD1_MASK));
        send_key (fixture.window, "<Control><Alt>F8");
        send_key (fixture.window, "<Shift>F8");
        iterate ();
        g_assert_cmpuint (delete_calls, ==, 0);
        g_assert_null (find_confirmation ());
        g_settings_reset (nemo_keybinding_settings, "delete-permanently-alt");
        nemo_view_set_selection (view, &selection);
        nemo_view_grab_focus (view);
        WAIT_FOR (operation_enabled (fixture.window, NEMO_ACTION_DELETE));
    }
    expect_confirmation = TRUE;
    send_key (fixture.window, test->key);
    g_assert_cmpuint (delete_calls, ==, !test->trash);
    g_assert_cmpuint (trash_calls, ==, test->trash);
    WAIT_FOR (completed);
    g_source_remove (response);
    g_assert_true (confirmed);
    g_assert_cmpint (cancelled, ==, test->cancel);
    g_assert_cmpint (g_file_query_exists (fixture.item, NULL), ==, test->cancel);
    NemoProgressInfoManager *manager = nemo_progress_info_manager_new ();
    WAIT_FOR (nemo_progress_info_manager_get_all_infos (manager) == NULL);
    g_object_unref (manager);
    if (!test->cancel) {
        if (test->trash) {
            g_autofree char *trashed_path = g_build_filename (
                g_get_user_data_dir (), "Trash", "files", "needle.txt", NULL);
            g_autoptr (GFile) trashed = g_file_new_for_path (trashed_path);
            g_autofree char *contents = NULL;
            gsize length;
            g_assert_true (g_file_load_contents (trashed, NULL, &contents, &length, NULL, NULL));
            g_assert_cmpmem (contents, length, original_contents, original_length);
            g_autofree char *info_path = g_build_filename (
                g_get_user_data_dir (), "Trash", "info", "needle.txt.trashinfo", NULL);
            g_autoptr (GKeyFile) info = g_key_file_new ();
            g_assert_true (g_key_file_load_from_file (info, info_path, G_KEY_FILE_NONE, NULL));
            g_autofree char *encoded_path = g_key_file_get_string (info, "Trash Info", "Path", NULL);
            g_autofree char *decoded_path = g_uri_unescape_string (encoded_path, NULL);
            g_autofree char *path = g_file_get_path (fixture.item);
            g_assert_cmpstr (decoded_path, ==, path);
            g_assert_true (g_file_move (trashed, fixture.item, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL));
            g_assert_cmpint (g_unlink (info_path), ==, 0);
        } else {
            g_autofree char *path = g_file_get_path (fixture.item);
            g_assert_true (g_file_set_contents (path, "temporary search fixture", -1, NULL));
        }
    }
    nemo_file_unref (file);
    fixture_clear (&fixture);
    g_object_unref (application);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    gtk_test_init (&argc, &argv, NULL);
    static const CustomCase custom_cases[] = {
        { "trash", "<Shift>F8", "delete-permanently-alt", "trash" },
        { "delete-permanently", "Delete", "trash-alt", "delete" },
        { "trash-alt", "<Shift>F8", "delete-permanently-alt", "trash" },
        { "delete-permanently", "F8", "trash", "delete" }
    };
    for (guint i = 0; i < G_N_ELEMENTS (custom_cases); i++) {
        g_autofree char *name = g_strdup_printf ("/delete-shortcuts/custom-%s-conflict/%s",
                                                custom_cases[i].action, custom_cases[i].shortcut);
        g_test_add_data_func (name, &custom_cases[i], test_custom_binding_preserved);
    }
    static const DeleteCase cases[] = {
        { "list-view", "<Shift>Delete", TRUE, FALSE },
        { "list-view", "<Shift>F8", FALSE, TRUE },
        { "icon-view", "<Shift>Delete", FALSE, FALSE },
        { "icon-view", "<Shift>F8", TRUE, FALSE },
        { "compact-view", "<Shift>Delete", TRUE, FALSE },
        { "compact-view", "<Shift>F8", FALSE, FALSE },
        { "list-view", "Delete", FALSE, FALSE, TRUE },
        { "list-view", "F8", TRUE, FALSE, TRUE },
        { "icon-view", "Delete", TRUE, FALSE, TRUE },
        { "icon-view", "F8", FALSE, FALSE, TRUE },
        { "compact-view", "Delete", FALSE, FALSE, TRUE },
        { "compact-view", "F8", TRUE, FALSE, TRUE }
    };
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
        g_autofree char *name = g_strdup_printf ("/delete-shortcuts/%s/%s",
            cases[i].view, cases[i].key);
        g_test_add_data_func (name, &cases[i], test_shortcut);
    }
    return g_test_run ();
}
