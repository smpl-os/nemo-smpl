#include <config.h>
#include <glib/gstdio.h>
#include <sys/resource.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <linux/ioprio.h>
#endif
#include <libnemo-private/nemo-global-preferences.h>
#include "../src/nemo-disk-treemap.c"
#include "../src/nemo-overview.c"

static gboolean advance_clock;
static gint64 scan_clock;
static const char *foreign_path;
static GMutex gate_mutex;
static GCond gate_cond;
static const char *gate_path;
static gboolean gate_entered, gate_released;
#ifdef __linux__
static int scan_nice, scan_io;
#endif
static GFile *opened_location, *opened_selection;

static int
intercept_stat (const char *path, struct stat *st, int result)
{
	if (result == 0 && g_strcmp0 (path, foreign_path) == 0)
		st->st_dev++;
	g_mutex_lock (&gate_mutex);
	if (g_strcmp0 (path, gate_path) == 0 && !gate_entered) {
#ifdef __linux__
		scan_nice = getpriority (PRIO_PROCESS, 0);
		scan_io = syscall (SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0);
#endif
		gate_entered = TRUE;
		while (!gate_released)
			g_cond_wait (&gate_cond, &gate_mutex);
	}
	g_mutex_unlock (&gate_mutex);
	return result;
}

/* Advance scan time without slowing down filesystem fixtures. */
gint64 __real_g_get_monotonic_time (void);
int __real_lstat (const char *path, struct stat *st);
int __real_lstat64 (const char *path, struct stat *st);

int
__wrap_lstat (const char *path, struct stat *st)
{
	if (advance_clock)
		scan_clock += G_USEC_PER_SEC;
	return intercept_stat (path, st, __real_lstat (path, st));
}

int
__wrap_lstat64 (const char *path, struct stat *st)
{
	if (advance_clock)
		scan_clock += G_USEC_PER_SEC;
	return intercept_stat (path, st, __real_lstat64 (path, st));
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
GType nemo_window_slot_get_type (void) { return GTK_TYPE_GRID; }
void nemo_window_slot_make_hosting_pane_active (NemoWindowSlot *slot)
{
	g_assert_not_reached ();
}
NemoWindowSlot *nemo_window_get_active_slot (NemoWindow *window)
{
	return (NemoWindowSlot *) window;
}
void nemo_window_slot_open_location_full (NemoWindowSlot *slot, GFile *location,
	NemoWindowOpenFlags flags, GList *selection, NemoWindowGoToCallback callback,
	gpointer data)
{
	g_assert_null (opened_location);
	opened_location = g_object_ref (location);
	if (selection != NULL) {
		g_assert_cmpuint (g_list_length (selection), ==, 1);
		opened_selection = nemo_file_get_location (selection->data);
	}
}

#define WAIT_UNTIL(condition) G_STMT_START { \
	gint64 deadline = __real_g_get_monotonic_time () + 10 * G_USEC_PER_SEC; \
	while (!(condition) && __real_g_get_monotonic_time () < deadline) { \
		while (g_main_context_iteration (NULL, FALSE)); \
		g_usleep (1000); \
	} \
	g_assert_true (condition); \
} G_STMT_END

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

static void
assert_tree_totals (NemoDirScanNode *node)
{
	g_autofree char *path = nemo_dir_scan_node_get_path (node);
	g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
	if (node->children == NULL) {
		g_assert_cmpuint (node->files, ==, 1);
		return;
	}
	guint64 size = 0, files = 0, previous = G_MAXUINT64;
	for (guint i = 0; i < node->children->len; i++) {
		NemoDirScanNode *child = g_ptr_array_index (node->children, i);
		g_assert_true (child->parent == node);
		g_assert_cmpuint (child->size, <=, previous);
		previous = child->size;
		size += child->size;
		files += child->files;
		assert_tree_totals (child);
	}
	g_assert_cmpuint (size, ==, node->size);
	g_assert_cmpuint (files, ==, node->files);
}

static void
test_heatmap_tree (void)
{
	g_autofree char *root = make_tree ();
	g_autofree char *invalid = g_build_filename (root, "invalid-\xff.jpg", NULL);
	g_autofree char *empty = g_build_filename (root, "empty", NULL);
	g_autofree char *hardlink = g_build_filename (root, "hardlink", NULL);
	g_autofree char *source = g_build_filename (root, "home", "nested", "data", NULL);
	g_autofree char *fifo = g_build_filename (root, "fifo", NULL);
	g_autofree char *other = g_build_filename (root, "other-filesystem", NULL);
	g_autofree char *other_file = g_build_filename (other, "excluded", NULL);
	g_assert_true (g_file_set_contents (invalid, "1234567", 7, NULL));
	g_assert_true (g_file_set_contents (empty, "", 0, NULL));
	g_assert_cmpint (link (source, hardlink), ==, 0);
	g_assert_cmpint (mkfifo (fifo, 0600), ==, 0);
	g_assert_cmpint (g_mkdir (other, 0700), ==, 0);
	g_assert_true (g_file_set_contents (other_file, "must not count", 14, NULL));
	foreign_path = other;
	NemoDirScanStats stats;
	GError *error = NULL;
	g_autoptr (GCancellable) cancel = g_cancellable_new ();
	advance_clock = TRUE;
	scan_clock = 0;
	NemoDirScanNode *tree = nemo_dir_scan_collect_tree (root, cancel, &stats, &error);
	advance_clock = FALSE;
	g_assert_no_error (error);
	g_assert_nonnull (tree);
	g_assert_cmpint (scan_clock, >, 12 * G_USEC_PER_SEC);
	g_assert_false (stats.incomplete);
	g_assert_false (stats.timed_out);
	g_assert_cmpuint (stats.total_size, ==, 367);
	g_assert_cmpuint (tree->size, ==, 367);
	g_assert_cmpuint (tree->files, ==, 36);
	g_assert_cmpuint (tree->children->len, ==, 36);
	assert_tree_totals (tree);
	GArray *ranked = nemo_dir_scan_collect_top (root, cancel, 0, &stats);
	g_assert_cmpuint (stats.total_size, ==, tree->size);
	g_array_unref (ranked);
	nemo_dir_scan_node_free (tree);
	foreign_path = NULL;
	remove_tree (root);
}

static void
test_heatmap_errors (void)
{
	g_autofree char *root = make_tree ();
	g_autofree char *missing = g_build_filename (root, "missing", NULL);
	g_autofree char *file = g_build_filename (root, "home", "nested", "data", NULL);
	g_autoptr (GCancellable) cancel = g_cancellable_new ();
	NemoDirScanStats stats;
	GError *error = NULL;
	g_assert_null (nemo_dir_scan_collect_tree (missing, cancel, &stats, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	g_clear_error (&error);
	g_assert_null (nemo_dir_scan_collect_tree (file, cancel, &stats, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY);
	g_clear_error (&error);
	g_cancellable_cancel (cancel);
	g_assert_null (nemo_dir_scan_collect_tree (root, cancel, &stats, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error (&error);
	g_cancellable_reset (cancel);
	if (getuid () != 0) {
		g_autofree char *folder = g_build_filename (root, "home", NULL);
		g_assert_cmpint (g_chmod (folder, 0000), ==, 0);
		NemoDirScanNode *tree = nemo_dir_scan_collect_tree (root, cancel, &stats, &error);
		g_assert_cmpint (g_chmod (folder, 0700), ==, 0);
		g_assert_no_error (error);
		g_assert_true (stats.incomplete);
		g_assert_cmpuint (stats.total_size, ==, 320);
		assert_tree_totals (tree);
		nemo_dir_scan_node_free (tree);
	}
	NemoDiskTreemap *map = NEMO_DISK_TREEMAP (g_object_ref_sink (nemo_disk_treemap_new ()));
	nemo_disk_treemap_scan (map, missing);
	WAIT_UNTIL (!nemo_disk_treemap_is_loading (map));
	g_assert_false (nemo_disk_treemap_has_result (map));
	g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (map->status)), "Cannot measure"));
	g_assert_cmpint (g_mkdir (missing, 0700), ==, 0);
	gtk_button_clicked (GTK_BUTTON (map->refresh));
	WAIT_UNTIL (!nemo_disk_treemap_is_loading (map));
	g_assert_true (nemo_disk_treemap_has_result (map));
	g_assert_cmpuint (map->tree->size, ==, 0);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (map->status)), ==, "0 bytes in 0 files");
	gtk_widget_destroy (GTK_WIDGET (map));
	g_object_unref (map);
	remove_tree (root);
}

static void
set_gate (const char *path)
{
	g_mutex_lock (&gate_mutex);
	gate_path = path;
	gate_entered = gate_released = FALSE;
	g_mutex_unlock (&gate_mutex);
}

static gboolean
gate_is_entered (void)
{
	g_mutex_lock (&gate_mutex);
	gboolean entered = gate_entered;
	g_mutex_unlock (&gate_mutex);
	return entered;
}

static void
release_gate (void)
{
	g_mutex_lock (&gate_mutex);
	gate_released = TRUE;
	gate_path = NULL;
	g_cond_signal (&gate_cond);
	g_mutex_unlock (&gate_mutex);
}

static void
test_heatmap_lifecycle (void)
{
#ifdef __linux__
	int ui_nice = getpriority (PRIO_PROCESS, 0);
	int ui_io = syscall (SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0);
#endif
	g_autofree char *first = make_tree ();
	g_autofree char *second = make_tree ();
	NemoDiskTreemap *map = NEMO_DISK_TREEMAP (g_object_ref_sink (nemo_disk_treemap_new ()));
	set_gate (first);
	nemo_disk_treemap_scan (map, first);
	WAIT_UNTIL (gate_is_entered ());
#ifdef __linux__
	g_assert_cmpint (scan_nice, ==, 19);
	g_assert_cmpint (scan_io, >=, 0);
	g_assert_cmpint (IOPRIO_PRIO_CLASS (scan_io), ==, IOPRIO_CLASS_IDLE);
#endif
	gpointer old_cancel = map->cancel;
	g_object_add_weak_pointer (G_OBJECT (old_cancel), &old_cancel);
	nemo_disk_treemap_scan (map, second);
	g_assert_true (g_cancellable_is_cancelled (old_cancel));
	WAIT_UNTIL (!nemo_disk_treemap_is_loading (map));
	NemoDirScanNode *snapshot = map->tree;
	g_assert_nonnull (snapshot);
	g_assert_cmpstr (snapshot->name, ==, second);
	release_gate ();
	WAIT_UNTIL (old_cancel == NULL);
	g_assert_true (map->tree == snapshot);

	set_gate (first);
	nemo_disk_treemap_scan (map, first);
	WAIT_UNTIL (gate_is_entered ());
	gpointer pending_cancel = map->cancel;
	g_object_add_weak_pointer (G_OBJECT (pending_cancel), &pending_cancel);
	gpointer weak_map = map;
	g_object_add_weak_pointer (G_OBJECT (map), &weak_map);
	gtk_widget_destroy (GTK_WIDGET (map));
	g_assert_true (g_cancellable_is_cancelled (pending_cancel));
	g_object_unref (map);
	release_gate ();
	WAIT_UNTIL (weak_map == NULL);
	WAIT_UNTIL (pending_cancel == NULL);
#ifdef __linux__
	g_assert_cmpint (getpriority (PRIO_PROCESS, 0), ==, ui_nice);
	g_assert_cmpint (syscall (SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0), ==, ui_io);
#endif
	remove_tree (first);
	remove_tree (second);
}

static void
test_heatmap_many_small_files (void)
{
	NemoDiskTreemap *map = NEMO_DISK_TREEMAP (g_object_ref_sink (nemo_disk_treemap_new ()));
	NemoDirScanNode *tree = g_new0 (NemoDirScanNode, 1);
	tree->name = g_strdup ("/fixture");
	tree->children = g_ptr_array_new_with_free_func ((GDestroyNotify) nemo_dir_scan_node_free);
	for (guint i = 0; i < 25002; i++) {
		NemoDirScanNode *node = g_new0 (NemoDirScanNode, 1);
		node->parent = tree;
		node->name = i == 0 ? g_strdup ("invalid-\xff.jpg") : g_strdup_printf ("small-%05u", i);
		node->size = i == 0 ? 100000 : i == 25001 ? 0 : 1;
		node->files = 1;
		tree->size += node->size;
		tree->files++;
		g_ptr_array_add (tree->children, node);
	}
	map->tree = tree;
	treemap_zoom (map, tree);
	treemap_layout_children (map, tree, 0, 0, 1200, 700, 0);
	g_assert_cmpuint (map->rects->len, <, 3000);
	guint64 bytes = 0;
	guint items = 0;
	double area = 0;
	gboolean grouped = FALSE;
	gint group_index = -1;
	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 1200, 700);
	cairo_t *cr = cairo_create (surface);
	PangoLayout *layout = gtk_widget_create_pango_layout (map->area, NULL);
	for (guint i = 0; i < map->rects->len; i++) {
		TreemapRect rect = g_array_index (map->rects, TreemapRect, i);
		bytes += rect.size;
		items += rect.items;
		area += rect.width * rect.height;
		grouped |= rect.items > 1;
		if (rect.items > 1)
			group_index = i;
		if (i == 0)
			g_assert_cmpint (rect.kind, ==, NEMO_FILE_COLOR_IMAGE);
		treemap_paint_rect (map, cr, layout, rect, FALSE);
	}
	g_assert_true (grouped);
	g_assert_cmpuint (bytes, ==, 125000);
	g_assert_cmpuint (items, ==, 25001);
	g_assert_cmpfloat_with_epsilon (area, 1200 * 700, 0.01);
	TreemapRect *group = &g_array_index (map->rects, TreemapRect, group_index);
	map->selected = group->node;
	map->selected_rect = group_index;
	treemap_update_labels (map);
	g_autofree char *group_size = g_format_size (group->size);
	g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (map->details)), group_size));
	g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (map->details)), "small items"));
	map->selected = g_ptr_array_index (tree->children, 0);
	map->selected_rect = -1;
	treemap_update_labels (map);
	g_assert_true (g_utf8_validate (gtk_label_get_text (GTK_LABEL (map->details)), -1, NULL));
	g_object_unref (layout);
	cairo_destroy (cr);
	cairo_surface_destroy (surface);
	gtk_widget_destroy (GTK_WIDGET (map));
	g_object_unref (map);
}

static void
add_fixture_volume (NemoOverview *overview, const char *path)
{
	VolumeInfo volume = {
		.name = g_strdup ("Fixture"),
		.mount_path = g_strdup (path),
		.device = g_strdup ("fixture"),
		.fs_type = g_strdup ("fixture"),
		.total = 1000, .used = 500, .free_bytes = 500, .fraction = 0.5
	};
	g_array_append_val (overview->volumes, volume);
}

static void
map_key (NemoDiskTreemap *map, guint key)
{
	GdkEvent *event = gdk_event_new (GDK_KEY_PRESS);
	event->key.window = g_object_ref (gtk_widget_get_window (map->area));
	event->key.keyval = key;
	gboolean handled = gtk_widget_event (map->area, event);
	g_assert_true (handled);
	gdk_event_free (event);
}

static void
test_heatmap_overview (void)
{
	g_autofree char *first = make_tree ();
	g_autofree char *second = make_tree ();
	NemoOverview *overview = g_object_new (NEMO_TYPE_OVERVIEW, NULL);
	GtkWidget *window = g_object_ref_sink (gtk_window_new (GTK_WINDOW_TOPLEVEL));
	gtk_window_set_default_size (GTK_WINDOW (window), 1200, 900);
	gtk_container_add (GTK_CONTAINER (window), GTK_WIDGET (overview));
	add_fixture_volume (overview, first);
	add_fixture_volume (overview, second);
	rebuild_ui (overview);
	add_pareto_placeholder (overview, "Fixture", first);
	heatmap_sync_volumes (overview);
	gtk_widget_show_all (window);
	g_assert_cmpstr (gtk_stack_get_visible_child_name (GTK_STACK (overview->stack)), ==, "dashboard");
	g_assert_null (nemo_disk_treemap_get_path (overview->treemap));
	GtkWidget *dashboard = overview->dashboard;
	GtkWidget *pareto = gtk_widget_get_parent (find_widget (overview->pareto_box, GTK_TYPE_LABEL));
	set_gate (first);
	gtk_stack_set_visible_child_name (GTK_STACK (overview->stack), "heatmap");
	WAIT_UNTIL (gate_is_entered ());
	gpointer cancelled = overview->treemap->cancel;
	g_object_add_weak_pointer (G_OBJECT (cancelled), &cancelled);
	gtk_stack_set_visible_child_name (GTK_STACK (overview->stack), "dashboard");
	g_assert_true (g_cancellable_is_cancelled (cancelled));
	release_gate ();
	WAIT_UNTIL (cancelled == NULL);
	g_assert_false (nemo_disk_treemap_has_result (overview->treemap));
	gtk_stack_set_visible_child_name (GTK_STACK (overview->stack), "heatmap");
	NemoDiskTreemap *map = overview->treemap;
	WAIT_UNTIL (!nemo_disk_treemap_is_loading (map));
	WAIT_UNTIL (map->rects->len > 0);
	g_assert_cmpint (gtk_widget_get_allocated_height (map->area), >,
	                 gtk_widget_get_allocated_height (window) / 2);
	g_assert_cmpuint (map->tree->size, ==, 340);
	g_assert_false (gtk_widget_get_mapped (dashboard));
	g_assert_true (gtk_widget_get_mapped (map->area));
	double area = (gtk_widget_get_allocated_width (map->area) - 4.0) *
	              (gtk_widget_get_allocated_height (map->area) - 4.0);
	double covered = 0;
	guint outer_count = 0;
	NemoDirScanNode *home = NULL;
	for (guint i = 0; i < map->rects->len; i++) {
		TreemapRect *rect = &g_array_index (map->rects, TreemapRect, i);
		g_assert_cmpfloat (rect->width, >, 0);
		g_assert_cmpfloat (rect->height, >, 0);
		if (rect->node->parent == map->tree && rect->items == 1) {
			covered += rect->width * rect->height;
			outer_count++;
			g_assert_cmpfloat_with_epsilon (rect->width * rect->height / area,
			                               (double) rect->size / 340, 0.000001);
			if (g_str_equal (rect->node->name, "home")) {
				home = rect->node;
				g_assert_true (treemap_hit (map, rect->x + 2, rect->y + 2)->node == home);
			}
		}
	}
	g_assert_cmpuint (outer_count, ==, 33);
	g_assert_cmpfloat_with_epsilon (covered, area, 0.01);
	g_assert_nonnull (home);
	NemoDirScanNode *snapshot = map->tree;
	gtk_stack_set_visible_child_name (GTK_STACK (overview->stack), "dashboard");
	g_assert_true (overview->dashboard == dashboard);
	g_assert_true (gtk_widget_get_parent (find_widget (overview->pareto_box, GTK_TYPE_LABEL)) == pareto);
	gtk_stack_set_visible_child_name (GTK_STACK (overview->stack), "heatmap");
	g_assert_true (map->tree == snapshot);
	g_assert_false (nemo_disk_treemap_is_loading (map));

	treemap_zoom (map, home);
	g_assert_cmpuint (map->current->size, ==, 20);
	gtk_button_clicked (GTK_BUTTON (map->open));
	g_autofree char *home_path = g_build_filename (first, "home", NULL);
	g_autoptr (GFile) expected = g_file_new_for_path (home_path);
	g_assert_true (g_file_equal (opened_location, expected));
	g_assert_null (opened_selection);
	g_clear_object (&opened_location);
	map->selected = g_ptr_array_index (((NemoDirScanNode *) g_ptr_array_index (home->children, 0))->children, 0);
	map_key (map, GDK_KEY_Return);
	g_autofree char *nested = g_build_filename (home_path, "nested", NULL);
	g_autofree char *data = g_build_filename (nested, "data", NULL);
	g_autofree char *actual_location = g_file_get_path (opened_location);
	g_autofree char *actual_selection = g_file_get_path (opened_selection);
	g_assert_cmpstr (actual_location, ==, nested);
	g_assert_cmpstr (actual_selection, ==, data);
	g_clear_object (&opened_location);
	g_clear_object (&opened_selection);
	map_key (map, GDK_KEY_BackSpace);
	g_assert_true (map->current == map->tree);

	gtk_combo_box_set_active (GTK_COMBO_BOX (overview->volume_combo), 1);
	WAIT_UNTIL (!nemo_disk_treemap_is_loading (map));
	g_assert_cmpstr (map->tree->name, ==, second);
	g_array_remove_index (overview->volumes, 1);
	heatmap_sync_volumes (overview);
	WAIT_UNTIL (!nemo_disk_treemap_is_loading (map));
	g_assert_cmpstr (map->tree->name, ==, first);
	g_array_set_size (overview->volumes, 0);
	heatmap_sync_volumes (overview);
	g_assert_false (gtk_widget_get_visible (GTK_WIDGET (map)));
	g_assert_true (gtk_widget_get_visible (overview->heatmap_empty));
	gtk_widget_destroy (window);
	g_object_unref (window);
	remove_tree (first);
	remove_tree (second);
}

int
main (int argc, char **argv)
{
	g_assert_cmpstr (g_getenv ("NEMO_TEST_ISOLATED"), ==, "1");
	gtk_test_init (&argc, &argv, NULL);
	nemo_global_preferences_init ();
	g_test_add_func ("/overview/complete-and-cached", test_overview_complete_and_cached);
	g_test_add_func ("/overview/incomplete-and-empty", test_incomplete_and_empty);
	g_test_add_func ("/overview/heatmap/tree", test_heatmap_tree);
	g_test_add_func ("/overview/heatmap/errors", test_heatmap_errors);
	g_test_add_func ("/overview/heatmap/lifecycle", test_heatmap_lifecycle);
	g_test_add_func ("/overview/heatmap/many-small-files", test_heatmap_many_small_files);
	g_test_add_func ("/overview/heatmap/toggle-layout-and-navigation", test_heatmap_overview);
	int result = g_test_run ();
	g_clear_pointer (&g_scan_cache, g_hash_table_unref);
	return result;
}
