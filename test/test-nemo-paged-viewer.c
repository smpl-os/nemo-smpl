/* Isolated GTK regression tests. Including the implementation lets the tests
 * verify displayed bytes and stale generations, not just completion signals.
 * Link nemo-preview-utils.c for the shared bounded worker scheduler. */
#include "../src/nemo-paged-viewer.c"

#ifdef NEMO_SMPL

typedef struct {
	gint refs;
	GMutex mutex;
	GCond cond;
	gboolean hold_open;
	gboolean hold_read;
	gboolean hold_close;
	gint64 delay_from;
	gboolean fail_read;
	gint64 fail_from;
	gint64 size;
	GBytes *contents;
	gboolean seekable;
	gint opens;
	gint reads;
	gint closes;
	gint blocked_opens;
	gint blocked_reads;
	gint blocked_closes;
	GThread *main_thread;
} MockData;

typedef struct { GFileInputStream parent; MockData *data; goffset position; } MockStream;
typedef struct { GFileInputStreamClass parent; } MockStreamClass;
typedef struct { GObject parent; MockData *data; } MockFile;
typedef struct { GObjectClass parent; } MockFileClass;

static void mock_file_iface_init (GFileIface *iface);
G_DEFINE_TYPE (MockStream, mock_stream, G_TYPE_FILE_INPUT_STREAM)
G_DEFINE_TYPE_WITH_CODE (MockFile, mock_file, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE (G_TYPE_FILE, mock_file_iface_init))

static MockData *
mock_data_ref (MockData *data)
{
	g_atomic_int_inc (&data->refs);
	return data;
}

static void
mock_data_unref (MockData *data)
{
	if (!g_atomic_int_dec_and_test (&data->refs))
		return;
	g_clear_pointer (&data->contents, g_bytes_unref);
	g_mutex_clear (&data->mutex);
	g_cond_clear (&data->cond);
	g_free (data);
}

static void
wait_gate (MockData *data, gboolean *hold, gint *blocked)
{
	g_assert_true (g_thread_self () != data->main_thread);
	g_mutex_lock (&data->mutex);
	if (*hold) {
		g_atomic_int_inc (blocked);
		/* Deliberately ignore cancellation, just like a stuck kernel read. */
		while (*hold)
			g_cond_wait (&data->cond, &data->mutex);
	}
	g_mutex_unlock (&data->mutex);
}

static void
release_gates (MockData *data)
{
	g_mutex_lock (&data->mutex);
	data->hold_open = data->hold_read = data->hold_close = FALSE;
	g_cond_broadcast (&data->cond);
	g_mutex_unlock (&data->mutex);
}

static gssize
mock_stream_read (GInputStream *input, void *buffer, gsize count,
                  GCancellable *cancellable, GError **error)
{
	MockStream *stream = (MockStream *) input;
	MockData *data = stream->data;
	g_assert_true (g_thread_self () != data->main_thread);
	g_atomic_int_inc (&data->reads);
	if (stream->position >= data->delay_from)
		wait_gate (data, &data->hold_read, &data->blocked_reads);
	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return -1;
	if (data->fail_read && stream->position >= data->fail_from) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                     "Simulated device read failure");
		return -1;
	}
	count = MIN ((guint64) count, (guint64) (data->size - stream->position));
	if (data->contents) {
		memcpy (buffer, (const guint8 *) g_bytes_get_data (data->contents, NULL) +
		                stream->position, count);
	} else {
		for (gsize i = 0; i < count; i++)
			((guint8 *) buffer)[i] = (stream->position + i) % 80 == 79 ? '\n' : 'x';
	}
	stream->position += count;
	return count;
}

static gssize
mock_stream_skip (GInputStream *input, gsize count, GCancellable *cancellable, GError **error)
{
	MockStream *stream = (MockStream *) input;
	g_assert_true (g_thread_self () != stream->data->main_thread);
	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return -1;
	count = MIN ((guint64) count, (guint64) (stream->data->size - stream->position));
	stream->position += count;
	return count;
}

static gboolean
mock_stream_close (GInputStream *input, GCancellable *cancellable, GError **error)
{
	MockData *data = ((MockStream *) input)->data;
	wait_gate (data, &data->hold_close, &data->blocked_closes);
	g_atomic_int_inc (&data->closes);
	return TRUE;
}

static GFileInfo *
mock_info (MockData *data)
{
	g_assert_true (g_thread_self () != data->main_thread);
	GFileInfo *info = g_file_info_new ();
	g_file_info_set_size (info, data->size);
	g_file_info_set_file_type (info, G_FILE_TYPE_REGULAR);
	return info;
}

static GFileInfo *
mock_stream_info (GFileInputStream *stream, const gchar *attributes,
                  GCancellable *cancellable, GError **error)
{
	return mock_info (((MockStream *) stream)->data);
}

static goffset
mock_stream_tell (GFileInputStream *stream)
{
	return ((MockStream *) stream)->position;
}

static gboolean
mock_stream_can_seek (GFileInputStream *stream)
{
	return ((MockStream *) stream)->data->seekable;
}

static gboolean
mock_stream_seek (GFileInputStream *input, goffset offset, GSeekType type,
                  GCancellable *cancellable, GError **error)
{
	MockStream *stream = (MockStream *) input;
	g_assert_true (g_thread_self () != stream->data->main_thread);
	g_assert_cmpint (type, ==, G_SEEK_SET);
	g_assert_cmpint (offset, >=, 0);
	g_assert_cmpint (offset, <=, stream->data->size);
	stream->position = offset;
	return TRUE;
}

static void
mock_stream_finalize (GObject *object)
{
	mock_data_unref (((MockStream *) object)->data);
	G_OBJECT_CLASS (mock_stream_parent_class)->finalize (object);
}

static void
mock_stream_class_init (MockStreamClass *klass)
{
	GInputStreamClass *input = G_INPUT_STREAM_CLASS (klass);
	input->read_fn = mock_stream_read;
	input->skip = mock_stream_skip;
	input->close_fn = mock_stream_close;
	GFileInputStreamClass *file_input = G_FILE_INPUT_STREAM_CLASS (klass);
	file_input->tell = mock_stream_tell;
	file_input->can_seek = mock_stream_can_seek;
	file_input->seek = mock_stream_seek;
	file_input->query_info = mock_stream_info;
	G_OBJECT_CLASS (klass)->finalize = mock_stream_finalize;
}

static void mock_stream_init (MockStream *stream) {}

static GFileInputStream *
mock_file_read (GFile *file, GCancellable *cancellable, GError **error)
{
	MockData *data = ((MockFile *) file)->data;
	wait_gate (data, &data->hold_open, &data->blocked_opens);
	g_atomic_int_inc (&data->opens);
	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return NULL;
	MockStream *stream = g_object_new (mock_stream_get_type (), NULL);
	stream->data = mock_data_ref (data);
	return G_FILE_INPUT_STREAM (stream);
}

static GFile *mock_file_dup (GFile *file) { return g_object_ref (file); }
static gboolean mock_file_native (GFile *file) { return FALSE; }
static gchar *mock_file_path (GFile *file) { return NULL; }
static gchar *mock_file_uri (GFile *file) { return g_strdup ("mock://device/file"); }

static void
mock_file_iface_init (GFileIface *iface)
{
	iface->dup = mock_file_dup;
	iface->is_native = mock_file_native;
	iface->get_path = mock_file_path;
	iface->get_uri = mock_file_uri;
	iface->read_fn = mock_file_read;
}

static void
mock_file_finalize (GObject *object)
{
	mock_data_unref (((MockFile *) object)->data);
	G_OBJECT_CLASS (mock_file_parent_class)->finalize (object);
}

static void
mock_file_class_init (MockFileClass *klass)
{
	G_OBJECT_CLASS (klass)->finalize = mock_file_finalize;
}

static void mock_file_init (MockFile *file) {}

static GFile *
new_file (const gchar *contents, gsize size, MockData **data_out)
{
	MockFile *file = g_object_new (mock_file_get_type (), NULL);
	MockData *data = g_new0 (MockData, 1);
	data->refs = 1;
	data->size = size;
	data->seekable = TRUE;
	data->main_thread = g_thread_self ();
	g_mutex_init (&data->mutex);
	g_cond_init (&data->cond);
	if (contents)
		data->contents = g_bytes_new (contents, size);
	file->data = data;
	*data_out = data;
	return G_FILE (file);
}

static NemoPagedViewer *
new_viewer (void)
{
	return g_object_ref_sink (nemo_paged_viewer_new ());
}

static void
free_viewer (NemoPagedViewer *viewer)
{
	gtk_widget_destroy (GTK_WIDGET (viewer));
	g_object_unref (viewer);
}

/* Every wait pumps the GTK context with a finite deadline. */
#define WAIT_FOR(condition) G_STMT_START { \
	gint64 deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND; \
	while (!(condition) && g_get_monotonic_time () < deadline) { \
		g_main_context_iteration (NULL, FALSE); \
		g_usleep (1000); \
	} \
	g_assert_true (condition); \
} G_STMT_END

static gboolean
heartbeat (gpointer data)
{
	(*(guint *) data)++;
	return G_SOURCE_CONTINUE;
}

static void
assert_responsive (void)
{
	guint beats = 0;
	guint timer = g_timeout_add (5, heartbeat, &beats);
	WAIT_FOR (beats >= 5);
	g_source_remove (timer);
}

static void
assert_first_text (NemoPagedViewer *viewer, const gchar *text)
{
	g_assert_nonnull (viewer->page);
	g_assert_cmpuint (viewer->page->lines->len, >, 0);
	DisplayLine *line = g_ptr_array_index (viewer->page->lines, 0);
	g_assert_cmpstr (line->text, ==, text);
}

static void
load_count (NemoPagedViewer *viewer, GError *error, gpointer data)
{
	g_assert_no_error (error);
	(*(guint *) data)++;
}

static void
test_delayed_open_switch (void)
{
	MockData *slow, *fast;
	GFile *old = new_file ("old\n", 4, &slow);
	GFile *fresh = new_file ("new\n", 4, &fast);
	slow->hold_open = TRUE;
	NemoPagedViewer *viewer = new_viewer ();
	guint completed = 0;
	g_signal_connect (viewer, "load-finished", G_CALLBACK (load_count), &completed);
	nemo_paged_viewer_open_location (viewer, old);
	WAIT_FOR (g_atomic_int_get (&slow->blocked_opens) > 0);
	assert_responsive ();
	nemo_paged_viewer_open_location (viewer, fresh);
	WAIT_FOR (viewer->opened);
	assert_first_text (viewer, "new");
	release_gates (slow);
	WAIT_FOR (g_atomic_int_get (&slow->opens) > 0);
	assert_responsive ();
	assert_first_text (viewer, "new");
	g_assert_cmpuint (completed, ==, 1);
	free_viewer (viewer);
	g_object_unref (old);
	g_object_unref (fresh);
}

static void
test_destroy_pending (void)
{
	MockData *data;
	GFile *file = new_file ("pending\n", 8, &data);
	data->hold_open = TRUE;
	NemoPagedViewer *viewer = new_viewer ();
	gpointer weak = viewer;
	g_object_add_weak_pointer (G_OBJECT (viewer), &weak);
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (g_atomic_int_get (&data->blocked_opens) > 0);
	free_viewer (viewer);
	g_assert_null (weak);
	assert_responsive ();
	release_gates (data);
	WAIT_FOR (g_atomic_int_get (&data->opens) > 0);
	assert_responsive ();
	g_object_unref (file);
}

static void
test_delayed_page_switch (void)
{
	MockData *data, *replacement;
	GFile *file = new_file (NULL, 3 * 1024 * 1024, &data);
	GFile *fresh = new_file ("replacement\n", 12, &replacement);
	data->delay_from = 1024 * 1024;
	data->hold_read = TRUE;
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (viewer->opened);
	gtk_adjustment_set_value (viewer->vadjust, 2 * 1024 * 1024);
	WAIT_FOR (g_atomic_int_get (&data->blocked_reads) > 0);
	assert_responsive ();
	/* Cached navigation must also remain usable while an old page blocks. */
	GdkEventKey event = { .keyval = GDK_KEY_Down };
	on_key_press (viewer->drawing_area, &event, viewer);
	WAIT_FOR (viewer->page_cancel == NULL);
	g_assert_cmpint (viewer->top_offset, ==, 80);
	nemo_paged_viewer_open_location (viewer, fresh);
	WAIT_FOR (viewer->opened);
	release_gates (data);
	WAIT_FOR (g_atomic_int_get (&data->closes) >= 2);
	assert_responsive ();
	assert_first_text (viewer, "replacement");
	free_viewer (viewer);
	g_object_unref (file);
	g_object_unref (fresh);
}

static void
test_close_off_main (void)
{
	MockData *data, *replacement;
	GFile *file = new_file ("blocked close\n", 14, &data);
	GFile *fresh = new_file ("new\n", 4, &replacement);
	data->hold_close = TRUE;
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (g_atomic_int_get (&data->blocked_closes) > 0);
	nemo_paged_viewer_close_file (viewer);
	assert_responsive ();
	nemo_paged_viewer_open_location (viewer, fresh);
	WAIT_FOR (viewer->opened);
	assert_first_text (viewer, "new");
	free_viewer (viewer);
	release_gates (data);
	WAIT_FOR (g_atomic_int_get (&data->closes) == 1);
	assert_responsive ();
	g_object_unref (file);
	g_object_unref (fresh);
}

static void
test_search_cancel_and_defer (void)
{
	MockData *slow, *fast;
	GFile *file = new_file (NULL, 2 * 1024 * 1024, &slow);
	GFile *fresh = new_file ("first\nNeEdLe\nlast\n", 18, &fast);
	slow->delay_from = READ_CHUNK;
	slow->hold_read = TRUE;
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (viewer->opened);
	nemo_paged_viewer_search_set_needle (viewer, "absent");
	g_assert_true (nemo_paged_viewer_search_find_next (viewer));
	g_assert_true (nemo_paged_viewer_search_is_pending (viewer));
	WAIT_FOR (g_atomic_int_get (&slow->blocked_reads) > 0);
	assert_responsive ();
	/* Needle changes must supersede an uninterruptible old search. */
	nemo_paged_viewer_search_set_needle (viewer, "xxx");
	nemo_paged_viewer_search_find_next (viewer);
	WAIT_FOR (!nemo_paged_viewer_search_is_pending (viewer));
	g_assert_true (nemo_paged_viewer_search_has_match (viewer));
	g_assert_cmpint (viewer->search_match_offset, ==, 0);
	fast->hold_open = TRUE;
	nemo_paged_viewer_open_location (viewer, fresh);
	nemo_paged_viewer_search_set_needle (viewer, "needle");
	g_assert_true (nemo_paged_viewer_search_find_next (viewer));
	g_assert_true (nemo_paged_viewer_search_is_pending (viewer));
	WAIT_FOR (g_atomic_int_get (&fast->blocked_opens) > 0);
	release_gates (fast);
	WAIT_FOR (!nemo_paged_viewer_search_is_pending (viewer));
	g_assert_true (nemo_paged_viewer_search_has_match (viewer));
	g_assert_cmpint (viewer->search_match_offset, ==, 6);
	release_gates (slow);
	WAIT_FOR (g_atomic_int_get (&slow->closes) >= 2);
	assert_responsive ();
	g_assert_cmpint (viewer->search_match_offset, ==, 6);
	free_viewer (viewer);
	g_object_unref (file);
	g_object_unref (fresh);
}

static void
test_search_boundaries_and_wrap (void)
{
	gsize size = 2 * READ_CHUNK;
	gchar *contents = g_malloc0 (size);
	memset (contents, '.', size);
	memcpy (contents + PAGE_SIZE - 2, "AbCdEf", 6);
	memcpy (contents + READ_CHUNK - 2, "aBcDeF", 6);
	MockData *data;
	GFile *file = new_file (contents, size, &data);
	g_free (contents);
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (viewer->opened);
	nemo_paged_viewer_search_set_needle (viewer, "abcdef");
	const gint64 expected[] = { PAGE_SIZE - 2, READ_CHUNK - 2, PAGE_SIZE - 2 };
	for (guint i = 0; i < G_N_ELEMENTS (expected); i++) {
		nemo_paged_viewer_search_find_next (viewer);
		WAIT_FOR (!viewer->search_pending && viewer->page_cancel == NULL);
		g_assert_cmpint (viewer->search_match_offset, ==, expected[i]);
	}
	nemo_paged_viewer_search_find_prev (viewer);
	WAIT_FOR (!viewer->search_pending && viewer->page_cancel == NULL);
	g_assert_cmpint (viewer->search_match_offset, ==, READ_CHUNK - 2);
	nemo_paged_viewer_search_set_needle (viewer, "not in file");
	nemo_paged_viewer_search_find_next (viewer);
	WAIT_FOR (!viewer->search_pending);
	g_assert_false (nemo_paged_viewer_search_has_match (viewer));
	free_viewer (viewer);
	g_object_unref (file);
}

static void
test_large_nonseekable_hex (void)
{
	MockData *data;
	const gint64 size = (gint64) 5 * 1024 * 1024 * 1024;
	GFile *file = new_file (NULL, size, &data);
	data->seekable = FALSE;
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_set_mode (viewer, NEMO_VIEWER_MODE_HEX);
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (viewer->opened);
	g_assert_cmpint (viewer->file_size, ==, size);
	GdkEventKey end = { .keyval = GDK_KEY_End };
	on_key_press (viewer->drawing_area, &end, viewer);
	WAIT_FOR (viewer->page_cancel == NULL);
	g_assert_cmpint (viewer->top_offset, >, (gint64) 4 * 1024 * 1024 * 1024);
	g_assert_cmpuint (viewer->page->lines->len, ==, viewer->vis_lines);
	gsize cached = 0;
	for (int i = 0; i < MAX_PAGES; i++)
		if (viewer->cache.slots[i].bytes)
			cached += g_bytes_get_size (viewer->cache.slots[i].bytes);
	g_assert_cmpuint (cached, <=, MAX_PAGES * PAGE_SIZE);
	GdkEventKey home = { .keyval = GDK_KEY_Home };
	on_key_press (viewer->drawing_area, &home, viewer);
	WAIT_FOR (viewer->page_cancel == NULL);
	g_assert_cmpint (viewer->top_offset, ==, 0);
	nemo_paged_viewer_set_mode (viewer, NEMO_VIEWER_MODE_TEXT);
	WAIT_FOR (viewer->page_cancel == NULL);
	g_assert_cmpint (viewer->page->mode, ==, NEMO_VIEWER_MODE_TEXT);
	free_viewer (viewer);
	g_object_unref (file);
}

static void
test_draw_has_no_io (void)
{
	MockData *data;
	GFile *file = new_file ("text\n", 5, &data);
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (viewer->opened);
	gint reads = g_atomic_int_get (&data->reads);
	gint opens = g_atomic_int_get (&data->opens);
	cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 640, 480);
	cairo_t *cr = cairo_create (surface);
	for (int i = 0; i < 20; i++)
		on_draw (viewer->drawing_area, cr, viewer);
	g_assert_cmpint (g_atomic_int_get (&data->reads), ==, reads);
	g_assert_cmpint (g_atomic_int_get (&data->opens), ==, opens);
	cairo_destroy (cr);
	cairo_surface_destroy (surface);
	free_viewer (viewer);
	g_object_unref (file);
}

static void
test_destroy_page_and_search (void)
{
	for (int search = 0; search < 2; search++) {
		MockData *data;
		GFile *file = new_file (NULL, 2 * 1024 * 1024, &data);
		data->delay_from = READ_CHUNK;
		data->hold_read = TRUE;
		NemoPagedViewer *viewer = new_viewer ();
		nemo_paged_viewer_open_location (viewer, file);
		WAIT_FOR (viewer->opened);
		if (search) {
			nemo_paged_viewer_search_set_needle (viewer, "missing");
			nemo_paged_viewer_search_find_next (viewer);
		} else {
			gtk_adjustment_set_value (viewer->vadjust, 1024 * 1024);
		}
		WAIT_FOR (g_atomic_int_get (&data->blocked_reads) > 0);
		gpointer weak = viewer;
		g_object_add_weak_pointer (G_OBJECT (viewer), &weak);
		/* destroy with an outstanding reference must reject callbacks too. */
		gtk_widget_destroy (GTK_WIDGET (viewer));
		g_assert_true (viewer->destroyed);
		assert_responsive ();
		g_object_unref (viewer);
		g_assert_null (weak);
		release_gates (data);
		WAIT_FOR (g_atomic_int_get (&data->closes) == 2);
		assert_responsive ();
		g_object_unref (file);
	}
}

static void
test_long_needle_and_decode (void)
{
	gsize needle_len = READ_CHUNK + 33;
	gsize size = PAGE_SIZE + needle_len;
	gchar *contents = g_malloc (size);
	memset (contents, '.', PAGE_SIZE);
	memset (contents + PAGE_SIZE, 'A', needle_len);
	MockData *data;
	GFile *file = new_file (contents, size, &data);
	g_free (contents);
	gchar *needle = g_malloc (needle_len + 1);
	memset (needle, 'a', needle_len);
	needle[needle_len] = '\0';
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (viewer->opened);
	nemo_paged_viewer_search_set_needle (viewer, needle);
	g_free (needle);
	nemo_paged_viewer_search_find_next (viewer);
	WAIT_FOR (!viewer->search_pending);
	g_assert_cmpint (viewer->search_match_offset, ==, PAGE_SIZE);
	free_viewer (viewer);
	g_object_unref (file);

	const gchar invalid[] = "\xffneedle\n";
	file = new_file (invalid, sizeof (invalid) - 1, &data);
	viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (viewer->opened);
	assert_first_text (viewer, "\xef\xbf\xbdneedle");
	nemo_paged_viewer_search_set_needle (viewer, "needle");
	nemo_paged_viewer_search_find_next (viewer);
	WAIT_FOR (!viewer->search_pending && viewer->page_cancel == NULL);
	DisplayLine *line = g_ptr_array_index (viewer->page->lines, 0);
	g_assert_cmpuint (line->highlight_start, ==, 3);
	g_assert_cmpuint (line->highlight_end, ==, 9);
	free_viewer (viewer);
	g_object_unref (file);
}

static void
open_failed (NemoPagedViewer *viewer, GError *error, gpointer data)
{
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	(*(guint *) data)++;
}

static void
test_native_error (void)
{
	NemoPagedViewer *viewer = new_viewer ();
	guint failed = 0;
	g_signal_connect (viewer, "load-finished", G_CALLBACK (open_failed), &failed);
	GError *error = NULL;
	g_assert_true (nemo_paged_viewer_open_file (viewer,
		"test/nonexistent-paged-viewer-regression-fixture/no-file", &error));
	g_assert_no_error (error);
	g_assert_true (viewer->opening);
	WAIT_FOR (failed == 1);
	g_assert_false (viewer->opened);
	g_assert_nonnull (viewer->message);
	free_viewer (viewer);
}

static void
test_mode_during_open (void)
{
	MockData *data;
	GFile *file = new_file (NULL, 1024 * 1024, &data);
	data->hold_read = TRUE;
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (g_atomic_int_get (&data->blocked_reads) > 0);
	nemo_paged_viewer_set_mode (viewer, NEMO_VIEWER_MODE_HEX);
	GtkAllocation allocation = { .width = 800, .height = 600 };
	on_size_allocate (viewer->drawing_area, &allocation, viewer);
	assert_responsive ();
	release_gates (data);
	WAIT_FOR (viewer->opened && viewer->page_cancel == NULL);
	g_assert_cmpint (viewer->page->mode, ==, NEMO_VIEWER_MODE_HEX);
	g_assert_cmpuint (viewer->page->lines->len, ==, viewer->vis_lines);
	free_viewer (viewer);
	g_object_unref (file);
}

static void
test_clear_pending_match_page (void)
{
	gsize size = 2 * READ_CHUNK;
	gchar *contents = g_malloc (size);
	for (gsize i = 0; i < size; i++)
		contents[i] = i % 80 == 79 ? '\n' : 'x';
	memcpy (contents + READ_CHUNK - 10, "needle", 6);
	MockData *data;
	GFile *file = new_file (contents, size, &data);
	g_free (contents);
	data->delay_from = READ_CHUNK;
	data->hold_read = TRUE;
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (viewer->opened);
	nemo_paged_viewer_search_set_needle (viewer, "needle");
	nemo_paged_viewer_search_find_next (viewer);
	WAIT_FOR (!viewer->search_pending);
	WAIT_FOR (g_atomic_int_get (&data->blocked_reads) > 0);
	g_assert_true (viewer->page_for_search);
	g_assert_cmpint (viewer->top_offset, ==, 0);
	nemo_paged_viewer_search_clear (viewer);
	release_gates (data);
	WAIT_FOR (g_atomic_int_get (&data->closes) >= 2);
	assert_responsive ();
	g_assert_cmpint (viewer->top_offset, ==, 0);
	g_assert_false (nemo_paged_viewer_search_has_match (viewer));
	free_viewer (viewer);
	g_object_unref (file);
}

static void
test_search_read_error (void)
{
	MockData *data;
	GFile *file = new_file (NULL, 2 * READ_CHUNK, &data);
	data->fail_read = TRUE;
	data->fail_from = READ_CHUNK;
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (viewer->opened);
	nemo_paged_viewer_search_set_needle (viewer, "missing");
	nemo_paged_viewer_search_find_next (viewer);
	WAIT_FOR (!viewer->search_pending);
	const GError *error = nemo_paged_viewer_search_get_error (viewer);
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
	g_assert_cmpstr (viewer->message, ==, "Simulated device read failure");
	g_assert_false (nemo_paged_viewer_search_has_match (viewer));
	/* A subsequent successful search must clear the failure and its message. */
	data->fail_read = FALSE;
	nemo_paged_viewer_search_set_needle (viewer, "xxx");
	g_assert_null (nemo_paged_viewer_search_get_error (viewer));
	nemo_paged_viewer_search_find_next (viewer);
	WAIT_FOR (!viewer->search_pending && viewer->page_cancel == NULL);
	g_assert_null (viewer->message);
	g_assert_true (nemo_paged_viewer_search_has_match (viewer));
	free_viewer (viewer);
	g_object_unref (file);
}

static void
test_text_formatting_and_highlight (void)
{
	const gchar contents[] = "\t\303\251\001needle\r\nsecond\tline\nzero\0byte\n";
	MockData *data;
	GFile *file = new_file (contents, sizeof (contents) - 1, &data);
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (viewer->opened);
	g_assert_cmpuint (viewer->page->lines->len, ==, 3);
	assert_first_text (viewer, "\t\303\251\001needle\r");
	DisplayLine *second = g_ptr_array_index (viewer->page->lines, 1);
	DisplayLine *third = g_ptr_array_index (viewer->page->lines, 2);
	g_assert_cmpstr (second->text, ==, "second\tline");
	g_assert_cmpstr (third->text, ==, "zero\357\277\275byte");
	nemo_paged_viewer_search_set_needle (viewer, "needle");
	nemo_paged_viewer_search_find_next (viewer);
	WAIT_FOR (!viewer->search_pending && viewer->page_cancel == NULL);
	DisplayLine *first = g_ptr_array_index (viewer->page->lines, 0);
	g_assert_cmpint (viewer->search_match_offset, ==, 4);
	g_assert_cmpuint (first->highlight_start, ==, 4);
	g_assert_cmpuint (first->highlight_end, ==, 10);
	nemo_paged_viewer_set_mode (viewer, NEMO_VIEWER_MODE_HEX);
	WAIT_FOR (viewer->page_cancel == NULL);
	first = g_ptr_array_index (viewer->page->lines, 0);
	g_assert_nonnull (strstr (first->text, "09 c3 a9 01 6e 65 65 64"));
	g_assert_cmpint (viewer->page->match, ==, 4);
	free_viewer (viewer);
	g_object_unref (file);
}

static void
test_keyboard_navigation (void)
{
	GString *text = g_string_new (NULL);
	gint64 starts[100];
	for (guint i = 0; i < G_N_ELEMENTS (starts); i++) {
		starts[i] = text->len;
		g_string_append_printf (text, "line %03u\tvalue\r\n", i);
	}
	MockData *data;
	GFile *file = new_file (text->str, text->len, &data);
	g_string_free (text, TRUE);
	NemoPagedViewer *viewer = new_viewer ();
	nemo_paged_viewer_open_location (viewer, file);
	WAIT_FOR (viewer->opened);
	const guint keys[] = { GDK_KEY_Down, GDK_KEY_j, GDK_KEY_Up, GDK_KEY_k,
	                       GDK_KEY_Page_Down, GDK_KEY_Page_Up, GDK_KEY_space,
	                       GDK_KEY_Home, GDK_KEY_End };
	const guint rows[] = { 1, 2, 1, 0, 38, 0, 38, 0, 61 };
	for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
		GdkEventKey event = { .keyval = keys[i] };
		g_assert_true (on_key_press (viewer->drawing_area, &event, viewer));
		WAIT_FOR (viewer->page_cancel == NULL);
		g_assert_cmpint (viewer->top_offset, ==, starts[rows[i]]);
	}
	nemo_paged_viewer_set_mode (viewer, NEMO_VIEWER_MODE_HEX);
	WAIT_FOR (viewer->page_cancel == NULL);
	GdkEventKey down = { .keyval = GDK_KEY_Down };
	on_key_press (viewer->drawing_area, &down, viewer);
	WAIT_FOR (viewer->page_cancel == NULL);
	g_assert_cmpint (viewer->top_offset, ==, HEX_BPL);
	free_viewer (viewer);
	g_object_unref (file);
}

int
main (int argc, char **argv)
{
	if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
		g_printerr ("Run this test through test/run-isolated-regression.py\n");
		return 77;
	}
	g_setenv ("GDK_BACKEND", "x11", TRUE);
	g_unsetenv ("WAYLAND_DISPLAY");
	g_unsetenv ("AT_SPI_BUS_ADDRESS");
	g_unsetenv ("DBUS_STARTER_ADDRESS");
	g_setenv ("NO_AT_BRIDGE", "1", TRUE);
	g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
	g_test_init (&argc, &argv, NULL);
	if (!gtk_init_check (&argc, &argv))
		return 77;
	g_assert_cmpstr (gdk_display_get_name (gdk_display_get_default ()), ==, g_getenv ("DISPLAY"));
	g_test_message ("GTK display: %s", gdk_display_get_name (gdk_display_get_default ()));
	g_test_add_func ("/paged-viewer/delayed-open-switch", test_delayed_open_switch);
	g_test_add_func ("/paged-viewer/destroy-pending", test_destroy_pending);
	g_test_add_func ("/paged-viewer/delayed-page-switch", test_delayed_page_switch);
	g_test_add_func ("/paged-viewer/close-off-main", test_close_off_main);
	g_test_add_func ("/paged-viewer/search-cancel-defer", test_search_cancel_and_defer);
	g_test_add_func ("/paged-viewer/search-boundaries-wrap", test_search_boundaries_and_wrap);
	g_test_add_func ("/paged-viewer/large-nonseekable-hex", test_large_nonseekable_hex);
	g_test_add_func ("/paged-viewer/draw-no-io", test_draw_has_no_io);
	g_test_add_func ("/paged-viewer/destroy-page-search", test_destroy_page_and_search);
	g_test_add_func ("/paged-viewer/long-needle-decode", test_long_needle_and_decode);
	g_test_add_func ("/paged-viewer/native-error", test_native_error);
	g_test_add_func ("/paged-viewer/mode-during-open", test_mode_during_open);
	g_test_add_func ("/paged-viewer/clear-pending-match-page", test_clear_pending_match_page);
	g_test_add_func ("/paged-viewer/search-read-error", test_search_read_error);
	g_test_add_func ("/paged-viewer/text-formatting-highlight", test_text_formatting_and_highlight);
	g_test_add_func ("/paged-viewer/keyboard-navigation", test_keyboard_navigation);
	return g_test_run ();
}

#else

int main (void) { return 77; }

#endif
