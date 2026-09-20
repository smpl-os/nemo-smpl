/* Run with an isolated D-Bus/display and the memory GSettings backend.
 * Include the implementation to test its actual completion/destroy handlers;
 * only the filesystem query is replaced, without public production hooks.
 *
 * Link src/nemo-error-reporting.c and src/nemo-desktop-item-properties.c
 * with gtk/nemo_private, -ffunction-sections, -fdata-sections, and
 * -Wl,--gc-sections. Both support sources are required for -O0 builds;
 * optimized dead-code elimination can hide these dependencies.
 * Do not separately compile src/nemo-properties-window.c. */
#include <config.h>
#include <gtk/gtk.h>

#ifdef NEMO_SMPL
static void test_query_async (GFile *, const char *, int, GCancellable *,
                             GAsyncReadyCallback, gpointer);
static GFileInfo *test_query_finish (GFile *, GAsyncResult *, GError **);
static GFileInfo *test_query_sync (GFile *, const char *, GCancellable *, GError **) G_GNUC_UNUSED;

#define g_file_query_filesystem_info_async test_query_async
#define g_file_query_filesystem_info_finish test_query_finish
#define g_file_query_filesystem_info test_query_sync
#include "../src/nemo-properties-window.c"
#undef g_file_query_filesystem_info_async
#undef g_file_query_filesystem_info_finish
#undef g_file_query_filesystem_info

typedef GObject TestMount;
typedef GObjectClass TestMountClass;

static void test_mount_iface_init (GMountIface *iface);
G_DEFINE_TYPE_WITH_CODE (TestMount, test_mount, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE (G_TYPE_MOUNT, test_mount_iface_init))

static void test_mount_class_init (TestMountClass *klass) {}
static void test_mount_init (TestMount *mount) {}

static GFile *
test_mount_root (GMount *mount)
{
    return g_file_new_for_uri ("test-volume:///actual-mounted-root");
}

static void
test_mount_iface_init (GMountIface *iface)
{
    iface->get_root = test_mount_root;
}

typedef struct {
    GAsyncReadyCallback callback;
    gpointer data;
} QueryCompletion;

static GTask *pending_query;
static gboolean query_completed;
static guint query_count;

static GFileInfo *
test_query_sync (GFile *file, const char *attributes,
                 GCancellable *cancellable, GError **error)
{
    g_assert_not_reached ();
    return NULL;
}

static void
query_ready (GObject *source, GAsyncResult *result, gpointer data)
{
    QueryCompletion *completion = data;
    completion->callback (source, result, completion->data);
    query_completed = TRUE;
    g_free (completion);
}

static void
test_query_async (GFile *file, const char *attributes, int priority,
                  GCancellable *cancellable, GAsyncReadyCallback callback,
                  gpointer data)
{
    QueryCompletion *completion = g_new0 (QueryCompletion, 1);
    GFile *expected = test_mount_root (NULL);

    g_assert_null (pending_query);
    g_assert_true (g_file_equal (file, expected));
    g_assert_nonnull (strstr (attributes, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE));
    g_assert_nonnull (strstr (attributes, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE));
    g_assert_nonnull (strstr (attributes, G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
    g_assert_nonnull (cancellable);
    g_object_unref (expected);
    query_count++;
    completion->callback = callback;
    completion->data = data;
    pending_query = g_task_new (file, cancellable, query_ready, completion);
}

static GFileInfo *
test_query_finish (GFile *file, GAsyncResult *result, GError **error)
{
    g_assert_true (g_task_is_valid (result, file));
    return g_task_propagate_pointer (G_TASK (result), error);
}

static NemoPropertiesWindow *
new_volume_window (void)
{
    NemoPropertiesWindow *window;
    NemoFile *file;
    GMount *mount;
    GtkWidget *pie;

    query_count = 0;
    query_completed = FALSE;
    window = g_object_ref_sink (g_object_new (NEMO_TYPE_PROPERTIES_WINDOW, NULL));
    file = nemo_file_get_by_uri ("test-volume:///desktop-icon");
    mount = g_object_new (test_mount_get_type (), NULL);
    nemo_file_set_mount (file, mount);
    g_object_unref (mount);
    window->details->original_files = g_list_prepend (NULL, file);
    window->details->stack = GTK_STACK (gtk_stack_new ());
    gtk_container_add (GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (window))),
                       GTK_WIDGET (window->details->stack));
    pie = create_volume_usage_widget (window);
    gtk_stack_add_named (window->details->stack, pie, "usage");
    g_assert_cmpuint (query_count, ==, 1);
    g_assert_false (query_completed);
    g_assert_cmpstr (gtk_label_get_text (window->details->volume_capacity_label),
                     ==, _("Loading volume information…"));
    return window;
}

static GFileInfo *
volume_info (guint64 size, guint64 free_space)
{
    GFileInfo *info = g_file_info_new ();
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "testfs");
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, size);
    g_file_info_set_attribute_uint64 (info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, free_space);
    return info;
}

static void
complete_query (GFileInfo *info)
{
    if (info != NULL) {
        g_task_return_pointer (pending_query, info, g_object_unref);
    } else {
        g_task_return_new_error (pending_query, G_IO_ERROR, G_IO_ERROR_NOT_MOUNTED,
                                 "Test volume disconnected");
    }
    g_clear_object (&pending_query);
}

static void
wait_for_query (void)
{
    gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
    while (!query_completed && g_get_monotonic_time () < deadline) {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (1000);
    }
    g_assert_true (query_completed);
}

static void
destroy_window (NemoPropertiesWindow *window)
{
    gtk_widget_destroy (GTK_WIDGET (window));
    g_object_unref (window);
}

static void
test_volume_success (void)
{
    NemoPropertiesWindow *window = new_volume_window ();
    complete_query (volume_info (4096, 1024));
    wait_for_query ();
    g_assert_true (window->details->volume_usage_ready);
    g_assert_cmpuint (window->details->volume_capacity, ==, 4096);
    g_assert_cmpuint (window->details->volume_free, ==, 1024);
    g_assert_nonnull (strstr (gtk_label_get_text (window->details->volume_fstype_label), "testfs"));
    g_assert_null (window->details->volume_usage_cancellable);
    destroy_window (window);
}

static void
test_volume_error (void)
{
    NemoPropertiesWindow *window = new_volume_window ();
    complete_query (NULL);
    wait_for_query ();
    g_assert_false (window->details->volume_usage_ready);
    g_assert_cmpstr (gtk_label_get_text (window->details->volume_capacity_label),
                     ==, _("Volume information unavailable"));
    g_assert_cmpstr (gtk_label_get_text (window->details->volume_used_label), ==, "");
    destroy_window (window);
}

static void
test_volume_missing_attributes (void)
{
    NemoPropertiesWindow *window = new_volume_window ();
    GFileInfo *info = g_file_info_new ();
    g_file_info_set_attribute_string (info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, "testfs");
    complete_query (info);
    wait_for_query ();
    g_assert_false (window->details->volume_usage_ready);
    g_assert_nonnull (strstr (gtk_label_get_text (window->details->volume_capacity_label), _("Unknown")));
    g_assert_nonnull (strstr (gtk_label_get_text (window->details->volume_fstype_label), "testfs"));
    destroy_window (window);
}

static void
test_volume_limits (gconstpointer data)
{
    NemoPropertiesWindow *window = new_volume_window ();
    guint64 capacity = GPOINTER_TO_UINT (data);
    cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 200, 200);
    cairo_t *cr = cairo_create (surface);
    complete_query (volume_info (capacity, 8192));
    wait_for_query ();
    g_assert_cmpuint (window->details->volume_free, ==, capacity);
    paint_pie_chart (window->details->volume_pie, cr, window);
    g_assert_cmpint (cairo_status (cr), ==, CAIRO_STATUS_SUCCESS);
    cairo_destroy (cr);
    cairo_surface_destroy (surface);
    destroy_window (window);
}

static void
test_volume_destroyed (gconstpointer data)
{
    NemoPropertiesWindow *window = new_volume_window ();
    NemoPropertiesWindow *weak_window = window;
    GCancellable *cancellable = g_object_ref (window->details->volume_usage_cancellable);

    g_object_add_weak_pointer (G_OBJECT (window), (gpointer *) &weak_window);
    complete_query (volume_info (4096, 1024));
    gtk_widget_destroy (GTK_WIDGET (window));
    g_assert_true (g_cancellable_is_cancelled (cancellable));
    g_assert_null (window->details->volume_usage_cancellable);
    if (GPOINTER_TO_INT (data)) {
        g_object_unref (window);
        g_assert_null (weak_window);
    }
    wait_for_query ();
    if (!GPOINTER_TO_INT (data)) {
        g_assert_false (window->details->volume_usage_ready);
        g_object_unref (window);
        g_assert_null (weak_window);
    }
    g_object_unref (cancellable);
}

static void
test_volume_replaced_query (void)
{
    NemoPropertiesWindow *window = new_volume_window ();
    GCancellable *old = window->details->volume_usage_cancellable;
    GCancellable *current = g_cancellable_new ();
    window->details->volume_usage_cancellable = current;
    complete_query (volume_info (4096, 1024));
    wait_for_query ();
    g_assert_false (window->details->volume_usage_ready);
    g_assert_true (window->details->volume_usage_cancellable == current);
    g_assert_cmpstr (gtk_label_get_text (window->details->volume_capacity_label),
                     ==, _("Loading volume information…"));
    g_object_unref (old);
    destroy_window (window);
}
#endif

int
main (int argc, char **argv)
{
#ifdef NEMO_SMPL
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
        g_printerr ("Run this test through test/run-isolated-regression.py\n");
        return 77;
    }
    g_setenv ("GDK_BACKEND", "x11", TRUE);
    g_unsetenv ("WAYLAND_DISPLAY");
    g_unsetenv ("AT_SPI_BUS_ADDRESS");
    g_unsetenv ("DBUS_STARTER_ADDRESS");
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
    g_test_init (&argc, &argv, NULL);
    if (!gtk_init_check (&argc, &argv)) {
        return 77;
    }
    g_assert_cmpstr (gdk_display_get_name (gdk_display_get_default ()), ==, g_getenv ("DISPLAY"));
    g_test_message ("GTK display: %s", gdk_display_get_name (gdk_display_get_default ()));
    nemo_global_preferences_init ();
    g_test_add_func ("/properties-volume/success-mounted-root", test_volume_success);
    g_test_add_func ("/properties-volume/error", test_volume_error);
    g_test_add_func ("/properties-volume/missing-attributes", test_volume_missing_attributes);
    g_test_add_data_func ("/properties-volume/free-exceeds-capacity", GUINT_TO_POINTER (4096), test_volume_limits);
    g_test_add_data_func ("/properties-volume/zero-capacity", GUINT_TO_POINTER (0), test_volume_limits);
    g_test_add_data_func ("/properties-volume/destroy-queued-result", GINT_TO_POINTER (0), test_volume_destroyed);
    g_test_add_data_func ("/properties-volume/finalize-queued-result", GINT_TO_POINTER (1), test_volume_destroyed);
    g_test_add_func ("/properties-volume/replaced-query", test_volume_replaced_query);
    return g_test_run ();
#else
    return 77;
#endif
}
