#include <config.h>
#include <gtk/gtk.h>
#include <libnemo-private/nemo-package-version.h>

static GThread *main_thread;
static gint query_started;
static gint query_finished;
static gboolean block_query;
static gboolean fail_query;

static char *
fixture_package_version (GCancellable *cancel, GError **error)
{
	g_assert_true (g_thread_self () != main_thread);
	g_atomic_int_set (&query_started, 1);
	while (block_query && !g_cancellable_is_cancelled (cancel))
		g_usleep (1000);
	g_atomic_int_set (&query_finished, 1);
	if (g_cancellable_set_error_if_cancelled (cancel, error))
		return NULL;
	if (fail_query) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Package database unavailable");
		return NULL;
	}
	return g_strdup ("9.8.7-4");
}

#define nemo_get_package_version fixture_package_version
#include "../src/nemo-window-menus.c"
#undef nemo_get_package_version

static void
drain (void)
{
	while (g_main_context_iteration (NULL, FALSE));
	g_usleep (1000);
}

static GtkWidget *
show_about (GtkWidget *parent)
{
	action_about_nemo_callback (NULL, parent);
	GList *windows = gtk_window_list_toplevels ();
	GtkWidget *about = NULL;
	for (GList *l = windows; l != NULL; l = l->next)
		if (GTK_IS_ABOUT_DIALOG (l->data))
			about = g_object_ref (l->data);
	g_list_free (windows);
	g_assert_nonnull (about);
	return about;
}

static void
test_about_package_version (gconstpointer failure)
{
	GtkWidget *parent = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	fail_query = GPOINTER_TO_INT (failure);
	block_query = FALSE;
	if (fail_query)
		g_test_expect_message ("Nemo", G_LOG_LEVEL_WARNING, "*Package database unavailable*");
	GtkWidget *about = show_about (parent);
	const char *expected = fail_query ? "Package version unavailable" : "9.8.7-4";
	gint64 deadline = g_get_monotonic_time () + 5000000;
	while (g_strcmp0 (gtk_about_dialog_get_version (GTK_ABOUT_DIALOG (about)), expected) != 0 &&
	       g_get_monotonic_time () < deadline)
		drain ();
	g_assert_cmpstr (gtk_about_dialog_get_version (GTK_ABOUT_DIALOG (about)), ==, expected);
	if (fail_query)
		g_test_assert_expected_messages ();
	gtk_widget_destroy (about);
	g_object_unref (about);
	gtk_widget_destroy (parent);
}

static void
test_close_pending_about (void)
{
	GtkWidget *parent = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	g_atomic_int_set (&query_started, 0);
	g_atomic_int_set (&query_finished, 0);
	block_query = TRUE;
	fail_query = FALSE;
	GtkWidget *about = show_about (parent);
	gint64 deadline = g_get_monotonic_time () + 5000000;
	while (!g_atomic_int_get (&query_started) && g_get_monotonic_time () < deadline)
		drain ();
	g_assert_cmpint (g_atomic_int_get (&query_started), ==, 1);
	gpointer weak = about;
	g_object_add_weak_pointer (G_OBJECT (about), &weak);
	gtk_widget_destroy (about);
	g_object_unref (about);
	g_assert_null (weak);
	while (!g_atomic_int_get (&query_finished) && g_get_monotonic_time () < deadline)
		drain ();
	g_assert_cmpint (g_atomic_int_get (&query_finished), ==, 1);
	for (guint i = 0; i < 30; i++)
		drain ();
	gtk_widget_destroy (parent);
}

int
main (int argc, char **argv)
{
	if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
		return 77;
	gtk_test_init (&argc, &argv, NULL);
	main_thread = g_thread_self ();
	g_test_add_data_func ("/about/package-version", GINT_TO_POINTER (FALSE), test_about_package_version);
	g_test_add_data_func ("/about/package-error", GINT_TO_POINTER (TRUE), test_about_package_version);
	g_test_add_func ("/about/close-pending", test_close_pending_about);
	return g_test_run ();
}
