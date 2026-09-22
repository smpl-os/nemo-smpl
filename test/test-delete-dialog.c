/* Exercise the real asynchronous confirmations using only disposable files.
 * Run through run-isolated-regression.py, never on the user's desktop. */
#include <config.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>

#include <libnemo-private/nemo-file-operations.h>
#include <libnemo-private/nemo-file-undo-manager.h>
#include <libnemo-private/nemo-global-preferences.h>
#include <libnemo-private/nemo-progress-info-manager.h>

typedef struct {
    gboolean trash;
    gboolean cancel;
    gboolean close_parent;
} TestCase;

static GtkWidget *parent;
static gboolean presented;
static gboolean responded;
static gboolean completed;
static gboolean cancelled;
static guint parent_keys;

void __real_gtk_window_present (GtkWindow *window);

void
__wrap_gtk_window_present (GtkWindow *window)
{
    if (GTK_IS_MESSAGE_DIALOG (window)) {
        GSignalInvocationHint *hint = g_signal_get_invocation_hint (
            gtk_widget_get_frame_clock (GTK_WIDGET (window)));

        g_assert_nonnull (hint);
        g_assert_cmpstr (g_signal_name (hint->signal_id), ==, "after-paint");
        g_assert_false (presented);
        g_assert_true (gtk_window_get_modal (window));
        g_assert_true (gtk_widget_get_mapped (GTK_WIDGET (window)));
        g_assert_true (gtk_window_get_transient_for (window) ==
                       (parent != NULL ? GTK_WINDOW (parent) : NULL));
        if (parent != NULL) {
            g_assert_true (gtk_window_get_group (window) ==
                           gtk_window_get_group (GTK_WINDOW (parent)));
        }
        presented = TRUE;
    }
    __real_gtk_window_present (window);
}

/* There is no power manager on the isolated test bus. */
int
__wrap_nemo_inhibit_power_manager (const char *message)
{
    return -1;
}

static gboolean
parent_key_pressed (GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    parent_keys++;
    return TRUE;
}

static gboolean
deadline (gpointer data)
{
    g_error ("Timed out waiting for the deletion confirmation "
             "(presented=%d, responded=%d, completed=%d)",
             presented, responded, completed);
    return G_SOURCE_REMOVE;
}

static void
delete_done (GHashTable *debuting, gboolean user_cancel, gpointer data)
{
    completed = TRUE;
    cancelled = user_cancel;
}

static gboolean
respond (gpointer data)
{
    const TestCase *test = data;
    GList *windows = gtk_window_list_toplevels ();

    for (GList *l = windows; l != NULL; l = l->next) {
        GtkWidget *dialog = l->data;

        if (!GTK_IS_MESSAGE_DIALOG (dialog) || !gtk_widget_get_mapped (dialog)) {
            continue;
        }

        if (!presented || !gtk_window_is_active (GTK_WINDOW (dialog))) {
            continue;
        }
        g_assert_false (responded);
        g_assert_true (gtk_window_get_modal (GTK_WINDOW (dialog)));
        g_assert_true (gtk_window_group_get_current_grab (
                       gtk_window_get_group (GTK_WINDOW (dialog))) == dialog);

        GtkWidget *confirm = gtk_dialog_get_widget_for_response (GTK_DIALOG (dialog), 1);
        g_assert_nonnull (confirm);
        g_assert_true (gtk_window_get_default_widget (GTK_WINDOW (dialog)) == confirm);
        g_assert_true (gtk_window_get_focus (GTK_WINDOW (dialog)) == confirm);

        /* Even if the compositor delivers a key to the parent, the modal
         * grab must prevent the file view from activating its selection. */
        if (parent != NULL) {
            GdkEvent *event = gdk_event_new (GDK_KEY_PRESS);
            event->key.window = g_object_ref (gtk_widget_get_window (parent));
            event->key.keyval = GDK_KEY_Return;
            event->key.time = GDK_CURRENT_TIME;
            gdk_event_set_device (event, gdk_seat_get_keyboard (
                gdk_display_get_default_seat (gtk_widget_get_display (parent))));
            gtk_main_do_event (event);
            gdk_event_free (event);
            g_assert_cmpuint (parent_keys, ==, 0);
        }

        responded = TRUE;
        g_assert_true (gtk_test_widget_send_key (confirm,
                      test->cancel ? GDK_KEY_Escape : GDK_KEY_Return, 0));
        break;
    }

    g_list_free (windows);
    return responded ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static void
test_confirmation (gconstpointer data)
{
    const TestCase *test = data;
    GError *error = NULL;
    char *path = g_build_filename (g_getenv ("NEMO_TEST_PROFILE"), "delete-me", NULL);
    GFile *file = g_file_new_for_path (path);
    GList files = { .data = file };
    GtkWindowGroup *group = gtk_window_group_new ();
    NemoProgressInfoManager *manager = nemo_progress_info_manager_new ();

    presented = responded = completed = cancelled = FALSE;
    parent_keys = 0;
    g_assert_true (g_file_set_contents (path, "disposable test file", -1, &error));
    g_assert_no_error (error);

    parent = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_group_add_window (group, GTK_WINDOW (parent));
    g_signal_connect (parent, "key-press-event", G_CALLBACK (parent_key_pressed), NULL);
    gtk_window_present (GTK_WINDOW (parent));

    guint timeout = g_timeout_add_seconds (10, deadline, NULL);

    if (test->trash) {
        nemo_file_operations_trash_or_delete (&files, GTK_WINDOW (parent), delete_done, NULL);
    } else {
        nemo_file_operations_delete (&files, GTK_WINDOW (parent), delete_done, NULL);
    }
    if (test->close_parent) {
        gtk_widget_destroy (parent);
        parent = NULL;
    }
    g_timeout_add (10, respond, (gpointer) test);

    while (!completed || nemo_progress_info_manager_get_all_infos (manager) != NULL) {
        g_main_context_iteration (NULL, TRUE);
    }
    g_source_remove (timeout);

    g_assert_true (presented);
    g_assert_true (responded);
    g_assert_cmpint (cancelled, ==, test->cancel);
    g_assert_cmpint (g_file_query_exists (file, NULL), ==, test->cancel);
    g_assert_cmpuint (parent_keys, ==, 0);
    if (test->cancel) {
        g_assert_cmpint (g_remove (path), ==, 0);
    }
    if (parent != NULL) {
        gtk_widget_destroy (parent);
        parent = NULL;
    }
    g_object_unref (group);
    g_object_unref (manager);
    g_object_unref (file);
    g_free (path);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
        g_printerr ("Run delete-dialog tests through run-isolated-regression.py.\n");
        return 77;
    }
    g_assert_nonnull (g_getenv ("NEMO_TEST_PROFILE"));
    g_assert_cmpstr (g_getenv ("GSETTINGS_BACKEND"), ==, "memory");
    g_assert_cmpstr (g_getenv ("GDK_BACKEND"), ==, "x11");
    g_assert_null (g_getenv ("WAYLAND_DISPLAY"));
    gtk_test_init (&argc, &argv, NULL);
    nemo_global_preferences_init ();
    nemo_file_undo_manager_get ();
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_CONFIRM_TRASH, TRUE);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_CONFIRM_MOVE_TO_TRASH, TRUE);

    static const TestCase cases[] = {
        { FALSE, FALSE, FALSE },
        { FALSE, TRUE, FALSE },
        { TRUE, FALSE, FALSE },
        { TRUE, TRUE, FALSE },
        { FALSE, TRUE, TRUE },
    };
    g_test_add_data_func ("/delete-dialog/delete-enter", &cases[0], test_confirmation);
    g_test_add_data_func ("/delete-dialog/delete-escape", &cases[1], test_confirmation);
    g_test_add_data_func ("/delete-dialog/trash-enter", &cases[2], test_confirmation);
    g_test_add_data_func ("/delete-dialog/trash-escape", &cases[3], test_confirmation);
    g_test_add_data_func ("/delete-dialog/closed-parent", &cases[4], test_confirmation);
    return g_test_run ();
}
