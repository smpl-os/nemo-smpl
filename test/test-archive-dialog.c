/* Exercise the real chooser and confirmations, never a compression backend.
 * Run through run-isolated-regression.py on its disposable desktop/profile. */
#include <config.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../src/nemo-archive-dialog.h"
#include <libnemo-private/nemo-archive-create.h>
#include <libnemo-private/nemo-progress-info-manager.h>

typedef struct {
    guint calls;
    guint finishes;
    GList *sources;
    GFile *destination;
    gboolean move;
    gboolean overwrite;
    GTask *task;
    NemoProgressInfo *progress;
} RecordedJob;

static RecordedJob job;
static guint fixture_number;

/* These definitions intentionally replace the archive engine at link time. */
NemoProgressInfo *
nemo_archive_create_async (GList *sources, GFile *destination,
                           gboolean move, gboolean overwrite, GtkWindow *parent,
                           GAsyncReadyCallback callback, gpointer user_data)
{
    g_assert_null (job.task);
    job.calls++;
    for (GList *l = sources; l != NULL; l = l->next) {
        g_assert_true (G_IS_FILE (l->data));
        job.sources = g_list_append (job.sources, g_object_ref (l->data));
    }
    job.destination = g_object_ref (destination);
    job.move = move;
    job.overwrite = overwrite;
    job.progress = nemo_progress_info_new ();
    nemo_progress_info_set_parent_window (job.progress, parent);
    g_autoptr (GCancellable) cancellable = nemo_progress_info_get_cancellable (job.progress);
    job.task = g_task_new (NULL, cancellable, callback, user_data);
    nemo_progress_info_start (job.progress);
    return g_object_ref (job.progress);
}

gboolean
nemo_archive_create_finish (GAsyncResult *result,
                            NemoArchiveCreateResult *outcome, GError **error)
{
    job.finishes++;
    gboolean success = g_task_propagate_boolean (G_TASK (result), error);
    *outcome = (NemoArchiveCreateResult) {
        .published = success, .archive_ok = success,
        .sources_removed = success && job.move
    };
    return success;
}

static void
iterate (void)
{
    while (g_main_context_iteration (NULL, FALSE));
    g_usleep (1000);
}

#define WAIT_FOR(condition) G_STMT_START { \
    gint64 deadline = g_get_monotonic_time () + 10000000; \
    while (!(condition) && g_get_monotonic_time () < deadline) iterate (); \
    g_assert_true (condition); \
} G_STMT_END

typedef struct {
    GtkWidget *parent;
    GtkWidget *chooser;
    char *path;
    GFile *directory;
    GFile *first;
    GFile *second;
} Fixture;

static GtkWidget *
find_dialog (gboolean chooser)
{
    GList *windows = gtk_window_list_toplevels ();
    GtkWidget *found = NULL;
    for (GList *l = windows; l; l = l->next) {
        GtkWidget *widget = l->data;
        if (gtk_widget_get_visible (widget) &&
            (chooser ? GTK_IS_FILE_CHOOSER (widget) : GTK_IS_MESSAGE_DIALOG (widget))) {
            g_assert_null (found);
            found = widget;
        }
    }
    g_list_free (windows);
    return found;
}

static gboolean
chooser_at (Fixture *fixture)
{
    g_autoptr (GFile) folder =
        gtk_file_chooser_get_current_folder_file (GTK_FILE_CHOOSER (fixture->chooser));
    return folder != NULL && g_file_equal (folder, fixture->directory);
}

static Fixture *
fixture_new (gboolean move, gboolean multiple)
{
    g_assert_cmpuint (job.calls, ==, 0);
    Fixture *fixture = g_new0 (Fixture, 1);
    g_autofree char *name = g_strdup_printf ("archive-dialog-%u", fixture_number++);
    fixture->path = g_build_filename (g_getenv ("NEMO_TEST_PROFILE"), name, NULL);
    g_assert_cmpint (g_mkdir (fixture->path, 0700), ==, 0);
    fixture->directory = g_file_new_for_path (fixture->path);
    fixture->first = g_file_get_child (fixture->directory, "résumé 日本語.txt");
    fixture->second = g_file_get_child (fixture->directory, "other.txt");
    g_assert_true (g_file_replace_contents (fixture->first, "first", 5, NULL, FALSE,
                                           G_FILE_CREATE_NONE, NULL, NULL, NULL));
    g_assert_true (g_file_replace_contents (fixture->second, "second", 6, NULL, FALSE,
                                           G_FILE_CREATE_NONE, NULL, NULL, NULL));
    fixture->parent = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    g_object_add_weak_pointer (G_OBJECT (fixture->parent), (gpointer *) &fixture->parent);
    gtk_widget_show (fixture->parent);

    /* Only the dialog owns these instances once show() has returned. */
    g_autofree char *first_uri = g_file_get_uri (fixture->first);
    g_autofree char *second_uri = g_file_get_uri (fixture->second);
    GList *sources = g_list_append (NULL, g_file_new_for_uri (first_uri));
    if (multiple)
        sources = g_list_append (sources, g_file_new_for_uri (second_uri));
    nemo_archive_dialog_show (GTK_WINDOW (fixture->parent), sources, fixture->directory, move);
    g_list_free_full (sources, g_object_unref);
    fixture->chooser = find_dialog (TRUE);
    g_assert_nonnull (fixture->chooser);
    g_object_add_weak_pointer (G_OBJECT (fixture->chooser), (gpointer *) &fixture->chooser);
    WAIT_FOR (chooser_at (fixture));
    g_assert_cmpint (gtk_file_chooser_get_action (GTK_FILE_CHOOSER (fixture->chooser)),
                     ==, GTK_FILE_CHOOSER_ACTION_SAVE);
    g_assert_true (gtk_window_get_modal (GTK_WINDOW (fixture->chooser)));
    g_assert_true (gtk_window_get_transient_for (GTK_WINDOW (fixture->chooser)) ==
                   GTK_WINDOW (fixture->parent));
    return fixture;
}

static void
complete_job (gboolean cancel)
{
    g_assert_nonnull (job.task);
    if (cancel) {
        g_autoptr (GCancellable) cancellable = nemo_progress_info_get_cancellable (job.progress);
        g_cancellable_cancel (cancellable);
    }
    nemo_progress_info_finish (job.progress);
    g_task_return_boolean (job.task, TRUE);
    g_clear_object (&job.task);
    WAIT_FOR (job.finishes == 1);
    gpointer progress = job.progress;
    g_object_add_weak_pointer (G_OBJECT (progress), &progress);
    g_clear_object (&job.progress);
    WAIT_FOR (progress == NULL);
}

static void
fixture_free (Fixture *fixture)
{
    if (fixture->parent != NULL)
        gtk_widget_destroy (fixture->parent);
    WAIT_FOR (fixture->chooser == NULL);
    WAIT_FOR (fixture->parent == NULL);
    g_assert_null (find_dialog (FALSE));
    g_assert_null (job.task);
    g_assert_null (job.progress);
    g_list_free_full (job.sources, g_object_unref);
    g_clear_object (&job.destination);
    job = (RecordedJob) { 0 };
    g_autofree char *contents = NULL;
    gsize size;
    g_assert_true (g_file_load_contents (fixture->first, NULL, &contents, &size, NULL, NULL));
    g_assert_cmpstr (contents, ==, "first");
    g_clear_pointer (&contents, g_free);
    g_assert_true (g_file_load_contents (fixture->second, NULL, &contents, &size, NULL, NULL));
    g_assert_cmpstr (contents, ==, "second");
    g_assert_true (g_file_delete (fixture->first, NULL, NULL));
    g_assert_true (g_file_delete (fixture->second, NULL, NULL));
    g_assert_true (g_file_delete (fixture->directory, NULL, NULL));
    g_object_unref (fixture->first);
    g_object_unref (fixture->second);
    g_object_unref (fixture->directory);
    g_free (fixture->path);
    g_free (fixture);
}

static void
accept_name (Fixture *fixture, const char *name)
{
    gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (fixture->chooser), name);
    gtk_dialog_response (GTK_DIALOG (fixture->chooser), GTK_RESPONSE_ACCEPT);
}

static GtkWidget *
confirmation_for (Fixture *fixture, const char *name)
{
    WAIT_FOR (find_dialog (FALSE) != NULL);
    GtkWidget *confirmation = find_dialog (FALSE);
    g_assert_cmpuint (job.calls, ==, 0);
    GtkMessageType type;
    char *secondary = NULL;
    g_object_get (confirmation, "message-type", &type, "secondary-text", &secondary, NULL);
    g_assert_cmpint (type, ==, GTK_MESSAGE_WARNING);
    g_autofree char *path = g_build_filename (fixture->path, name, NULL);
    g_assert_nonnull (strstr (secondary, path));
    g_free (secondary);
    g_assert_true (gtk_window_get_modal (GTK_WINDOW (confirmation)));
    g_assert_true (gtk_window_get_transient_for (GTK_WINDOW (confirmation)) ==
                   GTK_WINDOW (fixture->chooser));
    g_assert_true (gtk_window_get_default_widget (GTK_WINDOW (confirmation)) ==
                   gtk_dialog_get_widget_for_response (GTK_DIALOG (confirmation), GTK_RESPONSE_CANCEL));
    return confirmation;
}

static void
assert_job (Fixture *fixture, const char *name, gboolean move, gboolean overwrite,
            guint source_count)
{
    WAIT_FOR (job.calls == 1);
    WAIT_FOR (fixture->chooser == NULL);
    g_autoptr (GFile) expected = g_file_get_child (fixture->directory, name);
    g_assert_true (g_file_equal (job.destination, expected));
    g_assert_cmpint (job.move, ==, move);
    g_assert_cmpint (job.overwrite, ==, overwrite);
    g_assert_cmpuint (g_list_length (job.sources), ==, source_count);
    g_assert_true (g_file_equal (job.sources->data, fixture->first));
    if (source_count == 2)
        g_assert_true (g_file_equal (job.sources->next->data, fixture->second));
    g_autoptr (GtkWindow) parent = nemo_progress_info_get_parent_window (job.progress);
    g_assert_true (parent == GTK_WINDOW (fixture->parent));
}

static void
test_copy_names (gconstpointer data)
{
    const char *name = data;
    Fixture *fixture = fixture_new (FALSE, TRUE);
    g_autofree char *initial = gtk_file_chooser_get_current_name (GTK_FILE_CHOOSER (fixture->chooser));
    g_autofree char *basename = g_file_get_basename (fixture->directory);
    g_autofree char *expected_initial = g_strconcat (basename, ".7z", NULL);
    g_assert_cmpstr (initial, ==, expected_initial);
    accept_name (fixture, name);
    g_autofree char *expected = g_str_has_suffix (name, ".7Z") || g_str_has_suffix (name, ".7z")
        ? g_strdup (name) : g_strconcat (name, ".7z", NULL);
    assert_job (fixture, expected, FALSE, FALSE, 2);
    g_assert_null (find_dialog (FALSE));
    complete_job (FALSE);
    g_autoptr (GFile) destination = g_file_get_child (fixture->directory, expected);
    g_assert_false (g_file_query_exists (destination, NULL));
    fixture_free (fixture);
}

typedef struct { gboolean move; gboolean existing; gboolean accept; } ConfirmationCase;

static void
test_confirmation (gconstpointer data)
{
    const ConfirmationCase *test = data;
    Fixture *fixture = fixture_new (test->move, FALSE);
    g_autofree char *initial = gtk_file_chooser_get_current_name (GTK_FILE_CHOOSER (fixture->chooser));
    g_assert_cmpstr (initial, ==, "résumé 日本語.txt.7z");
    g_autoptr (GFile) target = g_file_get_child (fixture->directory, "résultat.7z");
    if (test->existing)
        g_assert_true (g_file_replace_contents (target, "old archive", 11, NULL, FALSE,
                                               G_FILE_CREATE_NONE, NULL, NULL, NULL));
    /* The entered name does not exist; only the final suffixed name collides. */
    accept_name (fixture, "résultat");
    GtkWidget *confirmation = confirmation_for (fixture, "résultat.7z");
    gtk_dialog_response (GTK_DIALOG (confirmation),
                         test->accept ? GTK_RESPONSE_ACCEPT : GTK_RESPONSE_CANCEL);
    if (test->accept) {
        assert_job (fixture, "résultat.7z", test->move, test->existing, 1);
        complete_job (FALSE);
    } else {
        WAIT_FOR (find_dialog (FALSE) == NULL);
        g_assert_cmpuint (job.calls, ==, 0);
        g_assert_true (gtk_widget_get_sensitive (gtk_dialog_get_widget_for_response (
                       GTK_DIALOG (fixture->chooser), GTK_RESPONSE_ACCEPT)));
        gtk_dialog_response (GTK_DIALOG (fixture->chooser), GTK_RESPONSE_CANCEL);
    }
    if (test->existing) {
        g_autofree char *contents = NULL;
        g_assert_true (g_file_load_contents (target, NULL, &contents, NULL, NULL, NULL));
        g_assert_cmpstr (contents, ==, "old archive");
        g_assert_true (g_file_delete (target, NULL, NULL));
    } else {
        g_assert_false (g_file_query_exists (target, NULL));
    }
    fixture_free (fixture);
}

static void
test_reject_nonregular (gconstpointer data)
{
    gboolean symlink = GPOINTER_TO_INT (data);
    Fixture *fixture = fixture_new (FALSE, FALSE);
    g_autoptr (GFile) target = g_file_get_child (fixture->directory, "occupied.7z");
    if (symlink)
        g_assert_true (g_file_make_symbolic_link (target, "résumé 日本語.txt", NULL, NULL));
    else
        g_assert_true (g_file_make_directory (target, NULL, NULL));
    accept_name (fixture, "occupied");
    WAIT_FOR (find_dialog (FALSE) != NULL);
    GtkWidget *error = find_dialog (FALSE);
    GtkMessageType type;
    g_object_get (error, "message-type", &type, NULL);
    g_assert_cmpint (type, ==, GTK_MESSAGE_ERROR);
    g_assert_cmpuint (job.calls, ==, 0);
    gtk_dialog_response (GTK_DIALOG (error), GTK_RESPONSE_CLOSE);
    WAIT_FOR (find_dialog (FALSE) == NULL);
    g_assert_true (g_file_delete (target, NULL, NULL));
    accept_name (fixture, "safe");
    assert_job (fixture, "safe.7z", FALSE, FALSE, 1);
    complete_job (FALSE);
    fixture_free (fixture);
}

static void
test_cancel_chooser (void)
{
    Fixture *fixture = fixture_new (FALSE, FALSE);
    gtk_dialog_response (GTK_DIALOG (fixture->chooser), GTK_RESPONSE_CANCEL);
    WAIT_FOR (fixture->chooser == NULL);
    g_assert_cmpuint (job.calls, ==, 0);
    fixture_free (fixture);
}

static void
destroy_parent_after_accept (GtkDialog *dialog, gint response, gpointer data)
{
    Fixture *fixture = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        /* This runs after the real handler has queued its asynchronous query,
         * but before that query's completion can enter the main context. */
        g_assert_false (gtk_widget_get_sensitive (
            gtk_dialog_get_widget_for_response (dialog, GTK_RESPONSE_ACCEPT)));
        g_assert_cmpuint (job.calls, ==, 0);
        gtk_widget_destroy (fixture->parent);
    }
}

static void
test_parent_during_query (void)
{
    Fixture *fixture = fixture_new (FALSE, FALSE);
    g_signal_connect_after (fixture->chooser, "response",
                            G_CALLBACK (destroy_parent_after_accept), fixture);
    accept_name (fixture, "cancel-query");
    WAIT_FOR (fixture->parent == NULL);
    WAIT_FOR (fixture->chooser == NULL);
    g_assert_cmpuint (job.calls, ==, 0);
    fixture_free (fixture);
}

static void
test_parent_during_confirmation (void)
{
    Fixture *fixture = fixture_new (TRUE, FALSE);
    accept_name (fixture, "cancel-confirm");
    GtkWidget *confirmation = confirmation_for (fixture, "cancel-confirm.7z");
    gpointer weak = confirmation;
    g_object_add_weak_pointer (G_OBJECT (confirmation), &weak);
    gtk_widget_destroy (fixture->parent);
    WAIT_FOR (weak == NULL);
    WAIT_FOR (fixture->chooser == NULL);
    g_assert_cmpuint (job.calls, ==, 0);
    fixture_free (fixture);
}

static void
test_parent_during_job (gconstpointer data)
{
    Fixture *fixture = fixture_new (FALSE, FALSE);
    accept_name (fixture, "background");
    assert_job (fixture, "background.7z", FALSE, FALSE, 1);
    gtk_widget_destroy (fixture->parent);
    WAIT_FOR (fixture->parent == NULL);
    g_autoptr (GtkWindow) parent = nemo_progress_info_get_parent_window (job.progress);
    g_assert_null (parent);
    complete_job (GPOINTER_TO_INT (data));
    g_assert_null (find_dialog (FALSE));
    fixture_free (fixture);
}

static void
application_activated (GApplication *application, gpointer data)
{
}

static gboolean
application_hold_deadline (gpointer data)
{
    g_error ("An archive job leaked its application hold");
    return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
        g_printerr ("Run archive-dialog tests through run-isolated-regression.py.\n");
        return 77;
    }
    g_assert_nonnull (g_getenv ("NEMO_TEST_PROFILE"));
    g_assert_cmpstr (g_getenv ("GSETTINGS_BACKEND"), ==, "memory");
    g_assert_cmpstr (g_getenv ("GDK_BACKEND"), ==, "x11");
    g_assert_null (g_getenv ("WAYLAND_DISPLAY"));
    gtk_test_init (&argc, &argv, NULL);
    g_autoptr (GApplication) application =
        g_application_new ("org.nemo.ArchiveDialogRegression", G_APPLICATION_NON_UNIQUE);
    g_signal_connect (application, "activate", G_CALLBACK (application_activated), NULL);
    g_autoptr (GError) error = NULL;
    g_assert_true (g_application_register (application, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_application_get_default () == application);
    static const ConfirmationCase confirmations[] = {
        { FALSE, TRUE, TRUE }, { FALSE, TRUE, FALSE },
        { TRUE, FALSE, TRUE }, { TRUE, FALSE, FALSE },
        { TRUE, TRUE, TRUE }, { TRUE, TRUE, FALSE }
    };
    g_test_add_data_func ("/archive-dialog/copy/append-extension", "résultat 日本語", test_copy_names);
    g_test_add_data_func ("/archive-dialog/copy/keep-extension", "existing.7z", test_copy_names);
    g_test_add_data_func ("/archive-dialog/copy/keep-uppercase-extension", "existing.7Z", test_copy_names);
    g_test_add_data_func ("/archive-dialog/copy/append-to-other-extension", "existing.zip", test_copy_names);
    g_test_add_data_func ("/archive-dialog/collision/accept", &confirmations[0], test_confirmation);
    g_test_add_data_func ("/archive-dialog/collision/cancel", &confirmations[1], test_confirmation);
    g_test_add_data_func ("/archive-dialog/move/accept", &confirmations[2], test_confirmation);
    g_test_add_data_func ("/archive-dialog/move/cancel", &confirmations[3], test_confirmation);
    g_test_add_data_func ("/archive-dialog/move-overwrite/accept", &confirmations[4], test_confirmation);
    g_test_add_data_func ("/archive-dialog/move-overwrite/cancel", &confirmations[5], test_confirmation);
    g_test_add_data_func ("/archive-dialog/reject/directory", GINT_TO_POINTER (FALSE), test_reject_nonregular);
    g_test_add_data_func ("/archive-dialog/reject/symlink", GINT_TO_POINTER (TRUE), test_reject_nonregular);
    g_test_add_func ("/archive-dialog/cancel-chooser", test_cancel_chooser);
    g_test_add_func ("/archive-dialog/parent/query", test_parent_during_query);
    g_test_add_func ("/archive-dialog/parent/confirmation", test_parent_during_confirmation);
    g_test_add_data_func ("/archive-dialog/parent/successful-job", GINT_TO_POINTER (FALSE), test_parent_during_job);
    g_test_add_data_func ("/archive-dialog/parent/cancelled-job", GINT_TO_POINTER (TRUE), test_parent_during_job);
    int status = g_test_run ();
    guint deadline = g_timeout_add_seconds (10, application_hold_deadline, NULL);
    char *application_argv[] = { (char *) "test-archive-dialog", NULL };
    /* With every asynchronous job finished, run() must have no remaining
     * application holds. A leaked hold would keep Nemo alive after closing. */
    g_assert_cmpint (g_application_run (application, 1, application_argv), ==, 0);
    g_source_remove (deadline);
    return status;
}
