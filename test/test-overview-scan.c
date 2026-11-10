#include <config.h>
#include <glib/gstdio.h>
#include "../src/nemo-overview.c"

static gboolean advance_clock;
static gint64 scan_clock;

/* Advance scan time without slowing down filesystem fixtures. */
gint64 __real_g_get_monotonic_time (void);
int __real_lstat (const char *path, struct stat *st);
int __real_lstat64 (const char *path, struct stat *st);

int
__wrap_lstat (const char *path, struct stat *st)
{
	if (advance_clock)
		scan_clock += G_USEC_PER_SEC;
	return __real_lstat (path, st);
}

int
__wrap_lstat64 (const char *path, struct stat *st)
{
	if (advance_clock)
		scan_clock += G_USEC_PER_SEC;
	return __real_lstat64 (path, st);
}

gint64
__wrap_g_get_monotonic_time (void)
{
	if (advance_clock) {
		scan_clock += G_USEC_PER_SEC;
		return scan_clock;
	}
	return __real_g_get_monotonic_time ();
}

GType nemo_window_get_type (void) { return GTK_TYPE_WINDOW; }
NemoWindowSlot *nemo_window_get_active_slot (NemoWindow *window)
{
	g_assert_not_reached ();
}
void nemo_window_slot_open_location_full (NemoWindowSlot *slot, GFile *location,
	NemoWindowOpenFlags flags, GList *selection, NemoWindowGoToCallback callback,
	gpointer data)
{
	g_assert_not_reached ();
}

static GtkWidget *
find_widget (GtkWidget *root, GType type)
{
	GtkWidget *found = NULL;
	GList *children;

	if (G_TYPE_CHECK_INSTANCE_TYPE (root, type))
		return root;
	if (!GTK_IS_CONTAINER (root))
		return NULL;
	children = gtk_container_get_children (GTK_CONTAINER (root));
	for (GList *l = children; l != NULL && found == NULL; l = l->next)
		found = find_widget (l->data, type);
	g_list_free (children);
	return found;
}

static char *
make_tree (void)
{
	char *root = g_build_filename (g_getenv ("NEMO_TEST_PROFILE"),
	                               "nemo-overview-scan-XXXXXX", NULL);
	g_assert_nonnull (g_mkdtemp (root));
	for (guint i = 0; i < 32; i++) {
		g_autofree char *name = g_strdup_printf ("directory-%02u", i);
		g_autofree char *directory = g_build_filename (root, name, NULL);
		g_autofree char *file = g_build_filename (directory, ".hidden", NULL);
		g_assert_cmpint (g_mkdir (directory, 0700), ==, 0);
		g_assert_true (g_file_set_contents (file, "1234567890", 10, NULL));
	}
	g_autofree char *largest = g_build_filename (root, "home", NULL);
	g_autofree char *nested = g_build_filename (largest, "nested", NULL);
	g_autofree char *file = g_build_filename (nested, "data", NULL);
	g_autofree char *link = g_build_filename (root, "link", NULL);
	g_assert_cmpint (g_mkdir_with_parents (nested, 0700), ==, 0);
	g_assert_true (g_file_set_contents (file, "12345678901234567890", 20, NULL));
	g_assert_cmpint (symlink (largest, link), ==, 0);
	return root;
}

static void
remove_tree (const char *root)
{
	GDir *dir = g_dir_open (root, 0, NULL);
	const char *name;

	g_assert_nonnull (dir);
	while ((name = g_dir_read_name (dir)) != NULL) {
		g_autofree char *path = g_build_filename (root, name, NULL);
		struct stat st;
		g_assert_cmpint (lstat (path, &st), ==, 0);
		if (S_ISDIR (st.st_mode))
			remove_tree (path);
		else
			g_assert_cmpint (g_unlink (path), ==, 0);
	}
	g_dir_close (dir);
	g_assert_cmpint (g_rmdir (root), ==, 0);
}

static void
assert_scan (GArray *entries, const NemoDirScanStats *stats)
{
	gboolean found_home = FALSE;
	guint64 ranked_sum = 0;

	g_assert_nonnull (entries);
	g_assert_cmpuint (entries->len, ==, NEMO_DIR_SCAN_MAX_ENTRIES);
	g_assert_false (stats->incomplete);
	g_assert_false (stats->timed_out);
	g_assert_cmpuint (stats->total_size, ==, 340);
	for (guint i = 0; i < entries->len; i++) {
		NemoDirEntry *entry = &g_array_index (entries, NemoDirEntry, i);
		ranked_sum += entry->size;
		if (g_str_equal (entry->name, "./home")) {
			found_home = TRUE;
			g_assert_cmpuint (entry->size, ==, 20);
		}
		if (i > 0)
			g_assert_cmpuint (entry->size, <=,
			                 g_array_index (entries, NemoDirEntry, i - 1).size);
	}
	g_assert_true (found_home);
	g_assert_cmpuint (ranked_sum, !=, stats->total_size);
}

static void
test_overview_complete_and_cached (void)
{
	g_autofree char *root = make_tree ();
	NemoOverview *overview = g_object_ref_sink (g_object_new (NEMO_TYPE_OVERVIEW, NULL));
	ScanThreadData *td = g_new0 (ScanThreadData, 1);
	ScanResult cached = { 0 };
	GtkWidget *analyzer, *label;
	g_autoptr(GCancellable) cancel = g_cancellable_new ();

	add_pareto_placeholder (overview, "Fixture", root);
	td->self = g_object_ref (overview);
	td->cancel = g_object_ref (cancel);
	td->n_volumes = 1;
	td->mount_paths = g_new0 (char *, 1);
	td->volume_names = g_new0 (char *, 1);
	td->colour_idxs = g_new0 (int, 1);
	td->mount_paths[0] = g_strdup (root);
	td->volume_names[0] = g_strdup ("Fixture");

	advance_clock = TRUE;
	scan_clock = 0;
	g_thread_join (g_thread_new ("test-overview", scan_thread_func, td));
	advance_clock = FALSE;
	g_assert_cmpint (scan_clock, >, 12000 * 1000);
	while (g_main_context_iteration (NULL, FALSE));

	analyzer = find_widget (overview->pareto_box, NEMO_TYPE_DIR_ANALYZER);
	g_assert_nonnull (analyzer);
	assert_scan (nemo_dir_analyzer_get_entries (NEMO_DIR_ANALYZER (analyzer)),
	             nemo_dir_analyzer_get_stats (NEMO_DIR_ANALYZER (analyzer)));
	label = find_widget (analyzer, GTK_TYPE_LABEL);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==,
	                 "Total: 340 bytes — Top 10 entries");

	g_assert_true (scan_cache_lookup_copy (root, &cached));
	assert_scan (cached.deep, &cached.stats);
	nemo_dir_analyzer_set_entries (NEMO_DIR_ANALYZER (analyzer), cached.deep,
	                               "Cached", 0, &cached.stats);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==,
	                 "Total: 340 bytes — Top 10 entries");
	g_clear_pointer (&cached.deep, g_array_unref);

	CacheScanJob *job = g_new0 (CacheScanJob, 1);
	job->n_volumes = 1;
	job->mount_paths = g_new0 (char *, 1);
	job->volume_names = g_new0 (char *, 1);
	job->colour_idxs = g_new0 (int, 1);
	job->mount_paths[0] = g_strdup (root);
	job->volume_names[0] = g_strdup ("Fixture");
	advance_clock = TRUE;
	scan_clock = 0;
	g_thread_join (g_thread_new ("test-cache", cache_scan_thread_func, job));
	advance_clock = FALSE;
	g_assert_cmpint (scan_clock, >, 8000 * 1000);
	g_assert_true (scan_cache_lookup_copy (root, &cached));
	assert_scan (cached.deep, &cached.stats);
	g_clear_pointer (&cached.deep, g_array_unref);

	gtk_widget_destroy (GTK_WIDGET (overview));
	g_object_unref (overview);
	g_hash_table_remove_all (g_scan_cache);
	remove_tree (root);
}

static void
assert_overview_status (const char *root, GArray *entries,
                       const NemoDirScanStats *stats, const char *text)
{
	NemoOverview *overview = g_object_ref_sink (g_object_new (NEMO_TYPE_OVERVIEW, NULL));
	ScanResult *result = g_new0 (ScanResult, 1);
	add_pareto_placeholder (overview, "Fixture", root);
	result->self = g_object_ref (overview);
	result->cancel = g_cancellable_new ();
	result->mount_path = g_strdup (root);
	result->volume_name = g_strdup ("Fixture");
	result->deep = nemo_dir_entry_array_dup (entries);
	result->stats = *stats;
	pareto_idle_cb (result);
	GtkWidget *analyzer = find_widget (overview->pareto_box, NEMO_TYPE_DIR_ANALYZER);
	g_assert_nonnull (analyzer);
	GtkWidget *label = find_widget (analyzer, GTK_TYPE_LABEL);
	g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (label)), text));
	gtk_widget_destroy (GTK_WIDGET (overview));
	g_object_unref (overview);
}

static void
test_incomplete_and_empty (void)
{
	g_autofree char *root = make_tree ();
	g_autoptr(GCancellable) cancel = g_cancellable_new ();
	NemoDirScanStats stats;
	GArray *entries;
	NemoDirAnalyzer *analyzer = g_object_ref_sink (nemo_dir_analyzer_new ());
	GtkWidget *label = find_widget (GTK_WIDGET (analyzer), GTK_TYPE_LABEL);

	advance_clock = TRUE;
	entries = nemo_dir_scan_collect_top (root, cancel, 1, &stats);
	advance_clock = FALSE;
	g_assert_true (stats.incomplete);
	g_assert_true (stats.timed_out);
	nemo_dir_analyzer_set_entries (analyzer, entries, "Timed out", 0, &stats);
	g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (label)), "Incomplete"));
	g_array_unref (entries);

	advance_clock = TRUE;
	entries = nemo_dir_scan_collect_top (root, cancel, 30000, &stats);
	advance_clock = FALSE;
	g_assert_cmpuint (entries->len, >, 0);
	g_assert_true (stats.timed_out);
	nemo_dir_analyzer_set_entries (analyzer, entries, "Partial", 0, &stats);
	g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (label)), "scan time limit reached"));
	g_array_unref (entries);

	if (getuid () != 0) {
		g_autofree char *directory = g_build_filename (root, "home", NULL);
		g_assert_cmpint (g_chmod (directory, 0000), ==, 0);
		entries = nemo_dir_scan_collect_top (root, cancel, 0, &stats);
		g_assert_cmpint (g_chmod (directory, 0700), ==, 0);
		g_assert_true (stats.incomplete);
		g_assert_false (stats.timed_out);
		g_assert_cmpuint (stats.total_size, ==, 320);
		scan_cache_store ("Unreadable", root, 0, entries, &stats);
		ScanResult cached = { 0 };
		g_assert_true (scan_cache_lookup_copy (root, &cached));
		nemo_dir_analyzer_set_entries (analyzer, cached.deep, "Unreadable", 0, &cached.stats);
		g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (label)), "ranking is incomplete"));
		g_clear_pointer (&cached.deep, g_array_unref);
		g_array_unref (entries);
	}

	g_autofree char *missing = g_build_filename (root, "missing", NULL);
	entries = nemo_dir_scan_collect_top (missing, cancel, 0, &stats);
	g_assert_null (entries);
	g_assert_true (stats.incomplete);
	scan_cache_store ("Missing", missing, 0, entries, &stats);
	ScanResult cached = { 0 };
	g_assert_true (scan_cache_lookup_copy (missing, &cached));
	nemo_dir_analyzer_set_entries (analyzer, cached.deep, "Missing", 0, &cached.stats);
	g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (label)), "Incomplete"));
	assert_overview_status (missing, cached.deep, &cached.stats, "Incomplete");

	g_cancellable_cancel (cancel);
	entries = nemo_dir_scan_collect_top (root, cancel, 0, &stats);
	g_assert_true (stats.incomplete);
	g_array_unref (entries);

	remove_tree (root);
	g_assert_cmpint (g_mkdir (root, 0700), ==, 0);
	g_cancellable_reset (cancel);
	entries = nemo_dir_scan_collect_top (root, cancel, 0, &stats);
	g_assert_false (stats.incomplete);
	g_assert_cmpuint (stats.total_size, ==, 0);
	nemo_dir_analyzer_set_entries (analyzer, entries, "Empty", 0, &stats);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (label)), ==, "No files found.");
	g_assert_cmpuint (nemo_dir_analyzer_get_stats (analyzer)->total_size, ==, 0);
	assert_overview_status (root, entries, &stats, "No files found.");
	g_array_unref (entries);

	gtk_widget_destroy (GTK_WIDGET (analyzer));
	g_object_unref (analyzer);
	g_hash_table_remove_all (g_scan_cache);
	remove_tree (root);
}

int
main (int argc, char **argv)
{
	g_assert_cmpstr (g_getenv ("NEMO_TEST_ISOLATED"), ==, "1");
	gtk_test_init (&argc, &argv, NULL);
	g_test_add_func ("/overview/complete-and-cached", test_overview_complete_and_cached);
	g_test_add_func ("/overview/incomplete-and-empty", test_incomplete_and_empty);
	int result = g_test_run ();
	g_clear_pointer (&g_scan_cache, g_hash_table_unref);
	return result;
}
