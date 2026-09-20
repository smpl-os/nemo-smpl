#include <config.h>
#include <libnemo-private/nemo-file-utilities.h>

#ifdef NEMO_SMPL
/* A deferred GFile backend keeps cancellation tests independent of real mounts,
 * filesystem timing, a display server, and the user's files. */
typedef struct {
    GObject parent;
    int depth;
    int existing_depth;
} HierarchyFile;

typedef GObjectClass HierarchyFileClass;

static void hierarchy_file_iface_init (GFileIface *iface);

G_DEFINE_TYPE_WITH_CODE (HierarchyFile, hierarchy_file, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE (G_TYPE_FILE, hierarchy_file_iface_init))

static guint query_count;

static void
hierarchy_file_class_init (HierarchyFileClass *klass)
{
}

static void
hierarchy_file_init (HierarchyFile *file)
{
}

static GFile *
hierarchy_file_new (int depth, int existing_depth)
{
    HierarchyFile *file = g_object_new (hierarchy_file_get_type (), NULL);
    file->depth = depth;
    file->existing_depth = existing_depth;
    return G_FILE (file);
}

static GFile *
hierarchy_file_get_parent (GFile *file)
{
    HierarchyFile *self = (HierarchyFile *) file;
    return self->depth > 0 ? hierarchy_file_new (self->depth - 1, self->existing_depth) : NULL;
}

static GFileInfo *
hierarchy_file_query_info (GFile *file, const char *attributes,
                           GFileQueryInfoFlags flags, GCancellable *cancellable,
                           GError **error)
{
    g_assert_not_reached ();
    return NULL;
}

static gboolean
complete_query (gpointer data)
{
    GTask *task = G_TASK (data);
    HierarchyFile *file = g_task_get_source_object (task);

    if (file->depth <= file->existing_depth) {
        GFileInfo *info = g_file_info_new ();
        g_file_info_set_name (info, "ancestor");
        g_task_return_pointer (task, info, g_object_unref);
    } else {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Missing ancestor");
    }
    return G_SOURCE_REMOVE;
}

static void
hierarchy_file_query_info_async (GFile *file, const char *attributes,
                                 GFileQueryInfoFlags flags, int priority,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer data)
{
    GTask *task = g_task_new (file, cancellable, callback, data);
    query_count++;
    g_idle_add_full (priority, complete_query, task, g_object_unref);
}

static GFileInfo *
hierarchy_file_query_info_finish (GFile *file, GAsyncResult *result, GError **error)
{
    g_assert_true (g_task_is_valid (result, file));
    return g_task_propagate_pointer (G_TASK (result), error);
}

static void
hierarchy_file_iface_init (GFileIface *iface)
{
    iface->get_parent = hierarchy_file_get_parent;
    iface->query_info = hierarchy_file_query_info;
    iface->query_info_async = hierarchy_file_query_info_async;
    iface->query_info_finish = hierarchy_file_query_info_finish;
}

typedef struct {
    gboolean done;
    GFile *ancestor;
    GError *error;
    GCancellable *cancel_before_finish;
    GWeakRef owner;
    gboolean check_owner;
    gboolean owner_alive;
} SearchResult;

static void
search_ready (GObject *source, GAsyncResult *result, gpointer data)
{
    SearchResult *search = data;

    if (search->cancel_before_finish != NULL) {
        g_cancellable_cancel (search->cancel_before_finish);
    }
    search->ancestor = nemo_find_existing_uri_in_hierarchy_finish (G_FILE (source), result, &search->error);
    if (search->check_owner) {
        GObject *owner = g_weak_ref_get (&search->owner);
        search->owner_alive = owner != NULL;
        g_clear_object (&owner);
    }
    search->done = TRUE;
}

static void
wait_for_search (SearchResult *search)
{
    while (!search->done) {
        g_main_context_iteration (NULL, TRUE);
    }
}

static gboolean
heartbeat (gpointer data)
{
    *(gboolean *) data = TRUE;
    return G_SOURCE_REMOVE;
}

static void
test_ancestor (gconstpointer data)
{
    int expected_depth = GPOINTER_TO_INT (data);
    GFile *file = hierarchy_file_new (3, expected_depth);
    SearchResult result = { 0 };
    gboolean responsive = FALSE;

    query_count = 0;
    g_idle_add_full (G_PRIORITY_HIGH, heartbeat, &responsive, NULL);
    nemo_find_existing_uri_in_hierarchy_async (file, NULL, search_ready, &result);
    g_object_unref (file);
    g_assert_false (result.done);
    wait_for_search (&result);
    g_assert_true (responsive);
    g_assert_no_error (result.error);
    if (expected_depth >= 0) {
        g_assert_nonnull (result.ancestor);
        g_assert_cmpint (((HierarchyFile *) result.ancestor)->depth, ==, expected_depth);
        g_assert_cmpuint (query_count, ==, 4 - expected_depth);
    } else {
        g_assert_null (result.ancestor);
        g_assert_cmpuint (query_count, ==, 4);
    }
    g_clear_object (&result.ancestor);
}

static void
test_cancellation (gconstpointer data)
{
    int timing = GPOINTER_TO_INT (data);
    GFile *file = hierarchy_file_new (3, 3);
    GCancellable *cancellable = g_cancellable_new ();
    SearchResult result = { 0 };

    query_count = 0;
    if (timing == 0) {
        g_cancellable_cancel (cancellable);
    } else if (timing == 2) {
        result.cancel_before_finish = cancellable;
    }
    nemo_find_existing_uri_in_hierarchy_async (file, cancellable, search_ready, &result);
    if (timing == 1) {
        g_cancellable_cancel (cancellable);
    }
    g_assert_false (result.done);
    wait_for_search (&result);
    g_assert_null (result.ancestor);
    g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint (query_count, ==, timing == 0 ? 0 : 1);
    g_clear_error (&result.error);
    g_object_unref (cancellable);
    g_object_unref (file);
}

static void
test_replaced_search (void)
{
    GFile *file = hierarchy_file_new (3, 1);
    GCancellable *old_cancellable = g_cancellable_new ();
    SearchResult old = { 0 }, current = { 0 };

    nemo_find_existing_uri_in_hierarchy_async (file, old_cancellable, search_ready, &old);
    g_cancellable_cancel (old_cancellable);
    nemo_find_existing_uri_in_hierarchy_async (file, NULL, search_ready, &current);
    wait_for_search (&current);
    wait_for_search (&old);
    g_assert_error (old.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_null (old.ancestor);
    g_assert_no_error (current.error);
    g_assert_cmpint (((HierarchyFile *) current.ancestor)->depth, ==, 1);
    g_clear_error (&old.error);
    g_object_unref (current.ancestor);
    g_object_unref (old_cancellable);
    g_object_unref (file);
}

static void
cancel_on_destroy (gpointer data)
{
    g_cancellable_cancel (data);
    g_object_unref (data);
}

static void
test_destroyed_owner (void)
{
    GFile *file = hierarchy_file_new (3, 1);
    GObject *owner = g_object_new (G_TYPE_OBJECT, NULL);
    GCancellable *cancellable = g_cancellable_new ();
    SearchResult result = { 0 };

    result.check_owner = TRUE;
    g_weak_ref_init (&result.owner, owner);
    g_object_set_data_full (owner, "search", g_object_ref (cancellable), cancel_on_destroy);
    nemo_find_existing_uri_in_hierarchy_async (file, cancellable, search_ready, &result);
    g_object_unref (owner);
    wait_for_search (&result);
    g_assert_false (result.owner_alive);
    g_assert_null (result.ancestor);
    g_assert_error (result.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&result.error);
    g_weak_ref_clear (&result.owner);
    g_object_unref (cancellable);
    g_object_unref (file);
}
#endif

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
#ifdef NEMO_SMPL
    g_test_add_data_func ("/file-hierarchy/existing", GINT_TO_POINTER (3), test_ancestor);
    g_test_add_data_func ("/file-hierarchy/nearest-ancestor", GINT_TO_POINTER (1), test_ancestor);
    g_test_add_data_func ("/file-hierarchy/no-ancestor", GINT_TO_POINTER (-1), test_ancestor);
    g_test_add_data_func ("/file-hierarchy/cancel-before-start", GINT_TO_POINTER (0), test_cancellation);
    g_test_add_data_func ("/file-hierarchy/cancel-pending", GINT_TO_POINTER (1), test_cancellation);
    g_test_add_data_func ("/file-hierarchy/cancel-before-finish", GINT_TO_POINTER (2), test_cancellation);
    g_test_add_func ("/file-hierarchy/replaced-search", test_replaced_search);
    g_test_add_func ("/file-hierarchy/destroyed-owner", test_destroyed_owner);
#endif
    return g_test_run ();
}
