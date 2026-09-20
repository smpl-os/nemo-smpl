/* Private helper fixture, run with test-sidebar-async.py. */
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <math.h>
#include <time.h>

#define NEMO_SMPL
#define _(s) (s)
#define DEBUG(...) ((void) 0)

typedef struct {
    GObject parent;
    GHashTable *disk_full_cache;
    guint64 disk_full_generation;
    gboolean cache_only_redraw;
    gboolean idle_cache_only;
    guint update_places_on_idle_id;
    guint64 discovery_generation;
    GCancellable *discovery_cancellable;
    guint discovery_timeout_id;
    gint64 discovery_last_finished;
    gboolean home_different_fs;
    GList *portable_mounts;
    GVolumeMonitor *volume_monitor;
    struct _PlacesDiscovery *discovery_pending;
    guint redraws;
} TestSidebar;
typedef GObjectClass TestSidebarClass;
typedef TestSidebar NemoPlacesSidebar;

typedef struct {
    struct _NemoFilesystemQuery *fs_query;
    struct _NemoFilesystemQuery *fs_query_pending;
    guint64 fs_query_generation;
    GVolumeMonitor *fs_volume_monitor;
    gboolean fs_size_cached;
    guint64 free_space;
    time_t free_space_read;
    goffset size;
    gboolean is_gone;
    GFileType type;
    GMount *mount;
} TestDetails;
typedef struct {
    GObject parent;
    TestDetails *details;
    GFile *location;
    guint changes;
} TestFile;
typedef GObjectClass TestFileClass;
typedef TestFile NemoFile;

typedef struct {
    GObject parent;
} TestMount;
typedef GObjectClass TestMountClass;
static GFile *test_mount_root (GMount *mount) { return g_file_new_for_uri ("file:///test/mount"); }
static GVolume *test_mount_volume (GMount *mount) { return NULL; }
static void test_mount_iface_init (GMountIface *iface)
{
    iface->get_root = test_mount_root;
    iface->get_volume = test_mount_volume;
}
G_DEFINE_TYPE_WITH_CODE (TestMount, test_mount, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT, test_mount_iface_init))
static void test_mount_class_init (TestMountClass *klass) {}
static void test_mount_init (TestMount *mount) {}

static void sidebar_async_dispose (NemoPlacesSidebar *sidebar);
static void invalidate_filesystem_query (NemoFile *file);
G_DEFINE_TYPE (TestSidebar, test_sidebar, G_TYPE_OBJECT)
G_DEFINE_TYPE (TestFile, test_file, G_TYPE_OBJECT)

static GPtrArray *tasks;
static gint64 clock_us = 100 * G_USEC_PER_SEC;
static guint dispatched;
static gboolean real_dispatch;
static GThread *main_thread;
static GMutex worker_mutex;
static GCond worker_cond;
static gboolean worker_entered;
static gboolean worker_released;
static GFileInfo *reading (guint64 size, guint64 free_space);
static gint64 real_deadline (void) { return g_get_monotonic_time () + 5 * G_USEC_PER_SEC; }
static gint64 fake_clock (void) { return clock_us; }
static void fake_dispatch (GTask *task, GTaskThreadFunc worker)
{
    dispatched++;
    if (real_dispatch) {
        g_task_run_in_thread (task, worker);
    } else {
        g_ptr_array_add (tasks, g_object_ref (task));
    }
}
static GList *fake_unix_mounts (guint64 *time_read) { return NULL; }
static GList *fake_monitor_mounts (GVolumeMonitor *monitor)
{
    g_assert_true (g_thread_self () == main_thread);
    return NULL;
}
static GFileInfo *fake_filesystem_info (GFile *file, const gchar *attributes,
                                       GCancellable *cancellable, GError **error)
{
    g_assert_true (g_thread_self () != main_thread);
    g_mutex_lock (&worker_mutex);
    worker_entered = TRUE;
    g_cond_signal (&worker_cond);
    while (!worker_released) {
        g_assert_true (g_cond_wait_until (&worker_cond, &worker_mutex, real_deadline ()));
    }
    g_mutex_unlock (&worker_mutex);
    /* Deliberately ignore cancellation, like a wedged native syscall. */
    return reading (100, 20);
}
#define g_get_monotonic_time fake_clock
#define g_task_run_in_thread fake_dispatch
#define g_unix_mounts_get fake_unix_mounts
#define g_volume_monitor_get_mounts fake_monitor_mounts
#define g_file_query_filesystem_info fake_filesystem_info
static int nemo_global_preferences_get_size_prefix_preference (void) { return 0; }
static gchar *nemo_get_home_directory_uri (void) { return g_strdup ("file:///test/home"); }
static GFile *nemo_file_get_location (NemoFile *file) { return g_object_ref (file->location); }
static void nemo_file_unref (NemoFile *file) { g_object_unref (file); }
static void nemo_file_emit_changed (NemoFile *file) { file->changes++; }
static void redraw_places_on_idle (NemoPlacesSidebar *sidebar);
static void update_places_on_idle (NemoPlacesSidebar *sidebar);
static void update_places (NemoPlacesSidebar *sidebar);

#include "sidebar-production.inc"

static void test_sidebar_dispose (GObject *object)
{
    TestSidebar *sidebar = (TestSidebar *) object;
    g_clear_handle_id (&sidebar->update_places_on_idle_id, g_source_remove);
    sidebar_async_dispose (sidebar);
    G_OBJECT_CLASS (test_sidebar_parent_class)->dispose (object);
}
static void test_sidebar_class_init (TestSidebarClass *klass)
{
    klass->dispose = test_sidebar_dispose;
}
static void test_sidebar_init (TestSidebar *sidebar)
{
    sidebar->disk_full_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                     g_free, disk_full_entry_free);
}
static void test_file_finalize (GObject *object)
{
    TestFile *file = (TestFile *) object;
    invalidate_filesystem_query (file);
    g_clear_object (&file->details->fs_volume_monitor);
    g_object_unref (file->location);
    g_free (file->details);
    G_OBJECT_CLASS (test_file_parent_class)->finalize (object);
}
static void test_file_class_init (TestFileClass *klass) { klass->finalize = test_file_finalize; }
static void test_file_init (TestFile *file)
{
    file->details = g_new0 (TestDetails, 1);
    file->details->free_space = (guint64) -1;
    file->details->size = -1;
    /* No real volume monitor or bus is involved. */
    file->details->fs_volume_monitor = (GVolumeMonitor *) g_object_new (G_TYPE_OBJECT, NULL);
    file->location = g_file_new_for_uri ("file:///test/mount");
}
static void drain (void)
{
    while (g_main_context_iteration (NULL, FALSE)) {}
}
static GFileInfo *reading (guint64 size, guint64 free_space)
{
    GFileInfo *info = g_file_info_new ();
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, size);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, free_space);
    return info;
}
static void complete (GFileInfo *info, gint error_code)
{
    GTask *task = g_ptr_array_steal_index (tasks, 0);
    if (error_code >= 0) {
        g_task_return_new_error (task, G_IO_ERROR, error_code, "fixture error");
    } else {
        g_task_return_pointer (task, info, g_object_unref);
    }
    g_object_unref (task);
    drain ();
}
static gint request (TestSidebar *sidebar, const gchar *uri)
{
    GFile *file = g_file_new_for_uri (uri);
    gchar *tooltip;
    gint percent = get_disk_full (sidebar, file, &tooltip);
    g_free (tooltip);
    g_object_unref (file);
    return percent;
}
static DiskFullEntry *entry (TestSidebar *sidebar, const gchar *uri)
{
    return g_hash_table_lookup (sidebar->disk_full_cache, uri);
}
static void update_places (NemoPlacesSidebar *sidebar)
{
    GHashTableIter iter;
    gpointer key;
    sidebar->redraws++;
    /* Replay the real row-reader path for every mount on each redraw. */
    g_hash_table_iter_init (&iter, sidebar->disk_full_cache);
    while (g_hash_table_iter_next (&iter, &key, NULL)) {
        request (sidebar, key);
    }
}

static void test_replacement (void)
{
    TestSidebar *sidebar = g_object_new (test_sidebar_get_type (), NULL);
    GMount *mount = g_object_new (test_mount_get_type (), NULL);
    const gchar *uri = "file:///test/mount";
    request (sidebar, uri);
    guint64 generation = entry (sidebar, uri)->generation;
    invalidate_mount_readings (sidebar, mount);
    request (sidebar, uri);
    g_assert_cmpuint (entry (sidebar, uri)->generation, !=, generation);
    g_assert_cmpuint (tasks->len, ==, 1);
    complete (reading (100, 90), -1);
    g_assert_cmpint (entry (sidebar, uri)->percent, ==, -1);
    g_assert_cmpuint (tasks->len, ==, 1); /* One pending replacement request. */
    g_assert_cmpint (entry (sidebar, uri)->last_finished, ==, 0);
    complete (reading (100, 20), -1);
    g_assert_cmpint (entry (sidebar, uri)->percent, ==, 80);
    g_object_unref (mount);
    g_object_unref (sidebar);
}

static void test_disposal_and_deadline (void)
{
    TestSidebar *sidebar = g_object_new (test_sidebar_get_type (), NULL);
    GWeakRef weak;
    g_weak_ref_init (&weak, sidebar);
    request (sidebar, "file:///test/mount");
    DiskFullEntry *value = entry (sidebar, "file:///test/mount");
    g_clear_handle_id (&value->timeout_id, g_source_remove);
    disk_full_query_timeout_cb (value);
    for (guint i = 0; i < 100; i++) {
        clock_us += 3 * G_USEC_PER_SEC;
        request (sidebar, "file:///test/mount");
    }
    g_assert_cmpuint (tasks->len, ==, 1);
    g_assert_true (value->in_flight);
    g_object_unref (sidebar);
    g_assert_null (g_weak_ref_get (&weak));
    g_weak_ref_clear (&weak);
    complete (reading (100, 90), -1);
    g_assert_null (disk_full_queries);

    sidebar = g_object_new (test_sidebar_get_type (), NULL);
    request (sidebar, "file:///test/mount");
    g_object_run_dispose (G_OBJECT (sidebar));
    complete (reading (100, 90), -1);
    g_assert_cmpuint (sidebar->redraws, ==, 0);
    g_object_unref (sidebar);
}

static void test_slow_mounts (void)
{
    TestSidebar *sidebar = g_object_new (test_sidebar_get_type (), NULL);
    guint before = dispatched;
    request (sidebar, "file:///test/slow3");
    request (sidebar, "file:///test/slow6");
    clock_us += 3 * G_USEC_PER_SEC;
    complete (reading (100, 90), -1);
    clock_us += 3 * G_USEC_PER_SEC;
    complete (reading (100, 80), -1);
    clock_us += 49 * G_USEC_PER_SEC;
    drain ();
    g_assert_cmpuint (dispatched - before, ==, 2);
    g_assert_cmpuint (sidebar->redraws, ==, 2);
    g_assert_cmpuint (tasks->len, ==, 0);
    update_places_on_idle (sidebar); /* A real external update may refresh. */
    drain ();
    g_assert_cmpuint (tasks->len, ==, 2);
    complete (reading (100, 70), -1);
    complete (reading (100, 60), -1);
    g_assert_cmpuint (tasks->len, ==, 0);
    g_object_unref (sidebar);
}

static void test_bad_info_and_errors (void)
{
    TestSidebar *sidebar = g_object_new (test_sidebar_get_type (), NULL);
    const gchar *uri = "file:///test/mount";
    request (sidebar, uri);
    complete (reading (100, 40), -1); /* Derive used when absent. */
    g_assert_cmpint (entry (sidebar, uri)->percent, ==, 60);
    clock_us += 3 * G_USEC_PER_SEC;
    request (sidebar, uri);
    complete (NULL, G_IO_ERROR_FAILED);
    g_assert_cmpint (entry (sidebar, uri)->percent, ==, 60);
    for (guint i = 0; i < 5; i++) {
        GFileInfo *info = reading (100, 30);
        if (i == 0) g_file_info_remove_attribute (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        if (i == 1) g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, "bad");
        if (i == 2) g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, 0);
        if (i == 3) g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, 101);
        if (i == 4) g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_USED, 101);
        clock_us += 3 * G_USEC_PER_SEC;
        request (sidebar, uri);
        complete (info, -1);
        g_assert_cmpint (entry (sidebar, uri)->percent, ==, -1);
    }
    g_object_unref (sidebar);
}

static void test_global_bound (void)
{
    for (guint i = 0; i < 40; i++) {
        TestSidebar *sidebar = g_object_new (test_sidebar_get_type (), NULL);
        gchar *uri = g_strdup_printf ("file:///test/bound%u", i);
        request (sidebar, uri);
        g_free (uri);
        g_object_unref (sidebar);
    }
    g_assert_cmpuint (tasks->len, ==, DISK_FULL_MAX_QUERIES);
    while (tasks->len != 0) complete (reading (100, 40), -1);
    g_assert_null (disk_full_queries);
}

static void test_pending_windows (void)
{
    TestSidebar *one = g_object_new (test_sidebar_get_type (), NULL);
    TestSidebar *two = g_object_new (test_sidebar_get_type (), NULL);
    request (one, "file:///test/same");
    request (two, "file:///test/same");
    g_assert_cmpuint (tasks->len, ==, 1);
    complete (reading (100, 30), -1);
    g_assert_cmpuint (tasks->len, ==, 1);
    complete (reading (100, 30), -1);
    g_assert_cmpint (entry (two, "file:///test/same")->percent, ==, 70);
    g_assert_null (disk_full_waiters);
    for (guint i = 0; i < 12; i++) {
        gchar *uri = g_strdup_printf ("file:///test/queued%u", i);
        request (one, uri);
        g_free (uri);
    }
    g_assert_cmpuint (tasks->len, ==, DISK_FULL_MAX_QUERIES);
    g_assert_cmpuint (g_list_length (disk_full_waiters), ==, 4);
    guint finished = 0;
    while (tasks->len > 0) {
        complete (reading (100, 30), -1);
        g_assert_cmpuint (tasks->len, <=, DISK_FULL_MAX_QUERIES);
        finished++;
    }
    g_assert_cmpuint (finished, ==, 12);
    g_assert_null (disk_full_waiters);
    g_object_unref (one);
    g_object_unref (two);
}

static void complete_discovery (gboolean home_different)
{
    GTask *task = g_ptr_array_steal_index (tasks, 0);
    PlacesDiscovery *query = g_task_get_task_data (task);
    query->home_different_fs = home_different;
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
    drain ();
}

static void test_discovery (void)
{
    TestSidebar *sidebar = g_object_new (test_sidebar_get_type (), NULL);
    GMount *mount = g_object_new (test_mount_get_type (), NULL);
    start_places_discovery (sidebar);
    invalidate_mount_readings (sidebar, mount);
    start_places_discovery (sidebar);
    g_assert_cmpuint (tasks->len, ==, 1);
    g_assert_nonnull (sidebar->discovery_pending);
    complete_discovery (TRUE);
    g_assert_false (sidebar->home_different_fs);
    g_assert_cmpuint (tasks->len, ==, 1);
    complete_discovery (TRUE);
    g_assert_true (sidebar->home_different_fs);
    g_assert_cmpuint (tasks->len, ==, 0);
    g_assert_cmpuint (sidebar->redraws, ==, 1);
    g_object_unref (mount);
    g_object_unref (sidebar);

    TestSidebar *windows[4];
    for (guint i = 0; i < 4; i++) {
        windows[i] = g_object_new (test_sidebar_get_type (), NULL);
        start_places_discovery (windows[i]);
    }
    g_assert_cmpuint (tasks->len, ==, 2);
    g_assert_cmpuint (g_list_length (places_discovery_waiters), ==, 2);
    g_object_unref (windows[2]); /* A queued weak owner may disappear. */
    g_object_unref (windows[0]); /* So may an active owner. */
    complete_discovery (TRUE);
    g_assert_cmpuint (tasks->len, ==, 2);
    complete_discovery (TRUE);
    complete_discovery (TRUE);
    g_assert_true (windows[3]->home_different_fs);
    g_assert_null (places_discovery_waiters);
    g_assert_cmpuint (places_discovery_workers, ==, 0);
    g_object_unref (windows[1]);
    g_object_unref (windows[3]);
}

static void test_discovered_usage (void)
{
    TestSidebar *sidebar = g_object_new (test_sidebar_get_type (), NULL);
    GMount *mount = g_object_new (test_mount_get_type (), NULL);
    start_places_discovery (sidebar);
    PlacesDiscovery *query = g_task_get_task_data (g_ptr_array_index (tasks, 0));
    query->mounts = g_list_prepend (NULL, g_object_ref (mount));
    complete_discovery (FALSE);
    g_assert_cmpuint (tasks->len, ==, 1);
    complete (reading (100, 40), -1);
    g_assert_cmpint (entry (sidebar, "file:///test/mount")->percent, ==, 60);
    clock_us += 3 * G_USEC_PER_SEC;
    start_places_discovery (sidebar);
    query = g_task_get_task_data (g_ptr_array_index (tasks, 0));
    query->mounts = g_list_prepend (NULL, g_object_ref (mount));
    complete_discovery (FALSE);
    g_assert_cmpuint (tasks->len, ==, 0);
    g_object_unref (mount);
    g_object_unref (sidebar);
}

static void test_file_lifetime (void)
{
    TestFile *file = g_object_new (test_file_get_type (), NULL);
    GWeakRef weak;
    g_weak_ref_init (&weak, file);
    start_filesystem_query (file);
    g_clear_handle_id (&file->details->fs_query->timeout_id, g_source_remove);
    filesystem_query_timeout (file->details->fs_query);
    for (guint i = 0; i < 100; i++) start_filesystem_query (file);
    g_assert_cmpuint (tasks->len, ==, 1);
    g_object_unref (file);
    g_assert_null (g_weak_ref_get (&weak));
    g_weak_ref_clear (&weak);
    complete (reading (100, 90), -1);
    g_assert_null (filesystem_queries);
}

static void test_file_generation (void)
{
    TestFile *file = g_object_new (test_file_get_type (), NULL);
    GMount *mount = g_object_new (test_mount_get_type (), NULL);
    file->details->type = G_FILE_TYPE_MOUNTABLE;
    start_filesystem_query (file);
    complete (reading (100, 90), -1);
    g_assert_cmpuint (file->details->free_space, ==, 90);
    g_assert_cmpint (file->details->size, ==, 100);
    start_filesystem_query (file);
    filesystem_mount_changed (NULL, mount, file);
    start_filesystem_query (file); /* Requested during old native syscall. */
    g_assert_nonnull (file->details->fs_query_pending);
    g_assert_cmpuint (tasks->len, ==, 1);
    g_assert_cmpint (file->details->size, ==, -1);
    complete (reading (100, 20), -1);
    g_assert_cmpuint (file->details->free_space, ==, (guint64) -1);
    g_assert_cmpuint (tasks->len, ==, 1); /* Pending external demand resumes. */
    complete (reading (200, 180), -1);
    g_assert_cmpuint (file->details->free_space, ==, 180);
    start_filesystem_query (file);
    complete (NULL, G_IO_ERROR_FAILED);
    g_assert_cmpuint (file->details->free_space, ==, 180);
    start_filesystem_query (file);
    complete (g_file_info_new (), -1);
    g_assert_cmpuint (file->details->free_space, ==, (guint64) -1);
    g_assert_cmpint (file->details->size, ==, -1);
    g_object_unref (mount);
    g_object_unref (file);
}

static void test_file_saturation (void)
{
    TestFile *files[12];
    for (guint i = 0; i < 12; i++) {
        files[i] = g_object_new (test_file_get_type (), NULL);
        gchar *uri = g_strdup_printf ("file:///test/queued%u", i);
        g_object_unref (files[i]->location);
        files[i]->location = g_file_new_for_uri (uri);
        g_free (uri);
        start_filesystem_query (files[i]);
    }
    g_assert_cmpuint (tasks->len, ==, 8);
    g_assert_cmpuint (g_list_length (filesystem_query_waiters), ==, 4);
    guint finished = 0;
    while (tasks->len > 0) {
        complete (reading (100, 30), -1);
        g_assert_cmpuint (tasks->len, <=, 8);
        finished++;
    }
    g_assert_cmpuint (finished, ==, 12);
    for (guint i = 0; i < 12; i++) {
        g_assert_cmpuint (files[i]->details->free_space, ==, 30);
        g_object_unref (files[i]);
    }
    g_assert_null (filesystem_query_waiters);
}

static void test_mount_snapshot (void)
{
    GFile *location = g_file_new_for_uri ("computer:///fixture");
    GFile *root = g_file_new_for_uri ("file:///test/snapshot");
    GHashTable *snapshot = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
    GFileInfo *info = g_file_info_new ();
    g_hash_table_insert (snapshot, g_strdup ("/dev/fixture-disk"), g_object_ref (root));
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_MOUNTABLE_UNIX_DEVICE_FILE, "/dev/fixture-disk");
    GFile *resolved = resolve_computer_target (location, info, snapshot);
    g_assert_true (g_file_equal (resolved, root));
    g_object_unref (resolved);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_MOUNTABLE_UNIX_DEVICE_FILE, 42);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_TARGET_URI, 42);
    resolved = resolve_computer_target (location, info, snapshot);
    g_assert_true (g_file_equal (resolved, location));
    g_object_unref (resolved);
    g_object_unref (info);
    g_hash_table_unref (snapshot);
    g_object_unref (root);
    g_object_unref (location);
}

static void test_native_worker_deadline (void)
{
    TestSidebar *sidebar = g_object_new (test_sidebar_get_type (), NULL);
    GMount *mount = g_object_new (test_mount_get_type (), NULL);
    real_dispatch = TRUE;
    worker_entered = worker_released = FALSE;
    guint before = dispatched;
    request (sidebar, "file:///test/mount");
    g_mutex_lock (&worker_mutex);
    while (!worker_entered) {
        g_assert_true (g_cond_wait_until (&worker_cond, &worker_mutex, real_deadline ()));
    }
    g_mutex_unlock (&worker_mutex);
    DiskFullEntry *value = entry (sidebar, "file:///test/mount");
    g_clear_handle_id (&value->timeout_id, g_source_remove);
    disk_full_query_timeout_cb (value);
    for (guint i = 0; i < 50; i++) {
        invalidate_mount_readings (sidebar, mount);
        request (sidebar, "file:///test/mount");
        drain ();
    }
    g_assert_cmpuint (dispatched - before, ==, 1);
    g_assert_cmpuint (g_list_length (disk_full_queries), ==, 1);
    g_assert_cmpuint (g_list_length (disk_full_waiters), ==, 1);
    g_object_unref (sidebar);
    g_assert_null (disk_full_waiters);
    g_mutex_lock (&worker_mutex);
    worker_released = TRUE;
    g_cond_signal (&worker_cond);
    g_mutex_unlock (&worker_mutex);
    while (disk_full_queries != NULL) {
        g_main_context_iteration (NULL, TRUE);
    }
    real_dispatch = FALSE;
    g_object_unref (mount);
}

int main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    tasks = g_ptr_array_new_with_free_func (g_object_unref);
    main_thread = g_thread_self ();
    g_test_add_func ("/sidebar/replacement", test_replacement);
    g_test_add_func ("/sidebar/disposal-deadline", test_disposal_and_deadline);
    g_test_add_func ("/sidebar/slow-mounts", test_slow_mounts);
    g_test_add_func ("/sidebar/bad-info-errors", test_bad_info_and_errors);
    g_test_add_func ("/sidebar/global-bound", test_global_bound);
    g_test_add_func ("/sidebar/pending-windows", test_pending_windows);
    g_test_add_func ("/sidebar/discovery", test_discovery);
    g_test_add_func ("/sidebar/discovered-usage", test_discovered_usage);
    g_test_add_func ("/sidebar/native-worker-deadline", test_native_worker_deadline);
    g_test_add_func ("/filesystem/lifetime", test_file_lifetime);
    g_test_add_func ("/filesystem/generation", test_file_generation);
    g_test_add_func ("/filesystem/saturation", test_file_saturation);
    g_test_add_func ("/filesystem/mount-snapshot", test_mount_snapshot);
    int result = g_test_run ();
    g_assert_cmpuint (tasks->len, ==, 0);
    g_ptr_array_unref (tasks);
    return result;
}
