/*
 * Isolated GTK and sandbox regressions. The controlled fixture mode below is
 * also executed inside bubblewrap: fault injection never bypasses isolation.
 * Copyright (C) 2026 smplOS contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <cairo-pdf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

static GMutex probe_lock;
static const char *fixture_mode;
static char *executable_path, *latest_work;
static gint latest_pid, prepare_count, render_count, search_count;
static gboolean sandbox_available;
static GThread *main_thread;

static GSubprocess *
probe_spawn (GSubprocessLauncher *launcher, const char * const *argv, GError **error)
{
	g_assert_true (g_thread_self () != main_thread);
	g_autoptr (GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
	gboolean unshared = FALSE, parent_death = FALSE, clean = FALSE;
	g_mutex_lock (&probe_lock);
	const char *mode = fixture_mode;
	for (int i = 0; argv[i]; i++) {
		unshared |= !strcmp (argv[i], "--unshare-all");
		parent_death |= !strcmp (argv[i], "--die-with-parent");
		clean |= !strcmp (argv[i], "--clearenv");
		if (!strcmp (argv[i], "--bind") && argv[i + 1] && argv[i + 2] &&
		    !strcmp (argv[i + 2], "/work")) {
			g_free (latest_work);
			latest_work = g_strdup (argv[i + 1]);
		}
		if (mode && !strcmp (argv[i], "--ro-bind") && argv[i + 1] && argv[i + 2] &&
		    !strcmp (argv[i + 2], "/renderer")) {
			g_ptr_array_add (args, g_strdup (argv[i++]));
			g_ptr_array_add (args, g_strdup (executable_path));
			i++;
		}
		if (mode && !strcmp (argv[i], "--")) {
			if (!strcmp (mode, "deny-sandbox"))
				g_ptr_array_add (args, g_strdup ("--intentional-test-sandbox-failure"));
			g_ptr_array_add (args, g_strdup ("--setenv"));
			g_ptr_array_add (args, g_strdup ("NEMO_DOCUMENT_TEST_CASE"));
			g_ptr_array_add (args, g_strdup (mode));
		}
		g_ptr_array_add (args, g_strdup (argv[i]));
		if (!strcmp (argv[i], "/renderer") && argv[i + 1]) {
			if (!strcmp (argv[i + 1], "prepare")) prepare_count++;
			if (!strcmp (argv[i + 1], "render") || !strcmp (argv[i + 1], "render-themed")) render_count++;
			if (!strcmp (argv[i + 1], "search")) search_count++;
		}
	}
	g_assert_true (unshared);
	g_assert_true (parent_death);
	g_assert_true (clean);
	g_assert_cmpstr (argv[0], ==, "/usr/bin/bwrap");
	g_ptr_array_add (args, NULL);
	GSubprocess *process = g_subprocess_launcher_spawnv (launcher,
		(const char * const *) args->pdata, error);
	if (process)
		latest_pid = atoi (g_subprocess_get_identifier (process));
	g_mutex_unlock (&probe_lock);
	return process;
}

#ifndef NEMO_DOCUMENT_WALL_SECONDS
#define NEMO_DOCUMENT_WALL_SECONDS 2
#endif
#define g_subprocess_launcher_spawnv probe_spawn
#include "../src/nemo-document-viewer.c"
#undef g_subprocess_launcher_spawnv

static void
write_pdf (const char *path)
{
	cairo_surface_t *surface = cairo_pdf_surface_create (path, 600, 800);
	cairo_t *cr = cairo_create (surface);
	for (int i = 0; i < 3; i++) {
		cairo_set_source_rgb (cr, 1, 1, 1);
		cairo_paint (cr);
		cairo_set_source_rgb (cr, 0, 0, 0);
		cairo_select_font_face (cr, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
		cairo_set_font_size (cr, 24);
		cairo_move_to (cr, 50, 100);
		cairo_show_text (cr, i == 1 ? "Unique lighthouse needle" : i == 2 ? "Final chapter" : "First chapter");
		cairo_show_page (cr);
	}
	cairo_destroy (cr);
	cairo_surface_finish (surface);
	g_assert_cmpint (cairo_surface_status (surface), ==, CAIRO_STATUS_SUCCESS);
	cairo_surface_destroy (surface);
}

static int
fixture_main (int argc, char **argv, const char *mode)
{
	struct rlimit limit;
	if (getrlimit (RLIMIT_CPU, &limit) || limit.rlim_cur != 20 ||
	    getrlimit (RLIMIT_AS, &limit) || limit.rlim_cur != 768 * 1024 * 1024 ||
	    getrlimit (RLIMIT_FSIZE, &limit) || limit.rlim_cur != DOCUMENT_PDF_LIMIT) {
		g_printerr ("Unexpected document child resource limits\n");
		return 91;
	}
	if (g_getenv ("DBUS_SESSION_BUS_ADDRESS") || g_file_test ("/home", G_FILE_TEST_EXISTS)) {
		g_printerr ("Unexpected host directory or session bus in sandbox\n");
		return 92;
	}
	if (!strcmp (mode, "failure")) {
		g_printerr ("Intentional unsupported encrypted fixture\n");
		return 3;
	}
	if (!strcmp (mode, "file-limit")) {
		int fd = open ("/work/oversized-output", O_WRONLY | O_CREAT, 0600);
		if (fd < 0)
			return 96;
		/* Sparse, so exercising the kernel limit consumes no large allocation. */
		if (ftruncate (fd, DOCUMENT_PDF_LIMIT + 1) == 0)
			return 97;
		close (fd);
		return 98;
	}
	if (!strcmp (mode, "slow") || (!strcmp (argv[1], "search") && !strcmp (argv[3], "slow"))) {
		/* A grandchild must die as well as the GSubprocess/bwrap process. */
		int fd = open ("/work/heartbeat", O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (fd < 0)
			return 93;
		pid_t child = fork ();
		if (child < 0)
			return 94;
		for (;;) {
			if (write (fd, "x", 1) != 1)
				_exit (95);
			usleep (20000);
		}
	}
	if (!strcmp (argv[1], "prepare") && (argc == 6 || argc == 8)) {
		write_pdf (argv[4]);
		const char *metadata = !strcmp (mode, "bad-metadata") ? "{\"page_count\":\"3\"}" : "{\"page_count\":3}";
		return g_file_set_contents (argv[5], metadata, -1, NULL) ? 0 : 4;
	}
	if ((!strcmp (argv[1], "render") || !strcmp (argv[1], "render-themed")) && argc >= 7) {
		int width = MIN (atoi (argv[4]), 600);
		int height = MIN (atoi (argv[5]), width * 4 / 3);
		if (!strcmp (mode, "bad-png"))
			return g_file_set_contents (argv[6], "not a page", -1, NULL) ? 0 : 5;
		cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24, width, height);
		cairo_t *cr = cairo_create (surface);
		cairo_set_source_rgb (cr, atoi (argv[3]) == 1 ? 0.3 : 0.9, 0.8, 0.8);
		cairo_paint (cr);
		cairo_destroy (cr);
		cairo_status_t status = cairo_surface_write_to_png (surface, argv[6]);
		cairo_surface_destroy (surface);
		return status == CAIRO_STATUS_SUCCESS ? 0 : 6;
	}
	if (!strcmp (argv[1], "search") && argc == 7) {
		const char *json = !strcmp (mode, "bad-search") ? "{\"found\":true,\"page\":30000}" :
			!strcmp (argv[3], "needle") ? "{\"found\":true,\"page\":1}" : "{\"found\":false}";
		return g_file_set_contents (argv[6], json, -1, NULL) ? 0 : 7;
	}
	g_printerr ("Unexpected fixture command %s argc=%d\n", argv[1], argc);
	return 90;
}

static void
iterate_for (unsigned milliseconds)
{
	gint64 end = g_get_monotonic_time () + milliseconds * 1000;
	while (g_get_monotonic_time () < end) {
		while (g_main_context_iteration (NULL, FALSE));
		g_usleep (1000);
	}
}

typedef struct {
	GtkWidget *window;
	NemoDocumentViewer *viewer;
	unsigned finished;
	GError *error;
} Harness;

static void
loaded (NemoDocumentViewer *viewer, const GError *error, gpointer data)
{
	Harness *h = data;
	h->finished++;
	g_clear_error (&h->error);
	if (error)
		h->error = g_error_copy (error);
}

static void
harness_init (Harness *h, const char *mode)
{
	g_mutex_lock (&probe_lock);
	fixture_mode = mode;
	prepare_count = render_count = search_count = 0;
	g_mutex_unlock (&probe_lock);
	h->window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
	h->viewer = nemo_document_viewer_new ();
	gtk_container_add (GTK_CONTAINER (h->window), GTK_WIDGET (h->viewer));
	gtk_window_set_default_size (GTK_WINDOW (h->window), 540, 400);
	g_signal_connect (h->viewer, "load-finished", G_CALLBACK (loaded), h);
	gtk_widget_show_all (h->window);
	iterate_for (80);
}

static gboolean
harness_wait (Harness *h)
{
	gint64 end = g_get_monotonic_time () + 45000000;
	while (!h->finished && g_get_monotonic_time () < end)
		iterate_for (10);
	g_assert_cmpuint (h->finished, >, 0);
	if (!sandbox_available) {
		g_assert_nonnull (h->error);
		g_assert_nonnull (strstr (h->error->message, "Sandboxed"));
		g_assert_nonnull (strstr (h->error->message, "unsandboxed preview is disabled"));
		g_assert_null (h->viewer->pixbuf);
		g_assert_cmpuint (nemo_document_viewer_get_page_count (h->viewer), ==, 0);
		g_test_message ("Sandbox denied by host; explicit fail-closed result verified");
		return FALSE;
	}
	return TRUE;
}

static void
harness_clear (Harness *h)
{
	gtk_widget_destroy (h->window);
	g_clear_error (&h->error);
	iterate_for (100);
}

static char *
fixture_path (const char *name)
{
	return g_build_filename (g_getenv ("NEMO_TEST_PROFILE"), name, NULL);
}

static void
harness_load (Harness *h, const char *name, const char *mime)
{
	g_autofree char *path = fixture_path (name);
	g_autoptr (GFile) file = g_file_new_for_path (path);
	nemo_document_viewer_load_file (h->viewer, file, mime);
}

static void
wait_render (NemoDocumentViewer *viewer)
{
	gint64 end = g_get_monotonic_time () + 45000000;
	do {
		iterate_for (20);
	} while ((viewer->render_cancel || viewer->resize_source) && g_get_monotonic_time () < end);
	g_assert_null (viewer->render_cancel);
	g_assert_cmpuint (viewer->resize_source, ==, 0);
}

static void
wait_search (NemoDocumentViewer *viewer)
{
	gint64 end = g_get_monotonic_time () + 45000000;
	while (nemo_document_viewer_search_is_pending (viewer) && g_get_monotonic_time () < end)
		iterate_for (10);
	g_assert_false (nemo_document_viewer_search_is_pending (viewer));
	wait_render (viewer);
}

static void
test_classification (void)
{
	g_autoptr (GFile) missing = g_file_new_for_uri ("sftp://unreachable.invalid/NONEXISTENT.EPUB");
	g_assert_true (nemo_document_viewer_supports_file (missing, NULL));
	g_assert_true (nemo_document_viewer_supports_file (NULL, "text/markdown"));
	g_assert_true (nemo_document_viewer_supports_file (NULL, "application/pdf"));
	g_assert_false (nemo_document_viewer_supports_file (NULL, "application/octet-stream"));
	const char *unsupported[] = { "book.azw3", "book.fb2.zip", "book.html", "book.txt" };
	for (unsigned i = 0; i < G_N_ELEMENTS (unsupported); i++) {
		g_autoptr (GFile) file = g_file_new_for_path (unsupported[i]);
		g_assert_false (nemo_document_viewer_supports_file (file, NULL));
	}
}

static void
test_real_document (void)
{
	Harness h = { 0 };
	harness_init (&h, NULL);
	harness_load (&h, "three-pages.pdf", "application/pdf");
	if (!harness_wait (&h)) {
		harness_clear (&h);
		return;
	}
	g_assert_no_error (h.error);
	g_assert_nonnull (h.viewer->pixbuf);
	g_assert_cmpuint (nemo_document_viewer_get_page_count (h.viewer), ==, 3);
	g_autofree char *cached = g_strdup (h.viewer->job->path);
	document_next_clicked (GTK_BUTTON (h.viewer->next), h.viewer);
	wait_render (h.viewer);
	g_assert_cmpuint (nemo_document_viewer_get_page (h.viewer), ==, 1);
	g_assert_cmpstr (h.viewer->job->path, ==, cached);
	g_assert_cmpint (prepare_count, ==, 1);
	nemo_document_viewer_search_set_needle (h.viewer, "lighthouse");
	g_assert_true (nemo_document_viewer_search_find_next (h.viewer));
	wait_search (h.viewer);
	g_assert_no_error ((GError *) nemo_document_viewer_search_get_error (h.viewer));
	g_assert_true (nemo_document_viewer_search_has_match (h.viewer));
	g_assert_cmpuint (nemo_document_viewer_get_page (h.viewer), ==, 1);
	g_assert_true (nemo_document_viewer_search_find_prev (h.viewer));
	wait_search (h.viewer);
	g_assert_cmpuint (nemo_document_viewer_get_page (h.viewer), ==, 1);
	nemo_document_viewer_search_set_needle (h.viewer, "no such phrase exists");
	g_assert_true (nemo_document_viewer_search_find_next (h.viewer));
	wait_search (h.viewer);
	g_assert_false (nemo_document_viewer_search_has_match (h.viewer));
	harness_clear (&h);

	harness_init (&h, NULL);
	h.finished = 0;
	harness_load (&h, "document.md", "text/markdown");
	harness_wait (&h);
	g_assert_no_error (h.error);
	g_assert_nonnull (h.viewer->pixbuf);
	nemo_document_viewer_search_set_needle (h.viewer, "lighthouse");
	nemo_document_viewer_search_find_next (h.viewer);
	wait_search (h.viewer);
	g_assert_true (nemo_document_viewer_search_has_match (h.viewer));
	harness_clear (&h);
}

static void
assert_page_background (GdkPixbuf *pixbuf, guint8 expected)
{
	g_assert_nonnull (pixbuf);
	const guchar *pixel = gdk_pixbuf_get_pixels (pixbuf);
	for (guint i = 0; i < 3; i++)
		g_assert_cmpint (ABS (pixel[i] - expected), <=, 1);
}

static void
test_reflow_theme (void)
{
	g_autofree char *path = fixture_path ("themed.md");
	GString *text = g_string_new ("# Themed book\n\n");
	for (guint i = 0; i < 60; i++)
		g_string_append_printf (text, "## Chapter %u\n\nReadable native book text using the current theme.\n\n", i);
	g_assert_true (g_file_set_contents (path, text->str, text->len, NULL));
	g_string_free (text, TRUE);
	Harness h = { 0 };
	harness_init (&h, NULL);
	GtkCssProvider *theme = gtk_css_provider_new ();
	gtk_css_provider_load_from_data (theme, "* { background-color: black; color: lime; }", -1, NULL);
	gtk_style_context_add_provider (gtk_widget_get_style_context (h.viewer->drawing),
	                                GTK_STYLE_PROVIDER (theme), GTK_STYLE_PROVIDER_PRIORITY_USER + 2);
	harness_load (&h, "themed.md", "text/markdown");
	if (!harness_wait (&h)) {
		g_object_unref (theme);
		harness_clear (&h);
		return;
	}
	g_assert_no_error (h.error);
	wait_render (h.viewer);
	g_assert_cmpstr (h.viewer->job->background, ==, "#000000");
	assert_page_background (h.viewer->pixbuf, 0);
	g_assert_cmpuint (h.viewer->pages, >, 1);
	document_next_clicked (GTK_BUTTON (h.viewer->next), h.viewer);
	wait_render (h.viewer);
	h.viewer->zoom = 1.25;
	document_queue_render (h.viewer);
	wait_render (h.viewer);
	guint page = h.viewer->page;
	int prepared = prepare_count;
	h.finished = 0;
	gtk_css_provider_load_from_data (theme, "* { background-color: white; color: #202020; }", -1, NULL);
	harness_wait (&h);
	g_assert_no_error (h.error);
	wait_render (h.viewer);
	g_assert_cmpint (prepare_count, >, prepared);
	g_assert_cmpstr (h.viewer->job->background, ==, "#ffffff");
	g_assert_cmpuint (h.viewer->page, ==, page);
	g_assert_cmpfloat (h.viewer->zoom, ==, 1.25);
	assert_page_background (h.viewer->pixbuf, 255);

	h.finished = 0;
	harness_load (&h, "three-pages.pdf", "application/pdf");
	harness_wait (&h);
	g_assert_no_error (h.error);
	wait_render (h.viewer);
	prepared = prepare_count;
	g_assert_null (h.viewer->job->background);
	gtk_css_provider_load_from_data (theme, "* { background-color: black; color: lime; }", -1, NULL);
	iterate_for (350);
	g_assert_cmpint (prepare_count, ==, prepared);
	assert_page_background (h.viewer->pixbuf, 255);
	g_object_unref (theme);
	harness_clear (&h);
}

static void
test_navigation_resize (void)
{
	Harness h = { 0 };
	harness_init (&h, "normal");
	harness_load (&h, "three-pages.pdf", "application/pdf");
	if (!harness_wait (&h)) {
		harness_clear (&h);
		return;
	}
	g_assert_no_error (h.error);
	wait_render (h.viewer);
	GtkAdjustment *v = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (h.viewer->scroll));
	g_assert_cmpfloat (gtk_adjustment_get_upper (v), >, gtk_adjustment_get_page_size (v));
	nemo_document_viewer_scroll_page (h.viewer, TRUE);
	g_assert_cmpuint (h.viewer->page, ==, 0);
	g_assert_cmpfloat (gtk_adjustment_get_value (v), >, 0);
	gtk_adjustment_set_value (v, gtk_adjustment_get_upper (v));
	nemo_document_viewer_scroll_page (h.viewer, TRUE);
	wait_render (h.viewer);
	g_assert_cmpuint (h.viewer->page, ==, 1);
	g_assert_cmpfloat (gtk_adjustment_get_value (v), ==, 0);
	nemo_document_viewer_scroll_page (h.viewer, FALSE);
	wait_render (h.viewer);
	g_assert_cmpuint (h.viewer->page, ==, 0);
	g_assert_cmpfloat_with_epsilon (gtk_adjustment_get_value (v),
		gtk_adjustment_get_upper (v) - gtk_adjustment_get_page_size (v), 1);
	GdkEventKey event = { .keyval = GDK_KEY_KP_Page_Down, .state = GDK_CONTROL_MASK };
	g_assert_false (document_key (h.viewer->drawing, &event, h.viewer));
	event.state = 0;
	g_assert_true (document_key (h.viewer->drawing, &event, h.viewer));
	wait_render (h.viewer);
	g_assert_cmpuint (h.viewer->page, ==, 1);
	double zoom = h.viewer->zoom;
	document_zoom_clicked (GTK_BUTTON (h.viewer->zoom_in), h.viewer);
	g_assert_cmpfloat (h.viewer->zoom, >, zoom);
	gtk_window_resize (GTK_WINDOW (h.window), 700, 450);
	wait_render (h.viewer);
	g_assert_cmpint (prepare_count, ==, 1);
	g_assert_cmpint (gdk_pixbuf_get_width (h.viewer->pixbuf), <=, DOCUMENT_MAX_WIDTH);
	g_assert_cmpint (gdk_pixbuf_get_height (h.viewer->pixbuf), <=, DOCUMENT_MAX_HEIGHT);
	document_zoom_clicked (GTK_BUTTON (h.viewer->fit), h.viewer);
	wait_render (h.viewer);
	g_assert_cmpfloat (h.viewer->zoom, ==, 1);
	g_assert_cmpint (prepare_count, ==, 1);
	harness_clear (&h);
}

static void
test_errors_and_retry (void)
{
	Harness h = { 0 };
	harness_init (&h, "failure");
	harness_load (&h, "three-pages.pdf", "application/pdf");
	if (!harness_wait (&h)) {
		harness_clear (&h);
		return;
	}
	g_assert_nonnull (h.error);
	g_assert_nonnull (strstr (h.error->message, "encrypted"));
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (h.viewer->counter)), ==, "— / —");
	g_assert_true (gtk_widget_get_sensitive (h.viewer->retry));
	g_assert_true (gtk_widget_get_visible (h.viewer->status));
	g_mutex_lock (&probe_lock);
	fixture_mode = "normal";
	g_mutex_unlock (&probe_lock);
	h.finished = 0;
	document_retry_clicked (GTK_BUTTON (h.viewer->retry), h.viewer);
	harness_wait (&h);
	g_assert_no_error (h.error);
	g_mutex_lock (&probe_lock);
	fixture_mode = "bad-png";
	g_mutex_unlock (&probe_lock);
	document_next_clicked (GTK_BUTTON (h.viewer->next), h.viewer);
	wait_render (h.viewer);
	g_assert_null (h.viewer->pixbuf);
	g_assert_cmpuint (h.viewer->page, ==, 1);
	g_assert_cmpstr (gtk_label_get_text (GTK_LABEL (h.viewer->counter)), ==, "2 / 3");
	g_assert_true (gtk_widget_get_sensitive (h.viewer->previous));
	g_assert_nonnull (strstr (gtk_label_get_text (GTK_LABEL (h.viewer->status)), "PNG"));
	g_mutex_lock (&probe_lock);
	fixture_mode = "bad-search";
	g_mutex_unlock (&probe_lock);
	nemo_document_viewer_search_set_needle (h.viewer, "needle");
	nemo_document_viewer_search_find_next (h.viewer);
	wait_search (h.viewer);
	g_assert_nonnull (nemo_document_viewer_search_get_error (h.viewer));
	g_assert_false (nemo_document_viewer_search_has_match (h.viewer));
	harness_clear (&h);
}

static void
test_rapid_selection_and_search (void)
{
	Harness h = { 0 };
	harness_init (&h, "normal");
	for (int i = 0; i < 30; i++)
		harness_load (&h, "three-pages.pdf", "application/pdf");
	nemo_document_viewer_search_set_needle (h.viewer, "needle");
	g_assert_true (nemo_document_viewer_search_find_next (h.viewer));
	if (!harness_wait (&h)) {
		harness_clear (&h);
		return;
	}
	g_assert_no_error (h.error);
	wait_search (h.viewer);
	g_assert_cmpuint (h.finished, ==, 1);
	g_assert_true (nemo_document_viewer_search_has_match (h.viewer));
	g_assert_cmpuint (h.viewer->page, ==, 1);
	nemo_document_viewer_search_set_needle (h.viewer, "slow");
	nemo_document_viewer_search_find_next (h.viewer);
	iterate_for (120);
	nemo_document_viewer_search_set_needle (h.viewer, "missing");
	nemo_document_viewer_search_find_next (h.viewer);
	wait_search (h.viewer);
	g_assert_false (nemo_document_viewer_search_has_match (h.viewer));
	g_assert_no_error ((GError *) nemo_document_viewer_search_get_error (h.viewer));
	g_assert_cmpstr (h.viewer->needle, ==, "missing");
	char long_query[1026];
	memset (long_query, 'a', sizeof long_query - 1);
	long_query[sizeof long_query - 1] = 0;
	nemo_document_viewer_search_set_needle (h.viewer, long_query);
	g_assert_error ((GError *) nemo_document_viewer_search_get_error (h.viewer), G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_assert_false (nemo_document_viewer_search_find_next (h.viewer));
	nemo_document_viewer_search_clear (h.viewer);
	g_assert_no_error ((GError *) nemo_document_viewer_search_get_error (h.viewer));
	harness_clear (&h);
}

static void
test_cancel_reaps_descendants (gconstpointer destroy)
{
	Harness h = { 0 };
	harness_init (&h, "slow");
	harness_load (&h, "three-pages.pdf", "application/pdf");
	if (!sandbox_available) {
		harness_wait (&h);
		harness_clear (&h);
		return;
	}
	int fd = -1, pid = 0;
	gint64 end = g_get_monotonic_time () + 10000000;
	while (fd < 0 && g_get_monotonic_time () < end) {
		iterate_for (10);
		g_mutex_lock (&probe_lock);
		if (latest_work) {
			g_autofree char *path = g_build_filename (latest_work, "heartbeat", NULL);
			fd = open (path, O_RDONLY | O_NOFOLLOW);
			pid = latest_pid;
		}
		g_mutex_unlock (&probe_lock);
	}
	g_assert_cmpint (fd, >=, 0);
	g_assert_cmpint (pid, >, 1);
	gpointer weak = h.viewer;
	g_object_add_weak_pointer (G_OBJECT (h.viewer), &weak);
	if (GPOINTER_TO_INT (destroy)) {
		gtk_widget_destroy (h.window);
		g_assert_null (weak);
	} else {
		nemo_document_viewer_close (h.viewer);
		g_assert_cmpuint (h.viewer->pages, ==, 0);
	}
	end = g_get_monotonic_time () + 5000000;
	while (kill (pid, 0) == 0 && g_get_monotonic_time () < end)
		iterate_for (10);
	g_assert_cmpint (kill (pid, 0), ==, -1);
	g_assert_cmpint (errno, ==, ESRCH);
	struct stat before, after;
	g_assert_cmpint (fstat (fd, &before), ==, 0);
	iterate_for (150);
	g_assert_cmpint (fstat (fd, &after), ==, 0);
	g_assert_cmpint (before.st_size, ==, after.st_size);
	close (fd);
	g_assert_cmpuint (h.finished, ==, 0);
	if (!GPOINTER_TO_INT (destroy)) {
		g_object_remove_weak_pointer (G_OBJECT (h.viewer), &weak);
		harness_clear (&h);
	}
}

static void
test_timeout (void)
{
	Harness h = { 0 };
	harness_init (&h, "slow");
	harness_load (&h, "three-pages.pdf", "application/pdf");
	if (harness_wait (&h)) {
		g_assert_error (h.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
		g_assert_cmpint (kill (latest_pid, 0), ==, -1);
		g_assert_cmpint (errno, ==, ESRCH);
	}
	harness_clear (&h);
}

static void
test_sandbox_fail_closed (void)
{
	Harness h = { 0 };
	harness_init (&h, "deny-sandbox");
	harness_load (&h, "three-pages.pdf", "application/pdf");
	harness_wait (&h);
	g_assert_nonnull (h.error);
	g_assert_nonnull (strstr (h.error->message, "unsandboxed preview is disabled"));
	g_assert_null (h.viewer->pixbuf);
	g_assert_cmpuint (h.viewer->pages, ==, 0);
	g_assert_cmpint (prepare_count, ==, 1);
	g_assert_cmpint (render_count, ==, 0);
	harness_clear (&h);
}

static void
test_output_file_limit (void)
{
	Harness h = { 0 };
	harness_init (&h, "file-limit");
	harness_load (&h, "three-pages.pdf", "application/pdf");
	harness_wait (&h);
	g_assert_nonnull (h.error);
	g_assert_null (h.viewer->pixbuf);
	g_assert_cmpuint (h.viewer->pages, ==, 0);
	g_assert_cmpint (render_count, ==, 0);
	harness_clear (&h);
}

static void
test_output_validation (void)
{
	g_autofree char *directory = fixture_path ("validation");
	g_assert_cmpint (g_mkdir (directory, 0700), ==, 0);
	g_autofree char *metadata = g_build_filename (directory, "metadata.json", NULL);
	g_autofree char *png = g_build_filename (directory, "page.png", NULL);
	g_autofree char *sentinel = fixture_path ("sentinel");
	g_assert_true (g_file_set_contents (sentinel, "preserve", -1, NULL));
	DocumentJob job = { 0 };
	const char *invalid[] = { "[]", "{\"page_count\":0}", "{\"page_count\":10001}",
		"{\"page_count\":3.0}", "{\"page_count\":\"3\"}", "{\"page_count\":3,\"title\":false}" };
	for (unsigned i = 0; i < G_N_ELEMENTS (invalid); i++) {
		GError *error = NULL;
		g_assert_true (g_file_set_contents (metadata, invalid[i], -1, NULL));
		g_assert_false (document_metadata (&job, directory, &error));
		g_assert_nonnull (error);
		g_error_free (error);
	}
	g_assert_cmpint (g_unlink (metadata), ==, 0);
	g_assert_cmpint (symlink (sentinel, metadata), ==, 0);
	GError *error = NULL;
	g_assert_false (document_metadata (&job, directory, &error));
	g_assert_nonnull (error);
	g_clear_error (&error);
	const guint8 oversized[] = {
		137,80,78,71,13,10,26,10,0,0,0,13,'I','H','D','R',
		0,0,16,0,0,0,16,0,8,2,0,0,0,0,0,0,0
	};
	g_assert_true (g_file_set_contents (png, (const char *) oversized, sizeof oversized, NULL));
	g_assert_null (document_read_png (directory, DOCUMENT_MAX_WIDTH, DOCUMENT_MAX_HEIGHT, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error (&error);
	g_assert_cmpint (g_unlink (png), ==, 0);
	g_assert_cmpint (mkfifo (png, 0600), ==, 0);
	g_assert_null (document_read_png (directory, DOCUMENT_MAX_WIDTH, DOCUMENT_MAX_HEIGHT, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error (&error);
	document_remove_directory (directory);
	g_autofree char *contents = NULL;
	g_assert_true (g_file_get_contents (sentinel, &contents, NULL, NULL));
	g_assert_cmpstr (contents, ==, "preserve");
	g_assert_cmpint (g_unlink (sentinel), ==, 0);
}

static void
test_input_limit (void)
{
	g_autofree char *path = fixture_path ("large.md");
	int fd = open (path, O_CREAT | O_EXCL | O_WRONLY, 0600);
	g_assert_cmpint (fd, >=, 0);
	g_assert_cmpint (ftruncate (fd, DOCUMENT_INPUT_LIMIT + 1), ==, 0);
	close (fd);
	Harness h = { 0 };
	harness_init (&h, "normal");
	harness_load (&h, "large.md", "text/markdown");
	gint64 end = g_get_monotonic_time () + 10000000;
	while (!h.finished && g_get_monotonic_time () < end)
		iterate_for (10);
	g_assert_cmpuint (h.finished, ==, 1);
	g_assert_error (h.error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
	g_assert_cmpint (prepare_count, ==, 0);
	harness_clear (&h);
	g_assert_cmpint (g_unlink (path), ==, 0);
}

static gboolean
detect_sandbox (void)
{
	g_autoptr (GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
	document_arg (args, "/usr/bin/bwrap");
	document_arg (args, "--unshare-all");
	document_arg (args, "--die-with-parent");
	document_arg (args, "--new-session");
	document_mount (args, "--ro-bind", "/usr", "/usr");
	if (!document_runtime_mount (args, "/lib", NULL) || !document_runtime_mount (args, "/lib64", NULL))
		return FALSE;
	document_arg (args, "--proc"); document_arg (args, "/proc");
	document_arg (args, "--dev"); document_arg (args, "/dev");
	document_arg (args, "/usr/bin/true");
	g_ptr_array_add (args, NULL);
	int status = 0;
	g_autofree char *stderr_text = NULL;
	gboolean spawned = g_spawn_sync (NULL, (char **) args->pdata, NULL, 0, NULL, NULL, NULL, &stderr_text, &status, NULL);
	if (!spawned || !g_spawn_check_wait_status (status, NULL)) {
		g_test_message ("Host sandbox unavailable: %s", stderr_text ? stderr_text : "bubblewrap not installed");
		return FALSE;
	}
	return TRUE;
}

int
main (int argc, char **argv)
{
	const char *mode = g_getenv ("NEMO_DOCUMENT_TEST_CASE");
	if (mode)
		return fixture_main (argc, argv, mode);
	if (!g_getenv ("NEMO_TEST_ISOLATED") || !g_getenv ("NEMO_TEST_PROFILE")) {
		g_printerr ("Run with run-isolated-regression.py, not the user's desktop\n");
		return 2;
	}
	executable_path = g_file_read_link ("/proc/self/exe", NULL);
	g_assert_nonnull (executable_path);
	main_thread = g_thread_self ();
	gtk_test_init (&argc, &argv, NULL);
	sandbox_available = detect_sandbox ();
	g_autofree char *pdf = fixture_path ("three-pages.pdf");
	g_autofree char *markdown = fixture_path ("document.md");
	write_pdf (pdf);
	g_assert_true (g_file_set_contents (markdown, "# Native Markdown\n\nA **lighthouse** in an offline document.\n", -1, NULL));
	g_test_add_func ("/document/classification", test_classification);
	g_test_add_func ("/document/native-pdf-markdown", test_real_document);
	g_test_add_func ("/document/navigation-resize-zoom", test_navigation_resize);
	g_test_add_func ("/document/reflow-theme", test_reflow_theme);
	g_test_add_func ("/document/errors-retry", test_errors_and_retry);
	g_test_add_func ("/document/rapid-selection-search", test_rapid_selection_and_search);
	g_test_add_data_func ("/document/cancel-reaps-descendants", GINT_TO_POINTER (0), test_cancel_reaps_descendants);
	g_test_add_data_func ("/document/destroy-reaps-descendants", GINT_TO_POINTER (1), test_cancel_reaps_descendants);
	g_test_add_func ("/document/wall-timeout", test_timeout);
	g_test_add_func ("/document/sandbox-fail-closed", test_sandbox_fail_closed);
	g_test_add_func ("/document/output-file-limit", test_output_file_limit);
	g_test_add_func ("/document/output-validation", test_output_validation);
	g_test_add_func ("/document/input-limit", test_input_limit);
	int result = g_test_run ();
	iterate_for (200);
	g_autofree char *cache = g_build_filename (g_get_user_cache_dir (), "nemo-document-preview", NULL);
	gint64 end = g_get_monotonic_time () + 5000000;
	gboolean empty;
	do {
		g_autoptr (GDir) directory = g_dir_open (cache, 0, NULL);
		empty = !directory || !g_dir_read_name (directory);
		if (!empty)
			iterate_for (20);
	} while (!empty && g_get_monotonic_time () < end);
	g_assert_true (empty);
	g_unlink (pdf);
	g_unlink (markdown);
	g_free (executable_path);
	g_free (latest_work);
	return result;
}
