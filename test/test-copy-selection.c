/* Actual view callbacks, with deterministic completion instead of filesystem I/O.
 * Run only through run-isolated-regression.py. */
#include <config.h>
#include <gtk/gtk.h>
#include "../src/nemo-view.h"
#include "../src/nemo-view-dnd.h"
#include <libnemo-private/nemo-file-operations.h>

static void capture_copy (const GList *, GArray *, const char *, GdkDragAction,
                          GtkWidget *, NemoCopyCallback, gpointer);
static GtkWidget *drag_source (GdkDragContext *);
static void fixture_status (NemoWindowSlot *, const char *, const char *, gboolean);
static void request_clipboard (GtkClipboard *, GdkAtom, GtkClipboardReceivedFunc, gpointer);
static NemoWindowSlot *extra_slot (NemoWindow *);
static char *slot_uri (NemoWindowSlot *);
static gint confirm_copy (GtkDialog *);

#define nemo_file_operations_copy_move capture_copy
#define gtk_drag_get_source_widget drag_source
#define nemo_window_slot_set_status fixture_status
#define gtk_clipboard_request_contents request_clipboard
#define nemo_window_get_extra_slot(window) extra_slot (NULL)
#define nemo_window_slot_get_current_uri slot_uri
#define gtk_dialog_run confirm_copy
#include "../src/nemo-view.c"
#undef nemo_file_operations_copy_move
#undef gtk_drag_get_source_widget
#undef nemo_window_slot_set_status
#undef gtk_clipboard_request_contents
#undef nemo_window_get_extra_slot
#undef nemo_window_slot_get_current_uri
#undef gtk_dialog_run

typedef struct {
    NemoView parent;
    GList *selected;
    GList *transfer;
    guint clears;
} SelectionView;
typedef NemoViewClass SelectionViewClass;
G_DEFINE_TYPE (SelectionView, selection_view, NEMO_TYPE_VIEW)

typedef struct {
    NemoCopyCallback callback;
    gpointer data;
    GList *uris;
    int action;
} PendingCopy;
static GQueue copies = G_QUEUE_INIT;
static GtkWidget *current_drag_source;
static GtkClipboardReceivedFunc clipboard_received;
static gpointer clipboard_request;
static NemoWindowSlot *confirmation_slot;
static SelectionView *confirmation_source;
static gboolean close_confirmation;

static GList *
fixture_selection (NemoView *view)
{
    return nemo_file_list_copy (((SelectionView *) view)->selected);
}

static GList *
fixture_transfer (NemoView *view)
{
    SelectionView *fixture = (SelectionView *) view;
    return nemo_file_list_copy (fixture->transfer != NULL ? fixture->transfer : fixture->selected);
}

static void
fixture_set_selection (NemoView *view, GList *files)
{
    SelectionView *fixture = (SelectionView *) view;
    GList *copy = nemo_file_list_copy (files);
    nemo_file_list_free (fixture->selected);
    fixture->selected = copy;
    if (files == NULL) {
        fixture->clears++;
    }
}

static void fixture_reveal (NemoView *view) {}
static void fixture_clear (NemoView *view) {}

static void
fixture_property (GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
    if (g_str_equal (spec->name, "window-slot")) {
        return;
    }
    G_OBJECT_CLASS (selection_view_parent_class)->set_property (object, id, value, spec);
}

static void
selection_view_class_init (SelectionViewClass *klass)
{
    G_OBJECT_CLASS (klass)->set_property = fixture_property;
    g_object_class_override_property (G_OBJECT_CLASS (klass), PROP_WINDOW_SLOT, "window-slot");
    klass->get_selection = fixture_selection;
    klass->get_selection_for_file_transfer = fixture_transfer;
    klass->set_selection = fixture_set_selection;
    klass->get_backing_uri = nemo_view_get_uri;
    klass->reveal_selection = fixture_reveal;
    klass->clear = fixture_clear;
}

static void selection_view_init (SelectionView *view) {}

static SelectionView *
new_view (const char *name)
{
    SelectionView *view = g_object_ref_sink (g_object_new (selection_view_get_type (), NULL));
    char *uri = g_strdup_printf ("file://%s/%s", g_getenv ("NEMO_TEST_PROFILE"), name);
    NEMO_VIEW (view)->details->model = nemo_directory_get_by_uri (uri);
    NEMO_VIEW (view)->details->directory_as_file =
        nemo_directory_get_corresponding_file (NEMO_VIEW (view)->details->model);
    g_free (uri);
    uri = g_strdup_printf ("file://%s/%s/item", g_getenv ("NEMO_TEST_PROFILE"), name);
    view->selected = g_list_append (NULL, nemo_file_get_by_uri (uri));
    g_free (uri);
    return view;
}

static void
free_view (SelectionView *view)
{
    gtk_widget_destroy (GTK_WIDGET (view));
    nemo_file_list_free (view->selected);
    nemo_file_list_free (view->transfer);
    view->selected = view->transfer = NULL;
    g_object_unref (view);
}

static void
capture_copy (const GList *uris, GArray *points, const char *target,
              GdkDragAction action, GtkWidget *parent,
              NemoCopyCallback callback, gpointer data)
{
    PendingCopy *copy = g_new0 (PendingCopy, 1);
    copy->callback = callback;
    copy->data = data;
    copy->action = action;
    for (const GList *l = uris; l != NULL; l = l->next) {
        copy->uris = g_list_append (copy->uris, g_strdup (l->data));
    }
    g_queue_push_tail (&copies, copy);
}

static void
finish_copy_full (gboolean success, NemoFile *destination, gboolean wait_for_file)
{
    PendingCopy *copy = g_queue_pop_head (&copies);
    GHashTable *debuting = g_hash_table_new_full (g_file_hash, (GEqualFunc) g_file_equal,
                                                g_object_unref, NULL);
    g_assert_nonnull (copy);
    if (destination != NULL) {
        g_hash_table_insert (debuting, nemo_file_get_location (destination),
                             GINT_TO_POINTER (wait_for_file));
    }
    copy->callback (debuting, success, copy->data);
    g_hash_table_unref (debuting);
    g_list_free_full (copy->uris, g_free);
    g_free (copy);
}

static void
finish_copy (gboolean success, NemoFile *destination)
{
    finish_copy_full (success, destination, FALSE);
}

static void
start_copy (SelectionView *source, SelectionView *destination, int action)
{
    CopySelection *snapshot = copy_selection_new (NEMO_VIEW (source));
    GList *files = fixture_transfer (NEMO_VIEW (source));
    GList *uris = copy_selection_uris (files);
    char *target = nemo_view_get_uri (NEMO_VIEW (destination));
    move_copy_items_with_source (NEMO_VIEW (destination), uris, NULL,
                                target, action, 0, 0, snapshot);
    copy_selection_unref (snapshot);
    nemo_file_list_free (files);
    g_list_free_full (uris, g_free);
    g_free (target);
}

static void
test_completion (gconstpointer success)
{
    SelectionView *source = new_view ("source");
    SelectionView *dest = new_view ("destination");
    NemoFile *selected_dest = dest->selected->data;
    start_copy (source, dest, GDK_ACTION_COPY);
    g_assert_cmpuint (source->clears, ==, 0);
    finish_copy (GPOINTER_TO_INT (success), NULL);
    g_assert_cmpuint (source->clears, ==, GPOINTER_TO_INT (success));
    g_assert_true (dest->selected->data == selected_dest);
    free_view (source);
    free_view (dest);
}

static void
test_noncopy (gconstpointer action)
{
    SelectionView *source = new_view ("source");
    SelectionView *dest = new_view ("destination");
    start_copy (source, dest, GPOINTER_TO_INT (action));
    finish_copy (TRUE, NULL);
    g_assert_cmpuint (source->clears, ==, 0);
    free_view (source);
    free_view (dest);
}

static void
test_changed (gconstpointer mode)
{
    SelectionView *source = new_view ("source");
    SelectionView *dest = new_view ("destination");
    NemoView *view = NEMO_VIEW (source);
    start_copy (source, dest, GDK_ACTION_COPY);
    switch (GPOINTER_TO_INT (mode)) {
    case 0:
        nemo_view_start_batching_selection_changes (view);
        nemo_view_notify_selection_changed (view);
        g_source_remove (view->details->display_selection_idle_id);
        view->details->display_selection_idle_id = 0;
        nemo_view_notify_selection_changed (view);
        break;
    case 1: {
        NemoDirectory *original = g_object_ref (view->details->model);
        NemoDirectory *other = nemo_directory_get_by_uri ("file:///nonexistent-copy-test-location");
        load_directory (view, other);
        load_directory (view, original);
        nemo_directory_unref (other);
        nemo_directory_unref (original);
        break;
    }
    case 2:
        nemo_view_call_set_selection (view, source->selected);
        break;
    case 3:
        /* Raw expanded-child selection changes even when transfer is unchanged. */
        source->transfer = nemo_file_list_copy (source->selected);
        source->selected = g_list_append (source->selected,
                                         nemo_file_get_by_uri ("file:///not-a-real-child"));
        break;
    }
    finish_copy (TRUE, NULL);
    g_assert_cmpuint (source->clears, ==, 0);
    free_view (source);
    free_view (dest);
}

static void
test_same_folder (gconstpointer delayed)
{
    SelectionView *view = new_view ("same");
    NemoFile *result = nemo_file_get_by_uri ("file:///not-a-real-destination");
    start_copy (view, view, GDK_ACTION_COPY);
    finish_copy_full (TRUE, result, GPOINTER_TO_INT (delayed));
    g_assert_cmpuint (view->clears, ==, 1);
    if (GPOINTER_TO_INT (delayed)) {
        g_assert_null (view->selected);
        g_signal_emit_by_name (view, "add-file", result, NEMO_VIEW (view)->details->model);
    }
    g_assert_true (view->selected->data == result);
    nemo_file_unref (result);
    free_view (view);
}

static void
test_lifetime (gconstpointer mode)
{
    SelectionView *source = new_view ("source");
    SelectionView *dest = new_view ("destination");
    start_copy (source, dest, GDK_ACTION_COPY);
    switch (GPOINTER_TO_INT (mode)) {
    case 0: gtk_widget_destroy (GTK_WIDGET (source)); break;
    case 1: free_view (source); source = NULL; break;
    case 2: gtk_widget_destroy (GTK_WIDGET (dest)); break;
    case 3: free_view (dest); dest = NULL; break;
    }
    finish_copy (TRUE, NULL);
    if (source != NULL) {
        g_assert_cmpuint (source->clears, ==, GPOINTER_TO_INT (mode) >= 2);
        free_view (source);
    }
    if (dest != NULL) {
        free_view (dest);
    }
}

static void
test_overlapping (void)
{
    SelectionView *source = new_view ("source");
    SelectionView *dest = new_view ("destination");
    start_copy (source, dest, GDK_ACTION_COPY);
    start_copy (source, dest, GDK_ACTION_COPY);
    finish_copy (TRUE, NULL);
    finish_copy (TRUE, NULL);
    g_assert_cmpuint (source->clears, ==, 1);
    free_view (source);
    free_view (dest);
}

static void
test_saturation (void)
{
    SelectionView *view = new_view ("source");
    NEMO_VIEW (view)->details->selection_generation = G_MAXUINT64 - 1;
    CopySelection *snapshot = copy_selection_new (NEMO_VIEW (view));
    copy_selection_changed (NEMO_VIEW (view));
    copy_selection_changed (NEMO_VIEW (view));
    g_assert_cmpuint (NEMO_VIEW (view)->details->selection_generation, ==, G_MAXUINT64);
    g_assert_null (copy_selection_new (NEMO_VIEW (view)));
    copy_selection_complete (snapshot, TRUE);
    g_assert_cmpuint (view->clears, ==, 0);
    copy_selection_unref (snapshot);
    free_view (view);
}

static void
test_frozen_inputs (void)
{
    SelectionView *view = new_view ("source");
    GList *approved = fixture_transfer (NEMO_VIEW (view));
    CopySelection *snapshot = copy_selection_new (NEMO_VIEW (view));
    NemoFile *other = nemo_file_get_by_uri ("file:///not-the-approved-item");
    GList replacement = { .data = other };
    nemo_view_call_set_selection (NEMO_VIEW (view), &replacement);
    move_copy_files_to_location (NEMO_VIEW (view), approved, GDK_ACTION_COPY,
                                "file:///unused-destination", snapshot);
    PendingCopy *copy = g_queue_peek_head (&copies);
    g_assert_true (copy_selection_same_uris (copy->uris, snapshot->transfer));
    finish_copy (TRUE, NULL);
    g_assert_true (view->selected->data == other);
    g_assert_cmpuint (view->clears, ==, 0);
    copy_selection_unref (snapshot);
    nemo_file_list_free (approved);
    nemo_file_unref (other);
    free_view (view);
}

static GtkWidget *
drag_source (GdkDragContext *context)
{
    return current_drag_source;
}

static void
fixture_status (NemoWindowSlot *slot, const char *status, const char *detail, gboolean loading)
{
}

static void
request_clipboard (GtkClipboard *clipboard, GdkAtom target,
                   GtkClipboardReceivedFunc callback, gpointer data)
{
    g_assert_null (clipboard_request);
    clipboard_received = callback;
    clipboard_request = data;
}

static NemoWindowSlot *extra_slot (NemoWindow *window) { return confirmation_slot; }
static char *slot_uri (NemoWindowSlot *slot) { return g_strdup ("file:///unused-destination"); }

static gint
confirm_copy (GtkDialog *dialog)
{
    if (close_confirmation) {
        gtk_widget_destroy (GTK_WIDGET (confirmation_source));
        gtk_widget_destroy (GTK_WIDGET (dialog));
        return GTK_RESPONSE_NONE;
    }
    NemoFile *other = nemo_file_get_by_uri ("file:///not-the-confirmed-item");
    GList replacement = { .data = other };
    nemo_view_call_set_selection (NEMO_VIEW (confirmation_source), &replacement);
    nemo_file_unref (other);
    return GTK_RESPONSE_OK;
}

static void
test_pane_confirmation (gconstpointer mode)
{
    gboolean move = GPOINTER_TO_INT (mode) >= 2;
    close_confirmation = GPOINTER_TO_INT (mode) % 2;
    confirmation_source = new_view (move ? "f6-source" : "f5-source");
    confirmation_slot = g_object_ref_sink (g_object_new (NEMO_TYPE_WINDOW_SLOT, NULL));
    GList *original = copy_selection_uris (confirmation_source->selected);
    if (move)
        action_move_to_next_pane_callback (NULL, confirmation_source);
    else
        action_copy_to_next_pane_callback (NULL, confirmation_source);
    PendingCopy *copy = g_queue_peek_head (&copies);
    if (close_confirmation) {
        g_assert_null (copy);
    } else {
        g_assert_nonnull (copy);
        g_assert_cmpint (copy->action, ==, move ? GDK_ACTION_MOVE : GDK_ACTION_COPY);
        g_assert_true (copy_selection_same_uris (copy->uris, original));
        finish_copy (TRUE, NULL);
    }
    g_assert_cmpuint (confirmation_source->clears, ==, 0);
    g_list_free_full (original, g_free);
    free_view (confirmation_source);
    g_object_unref (confirmation_slot);
    confirmation_slot = NULL;
    confirmation_source = NULL;
    close_confirmation = FALSE;
}

static void
test_copy_to (void)
{
    SelectionView *view = new_view ("source");
    move_copy_selection_to_location (NEMO_VIEW (view), GDK_ACTION_COPY, "file:///unused-target");
    finish_copy (TRUE, NULL);
    g_assert_cmpuint (view->clears, ==, 1);
    free_view (view);
}

static void
test_copy_to_verified (gconstpointer changed)
{
    SelectionView *view = new_view ("source");
    NemoFile *new_selection = nemo_file_get_by_uri ("file:///new-source-selection");
    NemoFile *verified_target = nemo_file_get_by_uri ("file:///different-target/existing");
    GList replacement = { .data = new_selection };

    move_copy_selection_to_location (NEMO_VIEW (view), GDK_ACTION_COPY,
                                    "file:///different-target");
    if (GPOINTER_TO_INT (changed)) {
        nemo_view_call_set_selection (NEMO_VIEW (view), &replacement);
    }
    finish_copy (TRUE, verified_target);
    if (GPOINTER_TO_INT (changed)) {
        g_assert_true (view->selected->data == new_selection);
        g_assert_cmpuint (view->clears, ==, 0);
    } else {
        g_assert_null (view->selected);
        g_assert_cmpuint (view->clears, ==, 1);
    }
    free_view (view);
    nemo_file_unref (new_selection);
    nemo_file_unref (verified_target);
}

static void
test_destination_guard (gconstpointer mode)
{
    SelectionView *view = new_view ("same-folder");
    NemoView *base = NEMO_VIEW (view);
    NemoFile *original = nemo_file_ref (view->selected->data);
    NemoFile *result = nemo_file_get_by_uri ("file:///completed-destination");
    GList selection = { .data = original };

    start_copy (view, view, GDK_ACTION_COPY);
    if (GPOINTER_TO_INT (mode) == 0) {
        nemo_view_call_set_selection (base, &selection);
    } else if (GPOINTER_TO_INT (mode) == 1) {
        NemoDirectory *original_directory = g_object_ref (base->details->model);
        NemoDirectory *other = nemo_directory_get_by_uri ("file:///other-copy-location");
        load_directory (base, other);
        load_directory (base, original_directory);
        nemo_directory_unref (other);
        nemo_directory_unref (original_directory);
    }
    if (GPOINTER_TO_INT (mode) == 3) {
        finish_copy_full (TRUE, result, TRUE);
        g_assert_null (view->selected);
        nemo_view_call_set_selection (base, &selection);
        g_signal_emit_by_name (view, "add-file", result, base->details->model);
        g_assert_cmpuint (view->clears, ==, 1);
    } else {
        finish_copy (GPOINTER_TO_INT (mode) != 2, result);
        g_assert_cmpuint (view->clears, ==, 0);
    }
    g_assert_true (view->selected->data == original);
    free_view (view);
    nemo_file_unref (original);
    nemo_file_unref (result);
}

static void
test_changed_destination (void)
{
    SelectionView *source = new_view ("source");
    SelectionView *dest = new_view ("destination");
    NemoFile *selected = dest->selected->data;
    NemoFile *result = nemo_file_get_by_uri ("file:///verified-destination");

    start_copy (source, dest, GDK_ACTION_COPY);
    nemo_view_call_set_selection (NEMO_VIEW (dest), dest->selected);
    finish_copy (TRUE, result);
    g_assert_null (source->selected);
    g_assert_true (dest->selected->data == selected);
    nemo_file_unref (result);
    free_view (source);
    free_view (dest);
}

static void
test_clipboard (gconstpointer mode)
{
    SelectionView *source = new_view ("clipboard-source");
    SelectionView *dest = GPOINTER_TO_INT (mode) >= 6
        ? source : new_view ("clipboard-destination");
    GtkClipboard *clipboard = nemo_clipboard_get (GTK_WIDGET (source));
    action_copy_files_callback (NULL, source);
    action_paste_files_callback (NULL, dest);
    PasteIntoData *request = clipboard_request;
    CopySelection *origin = request->source_selection;
    g_assert_nonnull (origin);
    GtkSelectionData *payload = gtk_clipboard_wait_for_contents (clipboard, copied_files_atom);
    g_assert_nonnull (payload);
    char *target = nemo_view_get_uri (NEMO_VIEW (dest));
    switch (GPOINTER_TO_INT (mode)) {
    case 1:
        /* A new copy from the same view and same files is still a new origin. */
        action_copy_files_callback (NULL, source);
        break;
    case 2:
        gtk_clipboard_set_text (clipboard, "replacement", -1);
        break;
    case 3:
    case 6:
        nemo_view_call_set_selection (NEMO_VIEW (source), source->selected);
        break;
    case 4: {
        const char *wrong = "copy\nfile:///different-clipboard-item";
        gtk_selection_data_set (payload, copied_files_atom, 8,
                                (const guchar *) wrong, strlen (wrong));
        break;
    }
    }
    paste_clipboard_data (NEMO_VIEW (dest), payload, target, origin);
    if (GPOINTER_TO_INT (mode) == 5 || GPOINTER_TO_INT (mode) == 7) {
        gtk_clipboard_set_text (clipboard, "replaced while copying", -1);
    }
    NemoFile *verified = GPOINTER_TO_INT (mode) >= 6
        ? nemo_file_get_by_uri ("file:///clipboard-existing-target") : NULL;
    NemoFile *original = source->selected->data;
    finish_copy (TRUE, verified);
    if (verified != NULL) {
        g_assert_true (source->selected->data == original);
        nemo_file_unref (verified);
    }
    g_assert_cmpuint (source->clears, ==, GPOINTER_TO_INT (mode) == 0);
    g_assert_cmpuint (dest->clears, ==, 0);
    if (GPOINTER_TO_INT (mode) == 0) {
        g_assert_nonnull (nemo_clipboard_monitor_get_clipboard_info (nemo_clipboard_monitor_get ()));
        paste_clipboard_data (NEMO_VIEW (dest), payload, target, origin);
        finish_copy (TRUE, NULL);
        g_assert_cmpuint (source->clears, ==, 1);
    }
    gtk_clipboard_clear (clipboard);
    g_assert_null (copy_clipboard_get_source ());
    clipboard_received (clipboard, NULL, clipboard_request);
    clipboard_request = NULL;
    gtk_selection_data_free (payload);
    g_free (target);
    free_view (source);
    if (dest != source) {
        free_view (dest);
    }
}

static void
test_delayed_clipboard_close (gconstpointer into)
{
    SelectionView *source = new_view ("clipboard-source");
    SelectionView *dest = new_view ("clipboard-destination");
    action_copy_files_callback (NULL, source);
    if (GPOINTER_TO_INT (into)) {
        paste_into (NEMO_VIEW (dest), dest->selected->data);
    } else {
        action_paste_files_callback (NULL, dest);
    }
    g_assert_nonnull (clipboard_request);
    free_view (source);
    gtk_widget_destroy (GTK_WIDGET (dest));
    clipboard_received (nemo_clipboard_get (GTK_WIDGET (dest)), NULL, clipboard_request);
    clipboard_request = NULL;
    g_assert_true (g_queue_is_empty (&copies));
    free_view (dest);
    nemo_clipboard_monitor_set_clipboard_info (nemo_clipboard_monitor_get (), NULL);
}

typedef struct {
    NemoView *view;
    GList *uris;
    gboolean stop;
    gboolean uri_drop;
    GtkWidget *nested_source;
    gboolean nested;
} DropFixture;

static void
forward_icon_drop (NemoIconContainer *container, GList *uris, GArray *points,
                   const char *target, GdkDragAction action, int x, int y,
                   NemoView *view)
{
    nemo_view_drop_items (view, uris, points, target, action, x, y);
}

static void
forward_uri_drop (NemoIconContainer *container, const char *uris, const char *target,
                  GdkDragAction action, int x, int y, NemoView *view)
{
    nemo_view_handle_uri_list_drop (view, uris, target, action, x, y);
}

static void
dispatch_drop (GtkWidget *widget, GdkDragContext *context,
               int x, int y, GtkSelectionData *selection,
               guint info, guint time, DropFixture *fixture)
{
    if (fixture->nested) {
        /* The list drag destination stops even motion/data-only emissions. */
        g_signal_stop_emission_by_name (widget, "drag-data-received");
        return;
    }
    if (fixture->nested_source != NULL) {
        GtkWidget *outer_source = current_drag_source;
        current_drag_source = fixture->nested_source;
        fixture->nested = TRUE;
        g_signal_emit_by_name (widget, "drag-data-received", NULL, 0, 0, NULL, 0, 0);
        fixture->nested = FALSE;
        current_drag_source = outer_source;
    }
    if (fixture->uri_drop) {
        g_signal_emit_by_name (widget, "handle-uri-list", fixture->uris->data,
                               "file:///unused-drop-target", GDK_ACTION_COPY, 0, 0);
    } else if (NEMO_IS_ICON_CONTAINER (widget)) {
        g_signal_emit_by_name (widget, "move-copy-items", fixture->uris, NULL,
                               "file:///unused-drop-target", GDK_ACTION_COPY, 0, 0);
    } else {
        nemo_view_drop_items (fixture->view, fixture->uris, NULL,
                             "file:///unused-drop-target", GDK_ACTION_COPY, 0, 0);
    }
    if (fixture->stop) {
        g_signal_stop_emission_by_name (widget, "drag-data-received");
    }
}

static void
test_drag (gconstpointer mode)
{
    SelectionView *source = new_view ("drag-source");
    SelectionView *dest = new_view ("drag-dest");
    SelectionView *nested_source = NULL;
    GtkWidget *source_widget = g_object_ref_sink (gtk_drawing_area_new ());
    GtkWidget *dest_widget = GPOINTER_TO_INT (mode) == 0 || GPOINTER_TO_INT (mode) == 6
        ? g_object_ref_sink (g_object_new (NEMO_TYPE_ICON_CONTAINER, NULL))
        : g_object_ref_sink (gtk_drawing_area_new ());
    nemo_view_setup_copy_drag (NEMO_VIEW (source), source_widget);
    nemo_view_setup_copy_drag (NEMO_VIEW (dest), dest_widget);
    if (GPOINTER_TO_INT (mode) == 5) {
        source->transfer = nemo_file_list_copy (source->selected);
        source->selected = g_list_append (source->selected,
                                         nemo_file_get_by_uri ("file:///expanded-child"));
    }
    GList *files = fixture_transfer (NEMO_VIEW (source));
    DropFixture fixture = {
        .view = NEMO_VIEW (dest),
        .uris = copy_selection_uris (files),
        .stop = GPOINTER_TO_INT (mode) == 1 || GPOINTER_TO_INT (mode) == 7,
        .uri_drop = GPOINTER_TO_INT (mode) == 6,
    };
    nemo_file_list_free (files);
    if (GPOINTER_TO_INT (mode) == 7) {
        /* Identical URI payload, but a different view must never be cleared. */
        nested_source = new_view ("drag-source");
        fixture.nested_source = g_object_ref_sink (gtk_drawing_area_new ());
        nemo_view_setup_copy_drag (NEMO_VIEW (nested_source), fixture.nested_source);
        g_signal_emit_by_name (fixture.nested_source, "drag-begin", NULL);
    }
    g_signal_connect (dest_widget, "drag-data-received", G_CALLBACK (dispatch_drop), &fixture);
    if (NEMO_IS_ICON_CONTAINER (dest_widget)) {
        g_signal_connect (dest_widget, "move-copy-items", G_CALLBACK (forward_icon_drop), dest);
        g_signal_connect (dest_widget, "handle-uri-list", G_CALLBACK (forward_uri_drop), dest);
    }
    g_signal_emit_by_name (source_widget, "drag-begin", NULL);
    current_drag_source = GPOINTER_TO_INT (mode) == 2 ? NULL : source_widget;
    if (GPOINTER_TO_INT (mode) == 3) {
        nemo_view_call_set_selection (NEMO_VIEW (source), source->selected);
    } else if (GPOINTER_TO_INT (mode) == 4) {
        g_free (fixture.uris->data);
        fixture.uris->data = g_strdup ("file:///wrong-drag-item");
    }
    g_signal_emit_by_name (dest_widget, "drag-data-received", NULL, 0, 0, NULL, 0, 0);
    g_signal_emit_by_name (source_widget, "drag-end", NULL);
    finish_copy (TRUE, NULL);
    g_assert_cmpuint (source->clears, ==,
                     GPOINTER_TO_INT (mode) < 2 ||
                     GPOINTER_TO_INT (mode) == 5 || GPOINTER_TO_INT (mode) == 6);
    g_assert_cmpuint (dest->clears, ==, 0);
    /* A URI forwarded later, outside the trusted drag emission, has no origin. */
    nemo_view_drop_items (NEMO_VIEW (dest), fixture.uris, NULL,
                         "file:///unused-drop-target", GDK_ACTION_COPY, 0, 0);
    PendingCopy *copy = g_queue_peek_head (&copies);
    g_assert_null (((CopyMoveDoneData *) copy->data)->source_selection);
    finish_copy (TRUE, NULL);
    current_drag_source = NULL;
    if (nested_source != NULL) {
        g_assert_cmpuint (nested_source->clears, ==, 0);
        gtk_widget_destroy (fixture.nested_source);
        g_object_unref (fixture.nested_source);
        free_view (nested_source);
    }
    g_list_free_full (fixture.uris, g_free);
    gtk_widget_destroy (source_widget);
    gtk_widget_destroy (dest_widget);
    g_object_unref (source_widget);
    g_object_unref (dest_widget);
    free_view (source);
    free_view (dest);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
        g_printerr ("Run through test/run-isolated-regression.py\n");
        return 77;
    }
    g_test_init (&argc, &argv, NULL);
    /* Optional system Nemo actions may have unavailable Cinnamon dependencies. */
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    if (!gtk_init_check (&argc, &argv)) {
        return 77;
    }
    nemo_global_preferences_init ();
    g_test_add_data_func ("/copy-selection/success", GINT_TO_POINTER (TRUE), test_completion);
    g_test_add_data_func ("/copy-selection/failed-partial-skipped-cancelled",
                          GINT_TO_POINTER (FALSE), test_completion);
    g_test_add_data_func ("/copy-selection/move", GINT_TO_POINTER (GDK_ACTION_MOVE), test_noncopy);
    g_test_add_data_func ("/copy-selection/link", GINT_TO_POINTER (GDK_ACTION_LINK), test_noncopy);
    g_test_add_data_func ("/copy-selection/selection-away-back", GINT_TO_POINTER (0), test_changed);
    g_test_add_data_func ("/copy-selection/location-away-back", GINT_TO_POINTER (1), test_changed);
    g_test_add_data_func ("/copy-selection/same-set-reselection", GINT_TO_POINTER (2), test_changed);
    g_test_add_data_func ("/copy-selection/raw-selection-guard", GINT_TO_POINTER (3), test_changed);
    g_test_add_data_func ("/copy-selection/same-folder-destination", GINT_TO_POINTER (0), test_same_folder);
    g_test_add_data_func ("/copy-selection/same-folder-debuting-destination", GINT_TO_POINTER (1), test_same_folder);
    g_test_add_data_func ("/copy-selection/source-destroy", GINT_TO_POINTER (0), test_lifetime);
    g_test_add_data_func ("/copy-selection/source-finalize", GINT_TO_POINTER (1), test_lifetime);
    g_test_add_data_func ("/copy-selection/destination-destroy", GINT_TO_POINTER (2), test_lifetime);
    g_test_add_data_func ("/copy-selection/destination-finalize", GINT_TO_POINTER (3), test_lifetime);
    g_test_add_func ("/copy-selection/overlapping", test_overlapping);
    g_test_add_func ("/copy-selection/generation-saturation", test_saturation);
    g_test_add_func ("/copy-selection/approved-inputs", test_frozen_inputs);
    g_test_add_data_func ("/copy-selection/f5-modal-selection-change", GINT_TO_POINTER (0), test_pane_confirmation);
    g_test_add_data_func ("/copy-selection/f5-modal-source-close", GINT_TO_POINTER (1), test_pane_confirmation);
    g_test_add_data_func ("/copy-selection/f6-modal-selection-change", GINT_TO_POINTER (2), test_pane_confirmation);
    g_test_add_data_func ("/copy-selection/f6-modal-source-close", GINT_TO_POINTER (3), test_pane_confirmation);
    g_test_add_func ("/copy-selection/copy-to", test_copy_to);
    g_test_add_data_func ("/copy-selection/copy-to-verified-target", GINT_TO_POINTER (0), test_copy_to_verified);
    g_test_add_data_func ("/copy-selection/changed-copy-to-verified-target", GINT_TO_POINTER (1), test_copy_to_verified);
    g_test_add_data_func ("/copy-selection/same-folder-stale-verified-target",
                          GINT_TO_POINTER (0), test_destination_guard);
    g_test_add_data_func ("/copy-selection/same-folder-navigated-verified-target",
                          GINT_TO_POINTER (1), test_destination_guard);
    g_test_add_data_func ("/copy-selection/same-folder-partial-destination",
                          GINT_TO_POINTER (2), test_destination_guard);
    g_test_add_data_func ("/copy-selection/changed-before-destination-arrives",
                          GINT_TO_POINTER (3), test_destination_guard);
    g_test_add_func ("/copy-selection/changed-destination-preserved", test_changed_destination);
    g_test_add_data_func ("/copy-selection/clipboard-repeat", GINT_TO_POINTER (0), test_clipboard);
    g_test_add_data_func ("/copy-selection/clipboard-same-set-replaced", GINT_TO_POINTER (1), test_clipboard);
    g_test_add_data_func ("/copy-selection/clipboard-lost", GINT_TO_POINTER (2), test_clipboard);
    g_test_add_data_func ("/copy-selection/clipboard-selection-changed", GINT_TO_POINTER (3), test_clipboard);
    g_test_add_data_func ("/copy-selection/clipboard-payload-mismatch", GINT_TO_POINTER (4), test_clipboard);
    g_test_add_data_func ("/copy-selection/clipboard-replaced-during-copy", GINT_TO_POINTER (5), test_clipboard);
    g_test_add_data_func ("/copy-selection/same-folder-clipboard-stale-origin", GINT_TO_POINTER (6), test_clipboard);
    g_test_add_data_func ("/copy-selection/same-folder-clipboard-revoked-origin", GINT_TO_POINTER (7), test_clipboard);
    g_test_add_data_func ("/copy-selection/clipboard-delayed-destination-close",
                          GINT_TO_POINTER (0), test_delayed_clipboard_close);
    g_test_add_data_func ("/copy-selection/clipboard-into-delayed-destination-close",
                          GINT_TO_POINTER (1), test_delayed_clipboard_close);
    g_test_add_data_func ("/copy-selection/icon-drag", GINT_TO_POINTER (0), test_drag);
    g_test_add_data_func ("/copy-selection/list-drag", GINT_TO_POINTER (1), test_drag);
    g_test_add_data_func ("/copy-selection/external-drag", GINT_TO_POINTER (2), test_drag);
    g_test_add_data_func ("/copy-selection/drag-selection-changed", GINT_TO_POINTER (3), test_drag);
    g_test_add_data_func ("/copy-selection/drag-payload-mismatch", GINT_TO_POINTER (4), test_drag);
    g_test_add_data_func ("/copy-selection/expanded-parent-child-drag", GINT_TO_POINTER (5), test_drag);
    g_test_add_data_func ("/copy-selection/icon-uri-drag", GINT_TO_POINTER (6), test_drag);
    g_test_add_data_func ("/copy-selection/reentrant-list-drag", GINT_TO_POINTER (7), test_drag);
    return g_test_run ();
}
