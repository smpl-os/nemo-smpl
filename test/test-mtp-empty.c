#include <gtk/gtk.h>
#include <string.h>

#define NEMO_SMPL 1
#define _(text) (text)

typedef struct { gboolean not_empty, search, all_seen; } NemoDirectory;
typedef struct {
    guint focus_count, item_count;
    gboolean loading;
    NemoDirectory *model;
} NemoView;
typedef struct {
    NemoView *content_view;
    NemoDirectory *viewed_file;
    GFile *location, *pending_location;
    GtkWidget *filter_bar_revealer, *filter_bar;
    GtkWidget *mtp_unlock_box, *no_search_results_box, *no_results_label;
    guint mtp_retry_timeout_id, reloads;
    gboolean needs_reload, begin_on_reload, active;
} NemoWindowSlot;
typedef NemoWindowSlot NemoWindow;

#define NEMO_WINDOW_SLOT(slot) ((NemoWindowSlot *) (slot))
#define NEMO_FILTER_BAR(bar) (bar)
#define NEMO_IS_SEARCH_DIRECTORY(directory) ((directory) != NULL && (directory)->search)

static NemoDirectory *nemo_directory_get_for_file (NemoDirectory *file) { return file; }
static gboolean nemo_directory_is_not_empty (NemoDirectory *dir) { return dir->not_empty; }
static gboolean nemo_directory_are_all_files_seen (NemoDirectory *dir) { return dir->all_seen; }
static NemoDirectory *nemo_view_get_model (NemoView *view) { return view->model; }
static gboolean nemo_view_get_loading (NemoView *view) { return view->loading; }
static gboolean fixture_is_empty (NemoView *view) { return view->item_count == 0; }
static const struct { gboolean (*is_empty) (NemoView *); } view_class = { fixture_is_empty };
#define NEMO_VIEW_CLASS(klass) (&view_class)
static void nemo_directory_unref (NemoDirectory *dir) {}
static void nemo_filter_bar_set_text (GtkWidget *bar, const char *text) {}
static void nemo_view_grab_focus (NemoView *view) { view->focus_count++; }
static NemoWindow *nemo_window_slot_get_window (NemoWindowSlot *slot) { return slot; }
static NemoWindowSlot *nemo_window_get_active_slot (NemoWindow *window) { return window->active ? window : NULL; }
static void nemo_window_slot_queue_reload (NemoWindowSlot *slot, gboolean clear);
static void mtp_unlock_overlay_set_visible (NemoWindowSlot *slot, gboolean visible);

#include "mtp-slot-production.inc"

static void
nemo_window_slot_queue_reload (NemoWindowSlot *slot, gboolean clear)
{
    slot->reloads++;
    if (slot->begin_on_reload) {
        view_begin_loading_cb (slot->content_view, slot);
    }
}

static void
setup_slot (NemoWindowSlot *slot, NemoView *view, NemoDirectory *directory, const char *uri)
{
    memset (slot, 0, sizeof (*slot));
    slot->content_view = view;
    slot->active = TRUE;
    slot->viewed_file = directory;
    view->model = directory;
    directory->all_seen = TRUE;
    slot->location = g_file_new_for_uri (uri);
    slot->mtp_unlock_box = g_object_ref_sink (create_mtp_unlock_box ());
    slot->no_search_results_box = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
    slot->no_results_label = gtk_label_new ("");
    gtk_container_add (GTK_CONTAINER (slot->no_search_results_box), slot->no_results_label);
    slot->filter_bar_revealer = g_object_ref_sink (gtk_revealer_new ());
}

static void
clear_slot (NemoWindowSlot *slot)
{
    mtp_unlock_overlay_set_visible (slot, FALSE);
    g_clear_object (&slot->location);
    g_clear_object (&slot->pending_location);
    g_object_unref (slot->mtp_unlock_box);
    g_object_unref (slot->no_search_results_box);
    g_object_unref (slot->filter_bar_revealer);
}

static void
assert_empty_state (NemoWindowSlot *slot)
{
    g_assert_false (gtk_widget_get_visible (slot->mtp_unlock_box));
    g_assert_cmpuint (slot->mtp_retry_timeout_id, ==, 0);
    g_assert_cmpuint (slot->reloads, ==, 0);
}

static void
test_empty_listings (void)
{
    const char *uris[] = {
        "mtp://device/", "mtp://device/SD%20card/Notifications",
        "mtp://device/Internal%20Storage/Empty", "file:///tmp/empty", NULL
    };
    for (guint i = 0; uris[i] != NULL; i++) {
        NemoWindowSlot slot;
        NemoView view = { 0 };
        NemoDirectory directory = { 0 };
        setup_slot (&slot, &view, &directory, uris[i]);
        view_begin_loading_cb (&view, &slot);
        view_end_loading_cb (&view, TRUE, &slot);
        assert_empty_state (&slot);
        g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));
        directory.not_empty = TRUE;
        view_end_loading_cb (&view, TRUE, &slot);
        assert_empty_state (&slot);
        clear_slot (&slot);
    }
}

static void
test_fuse_alias (void)
{
    char *path = g_build_filename (g_getenv ("XDG_RUNTIME_DIR"), "gvfs",
                                   "mtp:host=PHONE", "SD card", "Notifications", NULL);
    char *uri = g_filename_to_uri (path, NULL, NULL);
    g_assert_true (uri_is_mtp_location (uri));
    g_assert_false (uri_is_mtp_location ("file:///tmp/gvfs/mtp:host=not-a-device/empty"));
    g_assert_false (uri_is_mtp_location ("sftp://server/empty"));
    g_assert_false (uri_is_mtp_location (NULL));
    NemoWindowSlot slot;
    NemoView view = { 0 };
    NemoDirectory directory = { 0 };
    setup_slot (&slot, &view, &directory, uri);
    view_end_loading_cb (&view, TRUE, &slot);
    assert_empty_state (&slot);
    clear_slot (&slot);
    g_free (uri);
    g_free (path);
}

static void
test_error_classification (void)
{
    const char *not_locked[] = {
        "Permission denied", "Access denied", "libmtp: no files found",
        "Unable to open MTP device", "The phone is unlocked", "Operation blocked", NULL
    };
    NemoWindowSlot slot;
    NemoView view = { 0 };
    NemoDirectory directory = { 0 };
    setup_slot (&slot, &view, &directory, "mtp://device/empty");
    for (guint i = 0; not_locked[i] != NULL; i++) {
        GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, not_locked[i]);
        g_assert_false (error_looks_like_locked_mtp (error));
        view_load_error_cb (&view, error, &slot);
        assert_empty_state (&slot);
        g_error_free (error);
    }
    GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED, "The phone is locked");
    view_load_error_cb (&view, error, &slot);
    assert_empty_state (&slot);
    g_error_free (error);
    clear_slot (&slot);
}

static void
test_actual_error_recovery (void)
{
    NemoWindowSlot slot;
    NemoView view = { 0 };
    NemoDirectory directory = { 0 };
    setup_slot (&slot, &view, &directory, "mtp://device/");
    GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "The MTP device is locked");
    view_end_loading_cb (&view, FALSE, &slot);
    view_load_error_cb (&view, error, &slot);
    g_assert_true (gtk_widget_get_visible (slot.mtp_unlock_box));
    guint timer = slot.mtp_retry_timeout_id;
    g_assert_cmpuint (timer, !=, 0);
    view_load_error_cb (&view, error, &slot);
    g_assert_cmpuint (slot.mtp_retry_timeout_id, ==, timer);
    view_end_loading_cb (&view, TRUE, &slot);
    assert_empty_state (&slot);
    view_load_error_cb (&view, error, &slot);
    slot.begin_on_reload = TRUE;
    mtp_unlock_retry_cb (&slot);
    g_assert_cmpuint (slot.reloads, ==, 1);
    g_assert_cmpuint (slot.mtp_retry_timeout_id, ==, 0);
    g_assert_false (gtk_widget_get_visible (slot.mtp_unlock_box));
    view_end_loading_cb (&view, TRUE, &slot);
    g_error_free (error);
    clear_slot (&slot);
}

static void
test_navigation_and_stale_views (void)
{
    NemoWindowSlot slot;
    NemoView view = { 0 }, old_view = { 0 };
    NemoDirectory directory = { 0 };
    setup_slot (&slot, &view, &directory, "mtp://device/old");
    GError *error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_FAILED, "Unlock your phone");
    view_load_error_cb (&old_view, error, &slot);
    assert_empty_state (&slot);
    view_load_error_cb (&view, error, &slot);
    view_end_loading_cb (&old_view, TRUE, &slot);
    g_assert_true (gtk_widget_get_visible (slot.mtp_unlock_box));
    slot.pending_location = g_file_new_for_uri ("mtp://device/other");
    g_assert_false (mtp_unlock_retry_cb (&slot));
    assert_empty_state (&slot);
    view_load_error_cb (&view, error, &slot);
    assert_empty_state (&slot);
    g_clear_object (&slot.pending_location);
    g_clear_object (&slot.location);
    slot.location = g_file_new_for_uri ("file:///tmp/other");
    view_load_error_cb (&view, error, &slot);
    assert_empty_state (&slot);
    g_error_free (error);
    clear_slot (&slot);
}

static void
test_cancelled_search_is_not_empty_result (void)
{
    NemoWindowSlot slot;
    NemoView view = { 0 };
    NemoDirectory directory = { .search = TRUE };
    setup_slot (&slot, &view, &directory, "x-nemo-search:///results");
    directory.all_seen = FALSE;
    view_end_loading_cb (&view, FALSE, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));
    clear_slot (&slot);
}

static void
test_search_restart_hides_previous_empty_result (void)
{
    NemoWindowSlot slot;
    NemoView view = { 0 };
    NemoDirectory directory = { .search = TRUE };
    setup_slot (&slot, &view, &directory, "x-nemo-search:///results");
    view_end_loading_cb (&view, TRUE, &slot);
    g_assert_true (gtk_widget_get_visible (slot.no_search_results_box));
    view.loading = TRUE;
    directory.all_seen = FALSE;
    view_begin_loading_cb (&view, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));
    clear_slot (&slot);
}

static void
test_search_streamed_results (void)
{
    NemoWindowSlot slot;
    NemoView view = { 0 };
    NemoDirectory directory = { .search = TRUE };
    setup_slot (&slot, &view, &directory, "x-nemo-search:///results");
    view_end_loading_cb (&view, TRUE, &slot);
    g_assert_true (gtk_widget_get_visible (slot.no_search_results_box));

    view.loading = TRUE;
    directory.all_seen = FALSE;
    directory.not_empty = TRUE;
    view.item_count = 1;
    view_update_empty_state_cb (&view, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));

    /* A temporarily empty batch is not the end of a recursive search. */
    directory.not_empty = FALSE;
    view.item_count = 0;
    view_update_empty_state_cb (&view, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));
    view.loading = FALSE;
    view_update_empty_state_cb (&view, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));

    directory.all_seen = TRUE;
    view_end_loading_cb (&view, TRUE, &slot);
    g_assert_true (gtk_widget_get_visible (slot.no_search_results_box));

    /* Never cover visible rows, even if the backing list was just reset. */
    view.item_count = 2;
    view_end_loading_cb (&view, TRUE, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));
    gtk_widget_show (slot.no_search_results_box);
    view_update_empty_state_cb (&view, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));
    clear_slot (&slot);
}

static void
test_search_stale_view_and_navigation (void)
{
    NemoWindowSlot slot;
    NemoView view = { 0 }, old_view = { .item_count = 10 };
    NemoDirectory directory = { .search = TRUE };
    setup_slot (&slot, &view, &directory, "x-nemo-search:///results");
    view_end_loading_cb (&view, TRUE, &slot);
    view_update_empty_state_cb (&old_view, &slot);
    view_begin_loading_cb (&old_view, &slot);
    g_assert_true (gtk_widget_get_visible (slot.no_search_results_box));
    slot.pending_location = g_file_new_for_uri ("file:///tmp/next");
    view_update_empty_state_cb (&view, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));
    g_clear_object (&slot.pending_location);
    view.model = NULL;
    view_update_empty_state_cb (&view, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));
    clear_slot (&slot);
}

static void
test_filter_result_updates (void)
{
    NemoWindowSlot slot;
    NemoView view = { 0 };
    NemoDirectory directory = { .not_empty = TRUE };
    setup_slot (&slot, &view, &directory, "file:///tmp/filter");
    gtk_revealer_set_reveal_child (GTK_REVEALER (slot.filter_bar_revealer), TRUE);
    view_update_empty_state_cb (&view, &slot);
    g_assert_true (gtk_widget_get_visible (slot.no_search_results_box));
    g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (slot.no_results_label)), ==, "No matching files");
    view.item_count = 1;
    view_update_empty_state_cb (&view, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));
    view.item_count = 0;
    gtk_widget_show (slot.mtp_unlock_box);
    view_update_empty_state_cb (&view, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));
    gtk_widget_hide (slot.mtp_unlock_box);
    gtk_revealer_set_reveal_child (GTK_REVEALER (slot.filter_bar_revealer), FALSE);
    view_update_empty_state_cb (&view, &slot);
    g_assert_false (gtk_widget_get_visible (slot.no_search_results_box));
    clear_slot (&slot);
}

static void
test_background_load_keeps_focus (void)
{
    NemoWindowSlot slot;
    NemoView view = { 0 };
    NemoDirectory directory = { 0 };
    setup_slot (&slot, &view, &directory, "file:///tmp/other-pane");
    slot.active = FALSE;
    view_begin_loading_cb (&view, &slot);
    g_assert_cmpuint (view.focus_count, ==, 0);
    slot.active = TRUE;
    view_begin_loading_cb (&view, &slot);
    g_assert_cmpuint (view.focus_count, ==, 1);
    clear_slot (&slot);
}

static void
test_search_state (void)
{
    NemoWindowSlot slot;
    NemoView view = { 0 };
    NemoDirectory directory = { .search = TRUE };
    setup_slot (&slot, &view, &directory, "search:///results");
    view_end_loading_cb (&view, TRUE, &slot);
    assert_empty_state (&slot);
    g_assert_true (gtk_widget_get_visible (slot.no_search_results_box));
    g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (slot.no_results_label)), ==, "No files found");
    clear_slot (&slot);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
        g_printerr ("Use the isolated regression runner\n");
        return 77;
    }
    g_test_init (&argc, &argv, NULL);
    gtk_init (&argc, &argv);
    g_test_add_func ("/mtp/empty-listings", test_empty_listings);
    g_test_add_func ("/mtp/fuse-alias", test_fuse_alias);
    g_test_add_func ("/mtp/error-classification", test_error_classification);
    g_test_add_func ("/mtp/error-recovery", test_actual_error_recovery);
    g_test_add_func ("/mtp/navigation-stale", test_navigation_and_stale_views);
    g_test_add_func ("/mtp/search-state", test_search_state);
    g_test_add_func ("/search/cancelled-is-not-no-results", test_cancelled_search_is_not_empty_result);
    g_test_add_func ("/search/restart-hides-old-empty-result", test_search_restart_hides_previous_empty_result);
    g_test_add_func ("/search/streamed-results", test_search_streamed_results);
    g_test_add_func ("/search/stale-view-navigation", test_search_stale_view_and_navigation);
    g_test_add_func ("/search/filter-result-updates", test_filter_result_updates);
    g_test_add_func ("/search/background-load-focus", test_background_load_keeps_focus);
    return g_test_run ();
}
