/* Real read-only archive GFiles, Nemo activation/navigation, previews and
 * verified copies. Run only through the private D-Bus/Xvfb fixture runner. */
#include <config.h>
#include <gtk/gtk.h>
#include "../src/nemo-archive-mounter.h"
#include "../src/nemo-mime-actions.h"
#include "../src/nemo-list-view.h"
#include "../src/nemo-image-viewer.h"
#include "../src/nemo-paged-viewer.h"
#include "../src/nemo-preview-utils.h"
#include "../src/nemo-quick-preview.h"
#include <libnemo-private/nemo-file-operations.h>
#include <libnemo-private/nemo-progress-info-manager.h>
#include "window-test-fixture.h"
#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#endif

/* Observe the real widgets without replacing their asynchronous GFile I/O. */
void __real_nemo_paged_viewer_open_location (NemoPagedViewer *, GFile *);

static void
preview_loaded (NemoPagedViewer *viewer, GError *error, gpointer unused)
{
    g_test_message ("Preview %p load finished: %s", viewer, error ? error->message : "success");
    g_object_set_data (G_OBJECT (viewer), "archive-test-loaded", GINT_TO_POINTER (1));
    g_object_set_data_full (G_OBJECT (viewer), "archive-test-error",
                            error ? g_error_copy (error) : NULL,
                            (GDestroyNotify) g_error_free);
}

void
__wrap_nemo_paged_viewer_open_location (NemoPagedViewer *viewer, GFile *location)
{
    g_autofree char *uri = g_file_get_uri (location);
    g_test_message ("Preview %p opening %s", viewer, uri);
    if (!g_object_get_data (G_OBJECT (viewer), "archive-test-observed")) {
        g_signal_connect (viewer, "load-finished", G_CALLBACK (preview_loaded), NULL);
        g_object_set_data (G_OBJECT (viewer), "archive-test-observed", GINT_TO_POINTER (1));
    }
    g_object_set_data (G_OBJECT (viewer), "archive-test-loaded", NULL);
    g_object_set_data_full (G_OBJECT (viewer), "archive-test-error", NULL, NULL);
    g_object_set_data_full (G_OBJECT (viewer), "archive-test-location",
                            g_object_ref (location), g_object_unref);
    __real_nemo_paged_viewer_open_location (viewer, location);
}

static GtkWidget *
find_descendant (GtkWidget *widget, GType type)
{
    if (G_TYPE_CHECK_INSTANCE_TYPE (widget, type))
        return widget;
    if (!GTK_IS_CONTAINER (widget))
        return NULL;
    GList *children = gtk_container_get_children (GTK_CONTAINER (widget));
    GtkWidget *found = NULL;
    for (GList *l = children; l != NULL && found == NULL; l = l->next)
        found = find_descendant (l->data, type);
    g_list_free (children);
    return found;
}

static GtkWidget *
error_dialog (void)
{
    GList *windows = gtk_window_list_toplevels ();
    GtkWidget *found = NULL;
    for (GList *l = windows; l != NULL; l = l->next) {
        if (GTK_IS_MESSAGE_DIALOG (l->data) && gtk_widget_get_visible (l->data)) {
            GtkMessageType type;
            g_object_get (l->data, "message-type", &type, NULL);
            if (type == GTK_MESSAGE_ERROR) {
                found = l->data;
                break;
            }
        }
    }
    g_list_free (windows);
    return found;
}

static void
assert_no_error_dialog (void)
{
    GtkWidget *dialog = error_dialog ();
    if (dialog != NULL) {
        char *message = NULL, *detail = NULL;
        g_object_get (dialog, "text", &message, "secondary-text", &detail, NULL);
        g_error ("Unexpected Nemo error: %s: %s", message, detail);
    }
}

static void
file_ready (NemoFile *file, gpointer data)
{
    *(gboolean *) data = TRUE;
}

static NemoFile *
ready_file (GFile *location)
{
    gboolean ready = FALSE;
    NemoFile *file = nemo_file_get (location);
    nemo_file_call_when_ready (file, NEMO_FILE_ATTRIBUTE_INFO |
                              NEMO_FILE_ATTRIBUTE_LINK_INFO,
                              file_ready, &ready);
    WAIT_FOR (ready);
    return file;
}

static void
activate (Fixture *fixture, GFile *location)
{
    NemoFile *file = ready_file (location);
    GList files = { .data = file };
    g_autofree char *directory = g_file_get_path (fixture->slot->location);
    nemo_mime_activate_files (GTK_WINDOW (fixture->window), fixture->slot,
                              &files, directory, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT, FALSE);
    nemo_file_unref (file);
}

static void
select_file (Fixture *fixture, GFile *location)
{
    NemoFile *file = ready_file (location);
    g_autofree char *mime = nemo_file_get_mime_type (file);
    g_autofree char *uri = g_file_get_uri (location);
    g_test_message ("Selecting %s type=%d mime=%s", uri, nemo_file_get_file_type (file), mime);
    GList files = { .data = file };
    nemo_view_set_selection (fixture->slot->content_view, &files);
    nemo_file_unref (file);
    nemo_view_grab_focus (fixture->slot->content_view);
}

static gboolean
selected (Fixture *fixture, GFile *location)
{
    GList *files = nemo_view_get_selection (fixture->slot->content_view);
    gboolean match = FALSE;
    if (g_list_length (files) == 1) {
        GFile *actual = nemo_file_get_location (files->data);
        match = g_file_equal (actual, location);
        g_object_unref (actual);
    }
    nemo_file_list_free (files);
    return match;
}

static void
assert_contents (GFile *actual, GFile *expected)
{
    g_autofree char *a = NULL;
    g_autofree char *b = NULL;
    gsize a_size, b_size;
    GError *error = NULL;
    g_assert_true (g_file_load_contents (actual, NULL, &a, &a_size, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_file_load_contents (expected, NULL, &b, &b_size, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpmem (a, a_size, b, b_size);
}

static void
assert_read_only_error (GError *error)
{
    g_assert_nonnull (error);
    g_test_message ("Archive mutation refused: %s", error->message);
    g_assert_true (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) ||
                   g_error_matches (error, G_IO_ERROR, G_IO_ERROR_READ_ONLY) ||
                   g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED));
    g_error_free (error);
}

static void
assert_read_only (GFile *root)
{
    GError *error = NULL;
    GFile *text = g_file_get_child (root, "text.txt");
    GFile *new_file = g_file_get_child (root, "must-not-be-created");
    g_assert_null (g_file_replace (text, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &error));
    assert_read_only_error (error);
    error = NULL;
    g_assert_false (g_file_delete (text, NULL, &error));
    assert_read_only_error (error);
    error = NULL;
    g_assert_null (g_file_create (new_file, G_FILE_CREATE_NONE, NULL, &error));
    assert_read_only_error (error);
    error = NULL;
    g_assert_false (g_file_make_directory (new_file, NULL, &error));
    assert_read_only_error (error);
    g_assert_false (g_file_query_exists (new_file, NULL));
    g_object_unref (new_file);
    g_object_unref (text);
}

typedef struct {
    NemoProgressInfo *info;
    gboolean done, success;
} Copy;

static void
progress_created (NemoProgressInfoManager *manager, NemoProgressInfo *info, Copy *copy)
{
    g_assert_null (copy->info);
    copy->info = g_object_ref (info);
}

static void
copied (GHashTable *debuting, gboolean success, gpointer data)
{
    Copy *copy = data;
    g_printerr ("Archive copy callback: success=%d\n", success);
    copy->success = success;
    copy->done = TRUE;
}

static void
append_dialog_text (GtkWidget *widget, GString *text)
{
    if (GTK_IS_LABEL (widget)) {
        g_string_append (text, gtk_label_get_text (GTK_LABEL (widget)));
        g_string_append_c (text, '\n');
    }
    if (GTK_IS_CONTAINER (widget)) {
        GList *children = gtk_container_get_children (GTK_CONTAINER (widget));
        for (GList *l = children; l != NULL; l = l->next)
            append_dialog_text (l->data, text);
        g_list_free (children);
    }
}

static gboolean
reject_copy_dialog (gpointer data)
{
    Copy *copy = data;
    if (copy->done)
        return G_SOURCE_CONTINUE;
    GList *windows = gtk_window_list_toplevels ();
    for (GList *l = windows; l != NULL; l = l->next) {
        if (GTK_IS_DIALOG (l->data) && gtk_widget_get_visible (l->data) &&
            gtk_window_get_modal (l->data)) {
            GString *text = g_string_new (NULL);
            append_dialog_text (l->data, text);
            g_error ("Archive copy unexpectedly requires a response: %s", text->str);
        }
    }
    g_list_free (windows);
    return G_SOURCE_CONTINUE;
}

static void
copy_out (Fixture *fixture, GFile *root, GFile *source)
{
    Copy copy = { 0 };
    GFile *text = g_file_get_child (root, "text.txt");
    GFile *nested = g_file_get_child (root, "nested");
    /* The Python fixture cleans the build-filesystem destination, including
     * durable-transfer bookkeeping; the isolated profile may be tmpfs. */
    g_assert_nonnull (g_getenv ("NEMO_TEST_ARCHIVE_COPY_ROOT"));
    g_autofree char *destination_path = g_build_filename (
        g_getenv ("NEMO_TEST_ARCHIVE_COPY_ROOT"), "extracted-XXXXXX", NULL);
    g_assert_nonnull (g_mkdtemp (destination_path));
    GFile *destination = g_file_new_for_path (destination_path);
    GFileInfo *filesystem = g_file_query_filesystem_info (
        destination, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE, NULL, NULL);
    g_assert_nonnull (filesystem);
    g_printerr ("Archive copy destination: %s (filesystem=%s)\n", destination_path,
                g_file_info_get_attribute_string (filesystem, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE));
    g_object_unref (filesystem);
    NemoProgressInfoManager *manager = nemo_progress_info_manager_new ();
    gulong handler = g_signal_connect (manager, "new-progress-info",
                                       G_CALLBACK (progress_created), &copy);
    nemo_file_operations_set_verify_copies (TRUE);
    GList second = { .data = nested };
    GList first = { .data = text, .next = &second };
    second.prev = &first;
    guint dialog_guard = g_timeout_add (25, reject_copy_dialog, &copy);
    nemo_file_operations_copy (&first, NULL, destination,
                              GTK_WINDOW (fixture->window), copied, &copy);
    WAIT_FOR (copy.done || error_dialog () != NULL);
    assert_no_error_dialog ();
    g_assert_true (copy.done);
    g_source_remove (dialog_guard);
    g_assert_true (copy.success);
    g_assert_nonnull (copy.info);
    WAIT_FOR (nemo_progress_info_get_is_finished (copy.info));
    NemoProgressResult result;
    g_assert_true (nemo_progress_info_get_result (copy.info, &result));
    g_assert_cmpint (result.operation, ==, NEMO_PROGRESS_OPERATION_COPY);
    g_assert_cmpint (result.outcome, ==, NEMO_PROGRESS_OUTCOME_SUCCESS);
    g_assert_true (result.verification_requested);
    g_assert_cmpuint (result.completed_regular_files, ==, 3);
    g_assert_cmpuint (result.checksum_verified_files, ==, 3);
    g_assert_cmpuint (result.failed_items, ==, 0);
    g_assert_cmpuint (result.skipped_items, ==, 0);
    const char *files[] = {
        "text.txt", "nested/deeper/payload.txt", "nested/deeper/uri # % café.txt"
    };
    for (guint i = 0; i < G_N_ELEMENTS (files); i++) {
        GFile *actual = g_file_resolve_relative_path (destination, files[i]);
        GFile *expected = g_file_resolve_relative_path (source, files[i]);
        assert_contents (actual, expected);
        g_object_unref (actual);
        g_object_unref (expected);
    }
    const char *dirs[] = { "nested/empty", "nested/deeper", "nested" };
    for (guint i = 0; i < G_N_ELEMENTS (dirs); i++) {
        GFile *dir = g_file_resolve_relative_path (destination, dirs[i]);
        g_assert_cmpint (g_file_query_file_type (dir, G_FILE_QUERY_INFO_NONE, NULL),
                        ==, G_FILE_TYPE_DIRECTORY);
        g_object_unref (dir);
    }
    g_signal_handler_disconnect (manager, handler);
    g_object_unref (manager);
    g_object_unref (copy.info);
    g_object_unref (text);
    g_object_unref (nested);
    g_object_unref (destination);
}

static gboolean
viewer_loaded (GtkWidget *viewer, GFile *location)
{
    if (viewer == NULL)
        return FALSE;
    GFile *actual = g_object_get_data (G_OBJECT (viewer), "archive-test-location");
    return actual != NULL && g_file_equal (actual, location) &&
           g_object_get_data (G_OBJECT (viewer), "archive-test-loaded") != NULL;
}

static void
assert_preview (GtkWidget *viewer, GFile *location, NemoViewerMode mode, const char *needle)
{
    g_assert_nonnull (viewer);
    WAIT_FOR (viewer_loaded (viewer, location));
    GError *error = g_object_get_data (G_OBJECT (viewer), "archive-test-error");
    g_assert_no_error (error);
    g_assert_cmpint (nemo_paged_viewer_get_mode (NEMO_PAGED_VIEWER (viewer)), ==, mode);
    WAIT_FOR (gtk_widget_get_mapped (viewer));
    GtkWidget *toplevel = gtk_widget_get_toplevel (viewer);
    if (NEMO_IS_QUICK_PREVIEW (toplevel)) {
        /* Use F3's actual search entry: its debounced signal must not overwrite
         * a needle injected directly into the underlying viewer. */
        GtkWidget *entry = find_descendant (toplevel, GTK_TYPE_SEARCH_ENTRY);
        g_assert_nonnull (entry);
        gtk_entry_set_text (GTK_ENTRY (entry), needle);
        WAIT_FOR (nemo_paged_viewer_search_has_match (NEMO_PAGED_VIEWER (viewer)) ||
                  nemo_paged_viewer_search_get_error (NEMO_PAGED_VIEWER (viewer)) != NULL);
    } else {
        nemo_paged_viewer_search_set_needle (NEMO_PAGED_VIEWER (viewer), needle);
        g_assert_true (nemo_paged_viewer_search_find_next (NEMO_PAGED_VIEWER (viewer)));
        WAIT_FOR (!nemo_paged_viewer_search_is_pending (NEMO_PAGED_VIEWER (viewer)));
    }
    g_assert_no_error (nemo_paged_viewer_search_get_error (NEMO_PAGED_VIEWER (viewer)));
    g_assert_true (nemo_paged_viewer_search_has_match (NEMO_PAGED_VIEWER (viewer)));
}

static void
previews (Fixture *fixture, GFile *root)
{
    nemo_window_slot_set_content_view (fixture->slot, NEMO_LIST_VIEW_ID);
    WAIT_FOR (NEMO_IS_LIST_VIEW (fixture->slot->content_view) &&
              !nemo_view_get_loading (fixture->slot->content_view));
    nemo_window_preview_pane_on (fixture->window);
    GtkWidget *sidebar = find_descendant (fixture->window->details->preview_pane,
                                          NEMO_TYPE_PAGED_VIEWER);
    const char *names[] = { "text.txt", "binary.dat" };
    const char *needles[] = { "archive-regression-text", "archive-regression-binary" };
    for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
        GFile *file = g_file_get_child (root, names[i]);
        /* Use real navigation to set both the selection and keyboard cursor;
         * shell-driven selections intentionally suppress the user signal. */
        nemo_view_grab_focus (fixture->slot->content_view);
        g_assert_true (key_press (fixture->window, i == 0 ? "End" : "Up"));
        WAIT_FOR (selected (fixture, file));
        GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (fixture->window));
        g_test_message ("Checking sidebar %s", names[i]);
        assert_preview (sidebar, file, i == 0 ? NEMO_VIEWER_MODE_TEXT : NEMO_VIEWER_MODE_HEX,
                        needles[i]);
        g_assert_true (gtk_window_get_focus (GTK_WINDOW (fixture->window)) == focus);
        g_assert_true (selected (fixture, file));
        g_assert_true (key_press (fixture->window, "F3"));
        NemoQuickPreview *quick = nemo_quick_preview_get_instance ();
        WAIT_FOR (gtk_widget_get_visible (GTK_WIDGET (quick)));
        GtkWidget *viewer = find_descendant (GTK_WIDGET (quick), NEMO_TYPE_PAGED_VIEWER);
        g_test_message ("Checking F3 %s", names[i]);
        assert_preview (viewer, file, i == 0 ? NEMO_VIEWER_MODE_TEXT : NEMO_VIEWER_MODE_HEX,
                        needles[i]);
        nemo_quick_preview_dismiss (quick);
        gtk_window_present (GTK_WINDOW (fixture->window));
        WAIT_FOR (gtk_window_is_active (GTK_WINDOW (fixture->window)));
        g_object_unref (file);
    }
    nemo_window_preview_pane_off (fixture->window);
}

static void
test_archive (gconstpointer data)
{
    Fixture fixture = fixture_new ();
    g_autofree char *name = g_strconcat ("fixture # % café", data, NULL);
    g_autofree char *path = g_build_filename (g_getenv ("NEMO_TEST_ARCHIVES"), name, NULL);
    GFile *archive = g_file_new_for_path (path);
    GFile *parent = g_file_get_parent (archive);
    GFile *root = nemo_archive_mounter_get_root (archive);
    g_assert_nonnull (root);
    g_assert_true (g_file_has_uri_scheme (root, "nemo-archive"));
    g_assert_false (g_file_is_native (root));
    nemo_window_slot_open_location (fixture.slot, parent, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, parent));
    NemoFile *file = ready_file (archive);
    g_autofree char *mime = nemo_file_get_mime_type (file);
    g_test_message ("%s detected as %s", name, mime);
    g_assert_true (nemo_archive_mounter_is_archive (mime));
    nemo_file_unref (file);
    activate (&fixture, archive);
    WAIT_FOR (slot_at (fixture.slot, root) || error_dialog () != NULL);
    assert_no_error_dialog ();
    g_assert_true (slot_at (fixture.slot, root));
    GError *error = NULL;
    GFile *source = g_file_get_child (parent, "source");
    const char *files[] = {
        "text.txt", "binary.dat", "nested/deeper/payload.txt", "nested/deeper/uri # % café.txt"
    };
    for (guint i = 0; i < G_N_ELEMENTS (files); i++) {
        GFile *actual = g_file_resolve_relative_path (root, files[i]);
        GFile *expected = g_file_resolve_relative_path (source, files[i]);
        assert_contents (actual, expected);
        g_object_unref (actual);
        g_object_unref (expected);
    }
    GFileInfo *info = g_file_query_filesystem_info (root, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY,
                                                   NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (info);
    g_assert_true (g_file_info_get_attribute_boolean (info, G_FILE_ATTRIBUTE_FILESYSTEM_READONLY));
    g_object_unref (info);
    assert_read_only (root);
    GFile *nested = g_file_get_child (root, "nested");
    activate (&fixture, nested);
    WAIT_FOR (slot_at (fixture.slot, nested));
    nemo_view_grab_focus (fixture.slot->content_view);
    g_assert_true (key_press (fixture.window, "<Alt>Up"));
    WAIT_FOR (slot_at (fixture.slot, root));
    previews (&fixture, root);
    copy_out (&fixture, root, source);
    nemo_view_grab_focus (fixture.slot->content_view);
    g_assert_true (key_press (fixture.window, "<Alt>Up"));
    WAIT_FOR (slot_at (fixture.slot, parent));
    WAIT_FOR (selected (&fixture, archive));
    g_object_unref (nested);
    g_object_unref (source);
    g_object_unref (root);
    g_object_unref (parent);
    g_object_unref (archive);
    fixture_clear (&fixture);
}

static void
test_nested_archive (void)
{
    Fixture fixture = fixture_new ();
    g_autofree char *path = g_build_filename (g_getenv ("NEMO_TEST_ARCHIVES"),
                                              "fixture # % café.zip", NULL);
    GFile *archive = g_file_new_for_path (path);
    GFile *parent = g_file_get_parent (archive);
    GFile *root = nemo_archive_mounter_get_root (archive);
    nemo_window_slot_open_location (fixture.slot, parent, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, parent));
    activate (&fixture, archive);
    WAIT_FOR (slot_at (fixture.slot, root) || error_dialog () != NULL);
    assert_no_error_dialog ();

    GFile *inner = g_file_get_child (root, "0-inner # %.7z");
    GFile *inner_root = nemo_archive_mounter_get_root (inner);
    g_assert_nonnull (inner_root);
    activate (&fixture, inner);
    WAIT_FOR (slot_at (fixture.slot, inner_root) || error_dialog () != NULL);
    assert_no_error_dialog ();
    GFile *backing = nemo_archive_mounter_get_archive (inner_root);
    g_assert_nonnull (backing);
    g_assert_true (g_file_equal (backing, inner));
    GFile *source = g_file_get_child (parent, "source");
    GFile *actual = g_file_resolve_relative_path (inner_root, "nested/deeper/uri # % café.txt");
    GFile *expected = g_file_resolve_relative_path (source, "nested/deeper/uri # % café.txt");
    assert_contents (actual, expected);
    assert_read_only (inner_root);
    previews (&fixture, inner_root);
    copy_out (&fixture, inner_root, source);
    nemo_view_grab_focus (fixture.slot->content_view);
    g_assert_true (key_press (fixture.window, "<Alt>Up"));
    WAIT_FOR (slot_at (fixture.slot, root));
    WAIT_FOR (selected (&fixture, inner));
    g_assert_true (key_press (fixture.window, "<Alt>Up"));
    WAIT_FOR (slot_at (fixture.slot, parent));
    WAIT_FOR (selected (&fixture, archive));
    g_object_unref (expected);
    g_object_unref (actual);
    g_object_unref (source);
    g_object_unref (backing);
    g_object_unref (inner_root);
    g_object_unref (inner);
    g_object_unref (root);
    g_object_unref (parent);
    g_object_unref (archive);
    fixture_clear (&fixture);
}

static void
assert_image_preview (GtkWidget *container)
{
    GtkWidget *viewer = find_descendant (container, NEMO_TYPE_IMAGE_VIEWER);
    g_assert_nonnull (viewer);
    GtkWidget *drawing = find_descendant (viewer, GTK_TYPE_DRAWING_AREA);
    g_assert_nonnull (drawing);
    WAIT_FOR (gtk_widget_get_mapped (drawing));
    gboolean found = FALSE;
    gint64 deadline = g_get_monotonic_time () + 10000000;
    while (!found && g_get_monotonic_time () < deadline) {
        iterate ();
        GtkAllocation allocation;
        gtk_widget_get_allocation (drawing, &allocation);
        gtk_widget_size_allocate (drawing, &allocation);
        int width = gtk_widget_get_allocated_width (drawing);
        int height = gtk_widget_get_allocated_height (drawing);
        cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
        cairo_t *cr = cairo_create (surface);
        gtk_widget_draw (drawing, cr);
        cairo_destroy (cr);
        cairo_surface_flush (surface);
        const guchar *pixels = cairo_image_surface_get_data (surface);
        int stride = cairo_image_surface_get_stride (surface);
        for (int y = 0; y < height && !found; y++)
            for (int x = 0; x < width; x++) {
                guint32 pixel;
                memcpy (&pixel, pixels + y * stride + x * 4, sizeof pixel);
                if (pixel == 0xff12ab34) {
                    found = TRUE;
                    break;
                }
            }
        cairo_surface_destroy (surface);
    }
    g_assert_true (found);
}

#ifdef HAVE_GSTREAMER
static void
media_source_setup (GstElement *pipeline, GstElement *source, gpointer data)
{
    g_assert_true (GST_IS_BIN (source));
    GstIterator *children = gst_bin_iterate_sources (GST_BIN (source));
    GValue child = G_VALUE_INIT;
    g_assert_cmpint (gst_iterator_next (children, &child), ==, GST_ITERATOR_OK);
    GstElementFactory *factory = gst_element_get_factory (g_value_get_object (&child));
    g_assert_nonnull (factory);
    g_assert_cmpstr (gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (factory)), ==, "giosrc");
    g_value_reset (&child);
    g_assert_cmpint (gst_iterator_next (children, &child), ==, GST_ITERATOR_DONE);
    g_value_unset (&child);
    gst_iterator_free (children);
    g_atomic_int_set ((gint *) data, 1);
}

static void
wait_media_message (GstBus *bus, GstMessageType expected)
{
    GstMessage *message = NULL;
    WAIT_FOR (message != NULL ||
              (message = gst_bus_pop_filtered (bus, expected | GST_MESSAGE_ERROR)) != NULL);
    if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR) {
        GError *error = NULL;
        char *debug = NULL;
        gst_message_parse_error (message, &error, &debug);
        g_error ("Archive media pipeline failed: %s (%s)", error->message, debug);
    }
    g_assert_cmpuint (GST_MESSAGE_TYPE (message), ==, expected);
    gst_message_unref (message);
}

static void
assert_audio_sample (GstElement *sink, GstClockTime timestamp)
{
    GstSample *sample = NULL;
    g_object_get (sink, "last-sample", &sample, NULL);
    g_assert_nonnull (sample);
    GstCaps *caps = gst_sample_get_caps (sample);
    g_assert_nonnull (caps);
    const GstStructure *format = gst_caps_get_structure (caps, 0);
    g_assert_cmpstr (gst_structure_get_name (format), ==, "audio/x-raw");
    int rate = 0, channels = 0;
    g_assert_true (gst_structure_get_int (format, "rate", &rate));
    g_assert_true (gst_structure_get_int (format, "channels", &channels));
    g_assert_cmpint (rate, ==, 8000);
    g_assert_cmpint (channels, ==, 1);
    GstBuffer *buffer = gst_sample_get_buffer (sample);
    g_assert_nonnull (buffer);
    g_assert_cmpuint (gst_buffer_get_size (buffer), >, 0);
    g_assert_cmpuint (GST_BUFFER_PTS (buffer), ==, timestamp);
    gst_sample_unref (sample);
}

static void
assert_media_decode_and_seek (GFile *location)
{
    GError *error = NULL;
    g_assert_true (nemo_preview_media_init (&error));
    g_assert_no_error (error);
    GstElement *pipeline = gst_element_factory_make ("playbin", "archive-media-test");
    GstElement *audio = gst_element_factory_make ("fakesink", "archive-audio-test");
    GstElement *video = gst_element_factory_make ("fakesink", "archive-video-test");
    g_assert_nonnull (pipeline);
    g_assert_nonnull (audio);
    g_assert_nonnull (video);
    gst_object_ref_sink (audio);
    gst_object_ref_sink (video);
    g_object_set (audio, "sync", FALSE, "enable-last-sample", TRUE, NULL);
    g_object_set (video, "sync", FALSE, NULL);
    g_autofree char *uri = g_file_get_uri (location);
    gint source_ready = 0;
    g_signal_connect (pipeline, "source-setup", G_CALLBACK (media_source_setup), &source_ready);
    g_object_set (pipeline, "uri", uri, "audio-sink", audio, "video-sink", video, NULL);
    GstBus *bus = gst_element_get_bus (pipeline);
    gst_element_set_state (pipeline, GST_STATE_PAUSED);
    wait_media_message (bus, GST_MESSAGE_ASYNC_DONE);
    g_assert_cmpint (g_atomic_int_get (&source_ready), ==, 1);
    gint64 duration = 0;
    g_assert_true (gst_element_query_duration (pipeline, GST_FORMAT_TIME, &duration));
    g_assert_cmpint (duration, ==, 2 * GST_SECOND);
    GstQuery *query = gst_query_new_seeking (GST_FORMAT_TIME);
    g_assert_true (gst_element_query (pipeline, query));
    gboolean seekable = FALSE;
    gst_query_parse_seeking (query, NULL, &seekable, NULL, NULL);
    g_assert_true (seekable);
    gst_query_unref (query);
    assert_audio_sample (audio, 0);
    const GstClockTime positions[] = { GST_SECOND, GST_SECOND / 4 };
    for (guint i = 0; i < G_N_ELEMENTS (positions); i++) {
        g_assert_true (gst_element_seek_simple (pipeline, GST_FORMAT_TIME,
            GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE, positions[i]));
        wait_media_message (bus, GST_MESSAGE_ASYNC_DONE);
        assert_audio_sample (audio, positions[i]);
    }
    g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
    wait_media_message (bus, GST_MESSAGE_EOS);
    g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_NULL), !=, GST_STATE_CHANGE_FAILURE);
    gst_object_unref (bus);
    gst_object_unref (pipeline);
    gst_object_unref (audio);
    gst_object_unref (video);
}
#endif

static void
test_image_media (void)
{
    Fixture fixture = fixture_new ();
    g_autofree char *path = g_build_filename (g_getenv ("NEMO_TEST_ARCHIVES"), "media # %.zip", NULL);
    GFile *archive = g_file_new_for_path (path);
    GFile *root = nemo_archive_mounter_get_root (archive);
    activate (&fixture, archive);
    WAIT_FOR (slot_at (fixture.slot, root) || error_dialog () != NULL);
    assert_no_error_dialog ();
    nemo_window_slot_set_content_view (fixture.slot, NEMO_LIST_VIEW_ID);
    WAIT_FOR (NEMO_IS_LIST_VIEW (fixture.slot->content_view) &&
              !nemo_view_get_loading (fixture.slot->content_view));
    nemo_window_preview_pane_on (fixture.window);
    GFile *image = g_file_get_child (root, "image.png");
    nemo_view_grab_focus (fixture.slot->content_view);
    GtkWidget *focus = gtk_window_get_focus (GTK_WINDOW (fixture.window));
    g_assert_true (key_press (fixture.window, "Home"));
    g_assert_true (key_press (fixture.window, "Down"));
    WAIT_FOR (selected (&fixture, image));
    assert_image_preview (fixture.window->details->preview_pane);
    g_assert_true (gtk_window_get_focus (GTK_WINDOW (fixture.window)) == focus);
    g_assert_true (key_press (fixture.window, "F3"));
    NemoQuickPreview *quick = nemo_quick_preview_get_instance ();
    WAIT_FOR (gtk_widget_get_visible (GTK_WIDGET (quick)));
    assert_image_preview (GTK_WIDGET (quick));
    nemo_quick_preview_dismiss (quick);
    nemo_window_preview_pane_off (fixture.window);
    GFile *audio = g_file_get_child (root, "tone.wav");
#ifdef HAVE_GSTREAMER
    assert_media_decode_and_seek (audio);
#else
    g_error ("Archive media release coverage requires GStreamer support");
#endif
    g_object_unref (audio);
    g_object_unref (image);
    g_object_unref (root);
    g_object_unref (archive);
    fixture_clear (&fixture);
}

static void
test_bad_archive (gconstpointer data)
{
    Fixture fixture = fixture_new ();
    g_autofree char *path = g_build_filename (g_getenv ("NEMO_TEST_ARCHIVES"), data, NULL);
    GFile *archive = g_file_new_for_path (path);
    GFile *parent = g_file_get_parent (archive);
    GFile *root = nemo_archive_mounter_get_root (archive);
    nemo_window_slot_open_location (fixture.slot, parent, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    WAIT_FOR (slot_at (fixture.slot, parent));
    activate (&fixture, archive);
    WAIT_FOR (error_dialog () != NULL || slot_at (fixture.slot, root));
    if (g_str_equal (data, "encrypted.zip") && error_dialog () == NULL) {
        /* Some libarchive versions enumerate encrypted ZIP names but reject
         * reading their contents. The real preview must surface that failure. */
        GFile *text = g_file_get_child (root, "text.txt");
        select_file (&fixture, text);
        g_assert_true (key_press (fixture.window, "F3"));
        NemoQuickPreview *quick = nemo_quick_preview_get_instance ();
        WAIT_FOR (gtk_widget_get_visible (GTK_WIDGET (quick)));
        GtkWidget *viewer = find_descendant (GTK_WIDGET (quick), NEMO_TYPE_PAGED_VIEWER);
        g_assert_nonnull (viewer);
        WAIT_FOR (viewer_loaded (viewer, text));
        GError *error = g_object_get_data (G_OBJECT (viewer), "archive-test-error");
        g_assert_nonnull (error);
        g_test_message ("Encrypted preview error: %s", error->message);
        g_autofree char *tooltip = gtk_widget_get_tooltip_text (viewer);
        g_assert_nonnull (tooltip);
        g_assert_nonnull (strstr (tooltip, error->message));
        nemo_quick_preview_dismiss (quick);
        g_object_unref (text);
    } else {
        GtkWidget *dialog = error_dialog ();
        g_assert_nonnull (dialog);
        g_autofree char *message = NULL;
        g_autofree char *detail = NULL;
        g_object_get (dialog, "text", &message, "secondary-text", &detail, NULL);
        g_assert_nonnull (message);
        g_assert_nonnull (detail);
        g_test_message ("Archive error: %s: %s", message, detail);
        g_assert_cmpuint (strlen (detail), >, 0);
        gtk_dialog_response (GTK_DIALOG (dialog), GTK_RESPONSE_CLOSE);
        WAIT_FOR (slot_at (fixture.slot, parent));
    }
    g_object_unref (root);
    g_object_unref (parent);
    g_object_unref (archive);
    fixture_clear (&fixture);
}

typedef struct {
    const char *path;
    const char *name;
    gboolean invalid;
} ArchiveCase;

static void
run_case (gconstpointer data)
{
    const ArchiveCase *test = data;
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 60000000, 0);
        g_test_trap_assert_passed ();
        return;
    }
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    application = nemo_main_application_get_singleton ();
    GError *error = NULL;
    g_assert_true (g_application_register (G_APPLICATION (application), NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_strv_contains (g_vfs_get_supported_uri_schemes (g_vfs_get_default ()),
                                   "nemo-archive"));
    if (test->invalid)
        test_bad_archive (test->name);
    else if (g_str_equal (test->name, "nested-7z"))
        test_nested_archive ();
    else if (g_str_equal (test->name, "image-media"))
        test_image_media ();
    else
        test_archive (test->name);
    g_object_unref (application);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 1;
    gtk_test_init (&argc, &argv, NULL);
    static const ArchiveCase cases[] = {
        { "/archive-browsing/zip", ".zip", FALSE },
        { "/archive-browsing/tar", ".tar", FALSE },
        { "/archive-browsing/tar-gzip", ".tar.gz", FALSE },
        { "/archive-browsing/tar-xz", ".tar.xz", FALSE },
        { "/archive-browsing/7z", ".7z", FALSE },
        { "/archive-browsing/iso9660", ".iso", FALSE },
        { "/archive-browsing/nested-7z", "nested-7z", FALSE },
        { "/archive-browsing/image-media", "image-media", FALSE },
        { "/archive-browsing/corrupt", "corrupt.zip", TRUE },
        { "/archive-browsing/encrypted", "encrypted.zip", TRUE },
    };
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
        g_test_add_data_func (cases[i].path, &cases[i], run_case);
    return g_test_run ();
}
