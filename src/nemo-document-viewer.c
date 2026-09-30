/*
 * Copyright (C) 2026 smplOS contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>
#include "nemo-document-viewer.h"
#include "nemo-preview-utils.h"
#include <libnemo-private/nemo-ui-utilities.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef NEMO_DOCUMENT_RENDERER_PATH
#define NEMO_DOCUMENT_RENDERER_PATH LIBEXECDIR "/nemo-document-renderer"
#endif
#ifndef NEMO_DOCUMENT_WALL_SECONDS
#define NEMO_DOCUMENT_WALL_SECONDS 30
#endif

#define DOCUMENT_INPUT_LIMIT (64 * 1024 * 1024)
#define DOCUMENT_PDF_LIMIT (128 * 1024 * 1024)
#define DOCUMENT_PNG_LIMIT (32 * 1024 * 1024)
#define DOCUMENT_JSON_LIMIT (32 * 1024)
#define DOCUMENT_MAX_WIDTH 2400
#define DOCUMENT_MAX_HEIGHT 3200
#define DOCUMENT_MAX_PAGES 10000

typedef struct {
	gint refs;
	char *path;
	char *background;
	unsigned pages;
} DocumentJob;

typedef enum { DOCUMENT_PREPARE, DOCUMENT_RENDER, DOCUMENT_SEARCH } DocumentOperation;

typedef struct {
	GWeakRef viewer;
	DocumentJob *job;
	GFile *file;
	char *kind;
	char *background, *foreground;
	char *needle;
	guint64 generation, serial, search_serial;
	DocumentOperation operation;
	unsigned page;
	int width, height, direction;
} DocumentRequest;

typedef struct {
	DocumentJob *job;
	GdkPixbuf *pixbuf;
	unsigned page;
	gboolean found;
} DocumentResult;

/* Layout is deliberately independent of feature defines in callers' config.h. */
struct _NemoDocumentViewer {
	GtkBox parent;
	GtkWidget *scroll, *drawing, *status, *counter;
	GtkWidget *previous, *next, *zoom_out, *zoom_in, *fit, *retry;
	GFile *file;
	char *mime, *needle;
	char *theme_background, *theme_foreground;
	DocumentJob *job;
	GdkPixbuf *pixbuf;
	GCancellable *load_cancel, *render_cancel, *search_cancel;
	GError *search_error;
	guint64 generation, render_serial, search_serial;
	unsigned page, pages;
	guint resize_source;
	guint theme_source;
	int viewport_width, viewport_height, scroll_edge;
	double zoom, aspect;
	gboolean destroyed, loading, render_pending;
	gboolean search_pending, search_match, deferred_search, backwards;
};

G_DEFINE_TYPE (NemoDocumentViewer, nemo_document_viewer, GTK_TYPE_BOX)

enum { LOAD_FINISHED, SEARCH_CHANGED, LAST_SIGNAL };
static guint document_signals[LAST_SIGNAL];

static void document_request_render (NemoDocumentViewer *self);
static gboolean document_start_search (NemoDocumentViewer *self, gboolean backwards);
static void document_update_size (NemoDocumentViewer *self);
static void document_update_controls (NemoDocumentViewer *self);
static void document_queue_render (NemoDocumentViewer *self);
static void document_begin_load (NemoDocumentViewer *self, GFile *file, const char *mime,
                                unsigned page, double zoom, const char *needle);

static gboolean
document_is_reflowable (const char *kind)
{
	return g_strcmp0 (kind, "markdown") == 0 || g_strcmp0 (kind, "epub") == 0 ||
	       g_strcmp0 (kind, "fb2") == 0 || g_strcmp0 (kind, "mobi") == 0;
}

static char *
document_color_string (const GdkRGBA *color)
{
	return g_strdup_printf ("#%02x%02x%02x",
		(unsigned) lround (CLAMP (color->red, 0, 1) * 255),
		(unsigned) lround (CLAMP (color->green, 0, 1) * 255),
		(unsigned) lround (CLAMP (color->blue, 0, 1) * 255));
}

static void
document_theme_colors (NemoDocumentViewer *self, char **background, char **foreground)
{
	GtkStyleContext *style = gtk_widget_get_style_context (self->drawing);
	GdkRGBA bg, fg;
	*background = *foreground = NULL;
	if (!nemo_ui_get_background_color (style, GTK_STATE_FLAG_NORMAL, &bg))
		return;
	gtk_style_context_get_color (style, GTK_STATE_FLAG_NORMAL, &fg);
	fg.red = fg.red * fg.alpha + bg.red * (1 - fg.alpha);
	fg.green = fg.green * fg.alpha + bg.green * (1 - fg.alpha);
	fg.blue = fg.blue * fg.alpha + bg.blue * (1 - fg.alpha);
	*background = document_color_string (&bg);
	*foreground = document_color_string (&fg);
}

static double
document_display_width (NemoDocumentViewer *self)
{
	double width = MAX (self->viewport_width - 48, 80) * self->zoom;
	return MIN (MIN (width, DOCUMENT_MAX_WIDTH * 4.0), DOCUMENT_MAX_HEIGHT * 4.0 * self->aspect);
}

static void
document_cancel (GCancellable **cancellable)
{
	if (*cancellable != NULL) {
		g_cancellable_cancel (*cancellable);
		g_clear_object (cancellable);
	}
}

static void
document_status (NemoDocumentViewer *self, const char *message)
{
	gtk_label_set_text (GTK_LABEL (self->status), message ? message : "");
	gtk_widget_set_tooltip_text (self->status, message);
	gtk_widget_set_visible (self->status, message && *message);
}

static gboolean
document_io_error (GError **error, const char *operation)
{
	int saved_errno = errno;
	g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
		     "%s: %s", operation, g_strerror (saved_errno));
	return FALSE;
}

/* Never traverse a converter-created symlink, including during removal. */
static gboolean
document_remove_contents (int directory, unsigned depth, GError **error)
{
	DIR *entries;
	struct dirent *entry;
	int duplicate;
	gboolean success = TRUE;

	if (depth > 128) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
				     "Document cache nesting exceeds the cleanup limit");
		return FALSE;
	}
	duplicate = dup (directory);
	if (duplicate < 0)
		return document_io_error (error, "Opening document cache");
	entries = fdopendir (duplicate);
	if (!entries) {
		close (duplicate);
		return document_io_error (error, "Reading document cache");
	}
	while (TRUE) {
		struct stat st;
		errno = 0;
		entry = readdir (entries);
		if (!entry) {
			if (errno)
				success = document_io_error (error, "Reading document cache");
			break;
		}
		if (!strcmp (entry->d_name, ".") || !strcmp (entry->d_name, ".."))
			continue;
		if (fstatat (directory, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
			success = document_io_error (error, "Inspecting document cache");
			break;
		}
		if (S_ISDIR (st.st_mode)) {
			int child = openat (directory, entry->d_name,
					    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (child < 0) {
				success = document_io_error (error, "Opening document cache");
				break;
			}
			success = document_remove_contents (child, depth + 1, error);
			close (child);
			if (!success)
				break;
		}
		if (unlinkat (directory, entry->d_name, S_ISDIR (st.st_mode) ? AT_REMOVEDIR : 0) < 0) {
			success = document_io_error (error, "Removing document cache");
			break;
		}
	}
	closedir (entries);
	return success;
}

static void
document_job_free (gpointer data)
{
	DocumentJob *job = data;
	g_free (job->path);
	g_free (job->background);
	g_free (job);
}

static void
document_remove_directory (char *path)
{
	GError *error = NULL;
	if (!path)
		return;
	int directory = open (path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory < 0)
		document_io_error (&error, "Opening document cache for cleanup");
	else {
		gboolean removed = document_remove_contents (directory, 0, &error);
		close (directory);
		if (removed && g_rmdir (path) < 0)
			document_io_error (&error, "Removing document cache directory");
	}
	if (error) {
		g_warning ("Document preview cleanup: %s", error->message);
		g_error_free (error);
	}
}

typedef char DocumentDirectory;
static void
document_directory_free (DocumentDirectory *path)
{
	document_remove_directory (path);
	g_free (path);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (DocumentDirectory, document_directory_free)

static void
document_cleanup_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	DocumentJob *job = data;
	document_remove_directory (job->path);
	g_task_return_boolean (task, TRUE);
}

static DocumentJob *
document_job_ref (DocumentJob *job)
{
	if (job)
		g_atomic_int_inc (&job->refs);
	return job;
}

static void
document_cleanup_ready (GObject *source, GAsyncResult *result, gpointer unused)
{
	GError *error = NULL;
	if (!g_task_propagate_boolean (G_TASK (result), &error)) {
		g_warning ("Document preview cleanup could not run: %s", error->message);
		g_error_free (error);
	}
}

static void
document_job_unref (DocumentJob *job)
{
	if (job && g_atomic_int_dec_and_test (&job->refs)) {
		/* A main-thread last unref must not remove files or close streams. */
		GTask *task = g_task_new (NULL, NULL, document_cleanup_ready, NULL);
		g_task_set_task_data (task, job, document_job_free);
#ifdef NEMO_SMPL
		nemo_preview_run_task (task, document_cleanup_worker);
#else
		g_task_run_in_thread (task, document_cleanup_worker);
#endif
		g_object_unref (task);
	}
}

static void
document_request_free (gpointer data)
{
	DocumentRequest *request = data;
	g_weak_ref_clear (&request->viewer);
	document_job_unref (request->job);
	g_clear_object (&request->file);
	g_free (request->kind);
	g_free (request->background);
	g_free (request->foreground);
	g_free (request->needle);
	g_free (request);
}

static void
document_result_free (gpointer data)
{
	DocumentResult *result = data;
	document_job_unref (result->job);
	g_clear_object (&result->pixbuf);
	g_free (result);
}

static const char *
document_kind (GFile *file, const char *mime)
{
	static const struct { const char *mime, *kind; } types[] = {
		{ "text/markdown", "markdown" }, { "text/x-markdown", "markdown" },
		{ "application/epub+zip", "epub" },
		{ "application/x-fictionbook+xml", "fb2" },
		{ "application/x-mobipocket-ebook", "mobi" },
		{ "application/vnd.amazon.ebook", "mobi" },
		{ "application/pdf", "pdf" },
		{ "application/x-cbz", "cbz" }, { "application/vnd.comicbook+zip", "cbz" }
	};
	static const struct { const char *suffix, *kind; } suffixes[] = {
		{ ".md", "markdown" }, { ".markdown", "markdown" },
		{ ".epub", "epub" }, { ".fb2", "fb2" }, { ".mobi", "mobi" },
		{ ".pdf", "pdf" }, { ".cbz", "cbz" }
	};
	if (mime)
		for (guint i = 0; i < G_N_ELEMENTS (types); i++)
			if (!strcmp (mime, types[i].mime))
				return types[i].kind;
	if (file) {
		g_autofree char *name = g_file_get_basename (file);
		g_autofree char *lower = name ? g_ascii_strdown (name, -1) : NULL;
		for (guint i = 0; lower && i < G_N_ELEMENTS (suffixes); i++)
			if (g_str_has_suffix (lower, suffixes[i].suffix))
				return suffixes[i].kind;
	}
	return NULL;
}

gboolean
nemo_document_viewer_supports_file (GFile *file, const char *mime)
{
#if defined (HAVE_DOCUMENT_PREVIEW) && defined (NEMO_SMPL)
	return document_kind (file, mime) != NULL;
#else
	return FALSE;
#endif
}

static DocumentJob *
document_job_new (GError **error)
{
	g_autofree char *root = g_build_filename (g_get_user_cache_dir (), "nemo-document-preview", NULL);
	struct stat st;
	if (g_mkdir_with_parents (root, 0700) < 0) {
		document_io_error (error, "Creating private document cache");
		return NULL;
	}
	if (lstat (root, &st) < 0) {
		document_io_error (error, "Inspecting private document cache");
		return NULL;
	}
	if (!S_ISDIR (st.st_mode) || st.st_uid != geteuid () || (st.st_mode & 0077)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
				     "Document cache must be a private directory owned by this user");
		return NULL;
	}
	DocumentJob *job = g_new0 (DocumentJob, 1);
	job->refs = 1;
	job->path = g_build_filename (root, "job-XXXXXX", NULL);
	if (!g_mkdtemp_full (job->path, 0700)) {
		document_io_error (error, "Creating private document job");
		document_job_free (job);
		return NULL;
	}
	return job;
}

static gboolean
document_write (int fd, const char *bytes, gsize length, GError **error)
{
	while (length) {
		ssize_t n = write (fd, bytes, length);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return document_io_error (error, "Staging document");
		bytes += n;
		length -= n;
	}
	return TRUE;
}

static gboolean
document_stage (GFile *file, const char *path, GCancellable *cancel, GError **error)
{
	g_autoptr (GFileInputStream) stream = g_file_read (file, cancel, error);
	gsize total = 0;
	char buffer[65536];
	gboolean success = FALSE;
	int fd;
	if (!stream)
		return FALSE;
	fd = open (path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return document_io_error (error, "Creating staged document");
	while (!g_cancellable_set_error_if_cancelled (cancel, error)) {
		gssize n = g_input_stream_read (G_INPUT_STREAM (stream), buffer, sizeof buffer, cancel, error);
		if (n < 0)
			break;
		if (!n) {
			success = TRUE;
			break;
		}
		total += n;
		if (total > DOCUMENT_INPUT_LIMIT) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
					     "Document preview input exceeds the 64 MiB limit");
			break;
		}
		if (!document_write (fd, buffer, n, error))
			break;
	}
	if (close (fd) < 0 && success)
		success = document_io_error (error, "Closing staged document");
	return success;
}

static GBytes *
document_read_output (const char *directory, const char *name, gsize limit, gboolean prefix,
		      GError **error)
{
	struct stat st;
	int dirfd = open (directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dirfd < 0) {
		document_io_error (error, "Opening document output directory");
		return NULL;
	}
	int fd = openat (dirfd, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	close (dirfd);
	if (fd < 0) {
		document_io_error (error, "Opening document output");
		return NULL;
	}
	if (fstat (fd, &st) < 0) {
		document_io_error (error, "Inspecting document output");
		close (fd);
		return NULL;
	}
	if (!S_ISREG (st.st_mode) || st.st_size < 0 ||
	    (!prefix && (guint64) st.st_size > limit)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				     "Document renderer returned an invalid or oversized output file");
		close (fd);
		return NULL;
	}
	gsize length = MIN ((guint64) st.st_size, limit);
	char *buffer = g_malloc (length + 1);
	gsize offset = 0;
	while (offset < length) {
		ssize_t n = read (fd, buffer + offset, length - offset);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (n < 0)
				document_io_error (error, "Reading document output");
			else
				g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
						     "Document output was truncated");
			g_free (buffer);
			close (fd);
			return NULL;
		}
		offset += n;
	}
	close (fd);
	buffer[length] = 0;
	return g_bytes_new_take (buffer, length);
}

static gboolean
document_validate_pdf (const char *directory, GError **error)
{
	int dirfd = open (directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dirfd < 0)
		return document_io_error (error, "Opening prepared document");
	int fd = openat (dirfd, "document.pdf", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	close (dirfd);
	if (fd < 0)
		return document_io_error (error, "Opening prepared PDF");
	struct stat st;
	char signature[5];
	gboolean valid = fstat (fd, &st) == 0 && S_ISREG (st.st_mode) &&
		st.st_size > 5 && st.st_size <= DOCUMENT_PDF_LIMIT &&
		read (fd, signature, sizeof signature) == sizeof signature &&
		memcmp (signature, "%PDF-", sizeof signature) == 0;
	close (fd);
	if (!valid)
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				     "Document renderer returned an invalid or oversized PDF");
	return valid;
}

static JsonParser *
document_read_json (const char *directory, const char *name, GError **error)
{
	g_autoptr (GBytes) bytes = document_read_output (directory, name, DOCUMENT_JSON_LIMIT, FALSE, error);
	if (!bytes)
		return NULL;
	gsize length;
	const char *data = g_bytes_get_data (bytes, &length);
	JsonParser *parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, data, length, error)) {
		g_object_unref (parser);
		return NULL;
	}
	if (!JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser))) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				     "Document renderer returned an invalid JSON object");
		g_object_unref (parser);
		return NULL;
	}
	return parser;
}

static gboolean
document_json_integer (JsonObject *object, const char *member, gint64 minimum,
		       gint64 maximum, gint64 *value, GError **error)
{
	JsonNode *node = json_object_get_member (object, member);
	if (node && JSON_NODE_HOLDS_VALUE (node) && json_node_get_value_type (node) == G_TYPE_INT64) {
		*value = json_node_get_int (node);
		if (*value >= minimum && *value <= maximum)
			return TRUE;
	}
	g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		     "Document renderer returned an invalid '%s'", member);
	return FALSE;
}

static gboolean
document_metadata (DocumentJob *job, const char *directory, GError **error)
{
	g_autoptr (JsonParser) parser = document_read_json (directory, "metadata.json", error);
	gint64 count;
	if (!parser)
		return FALSE;
	JsonObject *object = json_node_get_object (json_parser_get_root (parser));
	if (!document_json_integer (object, "page_count", 1, DOCUMENT_MAX_PAGES, &count, error))
		return FALSE;
	const char *strings[] = { "title", "author" };
	for (guint i = 0; i < G_N_ELEMENTS (strings); i++) {
		JsonNode *node = json_object_get_member (object, strings[i]);
		if (!node)
			continue;
		if (!JSON_NODE_HOLDS_VALUE (node) || json_node_get_value_type (node) != G_TYPE_STRING ||
		    strlen (json_node_get_string (node)) > 4096) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					     "Document renderer returned invalid document metadata");
			return FALSE;
		}
	}
	job->pages = count;
	return TRUE;
}

static GdkPixbuf *
document_read_png (const char *directory, int max_width, int max_height, GError **error)
{
	static const guint8 signature[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	g_autoptr (GBytes) bytes = document_read_output (directory, "page.png", DOCUMENT_PNG_LIMIT, FALSE, error);
	if (!bytes)
		return NULL;
	gsize size;
	const guint8 *data = g_bytes_get_data (bytes, &size);
	guint32 width = 0, height = 0;
	if (size >= 33) {
		memcpy (&width, data + 16, 4);
		memcpy (&height, data + 20, 4);
		width = GUINT32_FROM_BE (width);
		height = GUINT32_FROM_BE (height);
	}
	if (size < 33 || memcmp (data, signature, 8) || memcmp (data + 8, "\0\0\0\rIHDR", 8) ||
	    width == 0 || height == 0 || width > DOCUMENT_MAX_WIDTH || height > DOCUMENT_MAX_HEIGHT ||
	    width > (guint) max_width || height > (guint) max_height || (guint64) width * height > 8000000) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				     "Document renderer returned an invalid or oversized PNG page");
		return NULL;
	}
	g_autoptr (GdkPixbufLoader) loader = gdk_pixbuf_loader_new_with_type ("png", error);
	if (!loader)
		return NULL;
	if (!gdk_pixbuf_loader_write (loader, data, size, error)) {
		gdk_pixbuf_loader_close (loader, NULL);
		return NULL;
	}
	if (!gdk_pixbuf_loader_close (loader, error))
		return NULL;
	GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf (loader);
	if (!pixbuf || gdk_pixbuf_get_width (pixbuf) != (int) width ||
	    gdk_pixbuf_get_height (pixbuf) != (int) height) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				     "Document renderer returned inconsistent PNG dimensions");
		return NULL;
	}
	return g_object_ref (pixbuf);
}

static void
document_child_limits (gpointer unused)
{
	const struct rlimit cpu = { 20, 20 };
	const struct rlimit memory = { 768 * 1024 * 1024, 768 * 1024 * 1024 };
	const struct rlimit output = { DOCUMENT_PDF_LIMIT, DOCUMENT_PDF_LIMIT };
	const struct rlimit core = { 0, 0 };
	if (setrlimit (RLIMIT_CPU, &cpu) || setrlimit (RLIMIT_AS, &memory) ||
	    setrlimit (RLIMIT_FSIZE, &output) || setrlimit (RLIMIT_CORE, &core))
		_exit (125);
}

typedef struct {
	GMainLoop *loop;
	GSubprocess *process;
	gboolean timed_out, waited;
	GError *error;
} DocumentWait;

static void
document_process_cancelled (GCancellable *cancel, gpointer data)
{
	g_subprocess_force_exit (G_SUBPROCESS (data));
}

static gboolean
document_process_timeout (gpointer data)
{
	DocumentWait *wait = data;
	wait->timed_out = TRUE;
	g_subprocess_force_exit (wait->process);
	return G_SOURCE_REMOVE;
}

static void
document_process_reaped (GObject *source, GAsyncResult *result, gpointer data)
{
	DocumentWait *wait = data;
	wait->waited = g_subprocess_wait_finish (G_SUBPROCESS (source), result, &wait->error);
	g_main_loop_quit (wait->loop);
}

static void
document_arg (GPtrArray *args, const char *value)
{
	g_ptr_array_add (args, g_strdup (value));
}

static void
document_mount (GPtrArray *args, const char *type, const char *source, const char *target)
{
	document_arg (args, type);
	document_arg (args, source);
	document_arg (args, target);
}

static gboolean
document_runtime_mount (GPtrArray *args, const char *path, GError **error)
{
	struct stat st;
	if (lstat (path, &st) < 0)
		return errno == ENOENT || document_io_error (error, "Inspecting sandbox runtime");
	if (S_ISLNK (st.st_mode)) {
		g_autofree char *target = g_file_read_link (path, NULL);
		const char *relative = target && *target == '/' ? target + 1 : target;
		if (relative && (!strcmp (relative, "usr/lib") || !strcmp (relative, "usr/lib64") ||
				 !strcmp (relative, "usr/lib32"))) {
			document_mount (args, "--symlink", target, path);
			return TRUE;
		}
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				     "Document sandbox cannot use this system library directory layout");
		return FALSE;
	}
	if (!S_ISDIR (st.st_mode)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				     "Document sandbox runtime path is not a directory");
		return FALSE;
	}
	document_mount (args, "--ro-bind", path, path);
	return TRUE;
}

static gboolean
document_run_helper (DocumentJob *job, const char *work, const char * const *command,
		     GCancellable *cancel, GError **error)
{
	if (g_cancellable_set_error_if_cancelled (cancel, error))
		return FALSE;
	if (!g_file_test ("/usr/bin/bwrap", G_FILE_TEST_IS_EXECUTABLE) ||
	    !g_file_test (NEMO_DOCUMENT_RENDERER_PATH, G_FILE_TEST_IS_EXECUTABLE)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				     "Document preview requires bubblewrap and the native document renderer; "
				     "unsandboxed preview is disabled");
		return FALSE;
	}
	g_autofree char *stderr_path = g_build_filename (work, "stderr", NULL);
	int stderr_fd = open (stderr_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (stderr_fd < 0)
		return document_io_error (error, "Creating document diagnostic file");
	g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
	g_subprocess_launcher_take_stderr_fd (launcher, stderr_fd);
	g_subprocess_launcher_set_child_setup (launcher, document_child_limits, NULL, NULL);
	const char *environment[] = { "PATH=/usr/bin", "LANG=C.UTF-8", NULL };
	g_subprocess_launcher_set_environ (launcher, (char **) environment);
	g_subprocess_launcher_set_cwd (launcher, "/");
	g_autoptr (GPtrArray) args = g_ptr_array_new_with_free_func (g_free);
	const char *base[] = { "/usr/bin/bwrap", "--unshare-all", "--die-with-parent",
		"--new-session", "--clearenv", "--cap-drop", "ALL", NULL };
	for (int i = 0; base[i]; i++)
		document_arg (args, base[i]);
	document_mount (args, "--ro-bind", "/usr", "/usr");
	if (!document_runtime_mount (args, "/lib", error) ||
	    !document_runtime_mount (args, "/lib64", error))
		return FALSE;
	document_arg (args, "--proc"); document_arg (args, "/proc");
	document_arg (args, "--dev"); document_arg (args, "/dev");
	struct stat font_directory;
	if (lstat ("/etc/fonts", &font_directory) == 0 && S_ISDIR (font_directory.st_mode))
		document_mount (args, "--ro-bind", "/etc/fonts", "/etc/fonts");
	if (lstat ("/var/cache/fontconfig", &font_directory) == 0 && S_ISDIR (font_directory.st_mode))
		document_mount (args, "--ro-bind", "/var/cache/fontconfig", "/var/cache/fontconfig");
	document_mount (args, "--ro-bind", NEMO_DOCUMENT_RENDERER_PATH, "/renderer");
	/* The prepared document is read-only. Each render/search has a separate
	 * writable directory, so cancelled and concurrent requests cannot collide. */
	if (command[0] && strcmp (command[0], "prepare") != 0)
		document_mount (args, "--ro-bind", job->path, "/document");
	document_mount (args, "--bind", work, "/work");
	document_mount (args, "--setenv", "HOME", "/work");
	document_mount (args, "--setenv", "XDG_CACHE_HOME", "/work/cache");
	document_mount (args, "--setenv", "TMPDIR", "/work");
	document_mount (args, "--setenv", "PATH", "/usr/bin");
	document_mount (args, "--setenv", "LANG", "C.UTF-8");
	document_mount (args, "--setenv", "LC_ALL", "C.UTF-8");
	document_arg (args, "--chdir"); document_arg (args, "/work");
	document_arg (args, "--"); document_arg (args, "/renderer");
	for (guint i = 0; command[i]; i++)
		document_arg (args, command[i]);
	g_ptr_array_add (args, NULL);
	g_autoptr (GSubprocess) process = g_subprocess_launcher_spawnv (launcher,
		(const char * const *) args->pdata, error);
	if (!process)
		return FALSE;

	/* Waiting is never cancellable: first kill the sandbox, then reap it.
	 * A private context keeps timeout handling entirely off the GTK thread. */
	g_autoptr (GMainContext) context = g_main_context_new ();
	g_main_context_push_thread_default (context);
	DocumentWait wait = { .process = process, .loop = g_main_loop_new (context, FALSE) };
	GSource *timeout = g_timeout_source_new_seconds (NEMO_DOCUMENT_WALL_SECONDS);
	g_source_set_callback (timeout, document_process_timeout, &wait, NULL);
	g_source_attach (timeout, context);
	gulong handler = g_cancellable_connect (cancel, G_CALLBACK (document_process_cancelled),
					       g_object_ref (process), g_object_unref);
	g_subprocess_wait_async (process, NULL, document_process_reaped, &wait);
	g_main_loop_run (wait.loop);
	g_cancellable_disconnect (cancel, handler);
	g_source_destroy (timeout);
	g_source_unref (timeout);
	g_main_loop_unref (wait.loop);
	g_main_context_pop_thread_default (context);
	if (g_cancellable_set_error_if_cancelled (cancel, error)) {
		g_clear_error (&wait.error);
		return FALSE;
	}
	if (wait.timed_out) {
		g_clear_error (&wait.error);
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
				     "Document preview exceeded its sandbox time limit");
		return FALSE;
	}
	if (!wait.waited) {
		g_propagate_error (error, wait.error);
		return FALSE;
	}
	if (!g_subprocess_get_successful (process)) {
		g_autoptr (GBytes) diagnostic = document_read_output (work, "stderr", DOCUMENT_JSON_LIMIT, TRUE, NULL);
		gsize length = 0;
		const char *text = diagnostic ? g_bytes_get_data (diagnostic, &length) : "";
		g_autofree char *message = g_utf8_make_valid (text, MIN (length, 2048));
		g_autofree char *outcome = g_subprocess_get_if_signaled (process) ?
			g_strdup_printf ("signal %d", g_subprocess_get_term_sig (process)) :
			g_strdup_printf ("exit %d", g_subprocess_get_exit_status (process));
		g_strstrip (message);
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			     "Sandboxed document preview failed (%s)%s%s. "
			     "Check bubblewrap, mutool and the document format; unsandboxed preview is disabled",
			     outcome, *message ? ": " : "", message);
		return FALSE;
	}
	return TRUE;
}

static char *
document_operation_directory (DocumentJob *job, GError **error)
{
	char *path = g_build_filename (job->path, "request-XXXXXX", NULL);
	if (!g_mkdtemp_full (path, 0700)) {
		document_io_error (error, "Creating document request directory");
		g_free (path);
		return NULL;
	}
	return path;
}

static gboolean
document_render_worker_page (DocumentRequest *request, DocumentResult *result,
			     GCancellable *cancel, GError **error)
{
	g_autoptr (DocumentDirectory) work = document_operation_directory (result->job, error);
	g_autofree char *page = g_strdup_printf ("%u", request->page);
	g_autofree char *width = g_strdup_printf ("%d", request->width);
	g_autofree char *height = g_strdup_printf ("%d", request->height);
	const char *command[] = { "render", "/document/prepare/document.pdf",
		page, width, height, "/work/page.png", request->needle, NULL };
	const char *themed_command[] = { "render-themed", "/document/prepare/document.pdf",
		page, width, height, "/work/page.png", result->job->background, request->needle, NULL };
	if (!work || !document_run_helper (result->job, work,
			result->job->background != NULL ? themed_command : command, cancel, error))
		return FALSE;
	if (g_cancellable_set_error_if_cancelled (cancel, error))
		return FALSE;
	result->pixbuf = document_read_png (work, request->width, request->height, error);
	return result->pixbuf != NULL;
}

static void G_GNUC_UNUSED
document_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	DocumentRequest *request = data;
	DocumentResult *result = g_new0 (DocumentResult, 1);
	GError *error = NULL;
	result->page = request->page;
	result->job = document_job_ref (request->job);
	if (request->operation == DOCUMENT_PREPARE) {
		g_autofree char *work = NULL;
		g_autofree char *name = NULL;
		g_autofree char *input = NULL;
		g_autofree char *sandbox_input = NULL;
		result->job = document_job_new (&error);
		if (!result->job)
			goto done;
		result->job->background = g_strdup (request->background);
		work = g_build_filename (result->job->path, "prepare", NULL);
		if (g_mkdir (work, 0700) < 0) {
			document_io_error (&error, "Creating preparation directory");
			goto done;
		}
		name = g_strconcat ("input.", !strcmp (request->kind, "markdown") ? "md" : request->kind, NULL);
		input = g_build_filename (work, name, NULL);
		sandbox_input = g_strconcat ("/work/", name, NULL);
		const char *command[] = { "prepare", request->kind, sandbox_input,
			"/work/document.pdf", "/work/metadata.json", request->background, request->foreground, NULL };
		if (!document_stage (request->file, input, cancel, &error) ||
		    !document_run_helper (result->job, work, command, cancel, &error) ||
		    !document_metadata (result->job, work, &error) ||
		    !document_validate_pdf (work, &error))
			goto done;
		request->page = MIN (request->page, result->job->pages - 1);
		result->page = request->page;
		if (!document_render_worker_page (request, result, cancel, &error))
			goto done;
	} else if (request->operation == DOCUMENT_RENDER) {
		if (!document_render_worker_page (request, result, cancel, &error))
			goto done;
	} else {
		g_autoptr (JsonParser) parser = NULL;
		g_autoptr (DocumentDirectory) work = document_operation_directory (result->job, &error);
		g_autofree char *page = g_strdup_printf ("%u", request->page);
		const char *command[] = { "search", "/document/prepare/document.pdf", request->needle,
			page, request->direction < 0 ? "-1" : "1", "/work/result.json", NULL };
		if (!work || !document_run_helper (result->job, work, command, cancel, &error))
			goto done;
		parser = document_read_json (work, "result.json", &error);
		if (!parser)
			goto done;
		JsonObject *object = json_node_get_object (json_parser_get_root (parser));
		JsonNode *found = json_object_get_member (object, "found");
		if (!found || !JSON_NODE_HOLDS_VALUE (found) || json_node_get_value_type (found) != G_TYPE_BOOLEAN) {
			g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					     "Document renderer returned an invalid search result");
			goto done;
		}
		result->found = json_node_get_boolean (found);
		if (result->found) {
			gint64 found_page;
			if (!document_json_integer (object, "page", 0, result->job->pages - 1, &found_page, &error))
				goto done;
			result->page = found_page;
		}
	}
done:
	if (!error)
		g_cancellable_set_error_if_cancelled (cancel, &error);
	if (error) {
		document_result_free (result);
		g_task_return_error (task, error);
	} else
		g_task_return_pointer (task, result, document_result_free);
}

static void
document_apply_edge (NemoDocumentViewer *self)
{
	if (!self->pixbuf || !self->scroll_edge)
		return;
	GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scroll));
	gtk_adjustment_set_value (adjustment, self->scroll_edge > 0 ? gtk_adjustment_get_lower (adjustment) :
				  MAX (gtk_adjustment_get_lower (adjustment),
				       gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_page_size (adjustment)));
}

static void
document_adjustment_changed (GtkAdjustment *adjustment, gpointer data)
{
	document_apply_edge (data);
}

static void
document_set_pixbuf (NemoDocumentViewer *self, GdkPixbuf *pixbuf)
{
	g_set_object (&self->pixbuf, pixbuf);
	self->aspect = (double) gdk_pixbuf_get_width (pixbuf) / gdk_pixbuf_get_height (pixbuf);
	document_update_size (self);
	document_apply_edge (self);
	document_status (self, NULL);
}

static void
document_ready (GObject *source, GAsyncResult *async_result, gpointer unused)
{
	GTask *task = G_TASK (async_result);
	DocumentRequest *request = g_task_get_task_data (task);
	g_autoptr (NemoDocumentViewer) self = g_weak_ref_get (&request->viewer);
	g_autoptr (GError) error = NULL;
	DocumentResult *result = g_task_propagate_pointer (task, &error);
	if (!self || self->destroyed || self->generation != request->generation ||
	    g_cancellable_is_cancelled (g_task_get_cancellable (task)))
		goto done;
	if (request->operation == DOCUMENT_PREPARE) {
		self->loading = FALSE;
		g_clear_object (&self->load_cancel);
		if (result) {
			self->job = document_job_ref (result->job);
			self->pages = result->job->pages;
			self->page = result->page;
			document_set_pixbuf (self, result->pixbuf);
			if (self->deferred_search)
				document_start_search (self, self->backwards);
			else if (self->needle)
				document_request_render (self);
			else
				document_queue_render (self);
		} else {
			self->pages = self->page = 0;
			self->search_pending = self->deferred_search = FALSE;
			document_status (self, error->message);
			if (self->needle) {
				g_clear_error (&self->search_error);
				self->search_error = g_error_copy (error);
				g_signal_emit (self, document_signals[SEARCH_CHANGED], 0);
			}
		}
		if (self->destroyed || self->generation != request->generation)
			goto done;
		document_update_controls (self);
		g_signal_emit (self, document_signals[LOAD_FINISHED], 0, error);
	} else if (request->operation == DOCUMENT_RENDER) {
		if (request->serial != self->render_serial || request->search_serial != self->search_serial)
			goto done;
		g_clear_object (&self->render_cancel);
		self->render_pending = FALSE;
		if (result)
			document_set_pixbuf (self, result->pixbuf);
		else {
			g_clear_object (&self->pixbuf);
			gtk_widget_queue_draw (self->drawing);
			document_status (self, error->message);
		}
		document_update_controls (self);
	} else {
		if (request->serial != self->search_serial)
			goto done;
		g_clear_object (&self->search_cancel);
		self->search_pending = FALSE;
		self->search_match = result && result->found;
		g_clear_error (&self->search_error);
		if (error) {
			self->search_error = g_error_copy (error);
			document_status (self, error->message);
		} else {
			if (result->found) {
				self->page = result->page;
				self->scroll_edge = 1;
				g_clear_object (&self->pixbuf);
			}
			document_request_render (self);
		}
		document_update_controls (self);
		g_signal_emit (self, document_signals[SEARCH_CHANGED], 0);
	}
done:
	if (result)
		document_result_free (result);
}

static DocumentRequest *
document_request_new (NemoDocumentViewer *self, DocumentOperation operation)
{
	DocumentRequest *request = g_new0 (DocumentRequest, 1);
	g_weak_ref_init (&request->viewer, self);
	request->operation = operation;
	request->generation = self->generation;
	request->serial = operation == DOCUMENT_SEARCH ? self->search_serial : self->render_serial;
	request->search_serial = self->search_serial;
	request->job = document_job_ref (self->job);
	request->page = self->page;
	request->needle = g_strdup (self->needle);
	double width = document_display_width (self);
	double height = width / self->aspect;
	int scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
	request->width = CLAMP ((int) ceil (width * scale), 1, DOCUMENT_MAX_WIDTH);
	request->height = CLAMP ((int) ceil (height * scale), 1, DOCUMENT_MAX_HEIGHT);
	return request;
}

static void
document_dispatch (DocumentRequest *request, GCancellable **cancel)
{
	document_cancel (cancel);
	*cancel = g_cancellable_new ();
	GTask *task = g_task_new (NULL, *cancel, document_ready, NULL);
	g_task_set_task_data (task, request, document_request_free);
#if defined (HAVE_DOCUMENT_PREVIEW) && defined (NEMO_SMPL)
	nemo_preview_run_task (task, document_worker);
#else
	g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				"Native document preview is disabled in this build");
#endif
	g_object_unref (task);
}

static void
document_update_controls (NemoDocumentViewer *self)
{
	g_autofree char *label = self->pages ? g_strdup_printf (_("%u / %u"), self->page + 1, self->pages) : g_strdup ("— / —");
	gtk_label_set_text (GTK_LABEL (self->counter), label);
	gtk_widget_set_sensitive (self->previous, self->pages && self->page > 0);
	gtk_widget_set_sensitive (self->next, self->pages && self->page + 1 < self->pages);
	gtk_widget_set_sensitive (self->zoom_out, self->pages && self->zoom > 0.25);
	gtk_widget_set_sensitive (self->zoom_in, self->pages && self->zoom < 4);
	gtk_widget_set_sensitive (self->fit, self->pages > 0);
	gtk_widget_set_sensitive (self->retry, self->file && !self->loading);
}

static void
document_update_size (NemoDocumentViewer *self)
{
	int width = ceil (document_display_width (self));
	int height = ceil (width / self->aspect);
	gtk_widget_set_size_request (self->drawing, width + 24, height + 24);
	gtk_widget_queue_draw (self->drawing);
}

static void
document_request_render (NemoDocumentViewer *self)
{
	if (self->destroyed || !self->job)
		return;
	if (self->resize_source) {
		g_source_remove (self->resize_source);
		self->resize_source = 0;
	}
	self->render_serial++;
	self->render_pending = TRUE;
	document_status (self, _("Rendering document page…"));
	DocumentRequest *request = document_request_new (self, DOCUMENT_RENDER);
	document_dispatch (request, &self->render_cancel);
	gtk_widget_queue_draw (self->drawing);
}

static gboolean
document_resize_render (gpointer data)
{
	NemoDocumentViewer *self = data;
	self->resize_source = 0;
	document_request_render (self);
	return G_SOURCE_REMOVE;
}

static void
document_queue_render (NemoDocumentViewer *self)
{
	if (!self->job || self->destroyed)
		return;
	document_cancel (&self->render_cancel);
	self->render_serial++;
	if (self->resize_source)
		g_source_remove (self->resize_source);
	self->resize_source = g_timeout_add (180, document_resize_render, self);
}

static void
document_size_allocate (GtkWidget *widget, GtkAllocation *allocation, gpointer data)
{
	NemoDocumentViewer *self = data;
	if (self->destroyed)
		return;
	if (self->viewport_width == allocation->width && self->viewport_height == allocation->height)
		return;
	gboolean width_changed = self->viewport_width != allocation->width;
	self->viewport_width = allocation->width;
	self->viewport_height = allocation->height;
	/* Fit-width rendering does not depend on height. In particular, showing
	 * the progress label must not start another render/resize cycle. */
	if (width_changed) {
		document_update_size (self);
		document_queue_render (self);
	}
}

static void
document_scale_changed (GObject *object, GParamSpec *property, gpointer data)
{
	document_queue_render (data);
}

static gboolean
document_draw (GtkWidget *widget, cairo_t *cr, gpointer data)
{
	NemoDocumentViewer *self = data;
	GtkStyleContext *style = gtk_widget_get_style_context (widget);
	int width = gtk_widget_get_allocated_width (widget);
	int height = gtk_widget_get_allocated_height (widget);
	gtk_render_background (style, cr, 0, 0, width, height);
	if (self->pixbuf) {
		double page_width = document_display_width (self);
		double page_height = page_width / self->aspect;
		double x = MAX (12, (width - page_width) / 2);
		cairo_save (cr);
		cairo_translate (cr, x, 12);
		cairo_rectangle (cr, 0, 0, page_width, page_height);
		cairo_set_source_rgb (cr, 1, 1, 1);
		cairo_fill (cr);
		cairo_scale (cr, page_width / gdk_pixbuf_get_width (self->pixbuf),
			     page_height / gdk_pixbuf_get_height (self->pixbuf));
		gdk_cairo_set_source_pixbuf (cr, self->pixbuf, 0, 0);
		cairo_pattern_set_filter (cairo_get_source (cr), CAIRO_FILTER_BILINEAR);
		cairo_paint (cr);
		cairo_restore (cr);
	}
	if (gtk_widget_has_focus (widget))
		gtk_render_focus (style, cr, 1, 1, width - 2, height - 2);
	return FALSE;
}

static void
document_change_page (NemoDocumentViewer *self, unsigned page, int edge)
{
	if (!self->job || page >= self->pages)
		return;
	document_cancel (&self->search_cancel);
	self->search_serial++;
	self->search_match = self->search_pending = self->deferred_search = FALSE;
	g_clear_error (&self->search_error);
	self->page = page;
	self->scroll_edge = edge;
	g_clear_object (&self->pixbuf);
	document_update_controls (self);
	document_request_render (self);
	g_signal_emit (self, document_signals[SEARCH_CHANGED], 0);
}

void
nemo_document_viewer_scroll_page (NemoDocumentViewer *self, gboolean forward)
{
	g_return_if_fail (NEMO_IS_DOCUMENT_VIEWER (self));
	if (self->destroyed || !self->pages)
		return;
	self->scroll_edge = 0;
	GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scroll));
	double value = gtk_adjustment_get_value (adjustment);
	double lower = gtk_adjustment_get_lower (adjustment);
	double size = gtk_adjustment_get_page_size (adjustment);
	double end = MAX (lower, gtk_adjustment_get_upper (adjustment) - size);
	if (forward && value < end - 1)
		gtk_adjustment_set_value (adjustment, MIN (end, value + MAX (size * 0.9, 1)));
	else if (!forward && value > lower + 1)
		gtk_adjustment_set_value (adjustment, MAX (lower, value - MAX (size * 0.9, 1)));
	else if (forward && self->page + 1 < self->pages)
		document_change_page (self, self->page + 1, 1);
	else if (!forward && self->page > 0)
		document_change_page (self, self->page - 1, -1);
}

static gboolean
document_key (GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	if (event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK))
		return FALSE;
	switch (event->keyval) {
	case GDK_KEY_Page_Up:
	case GDK_KEY_KP_Page_Up:
		nemo_document_viewer_scroll_page (data, FALSE);
		return TRUE;
	case GDK_KEY_Page_Down:
	case GDK_KEY_KP_Page_Down:
		nemo_document_viewer_scroll_page (data, TRUE);
		return TRUE;
	case GDK_KEY_space:
	case GDK_KEY_KP_Space:
		nemo_document_viewer_scroll_page (data, !(event->state & GDK_SHIFT_MASK));
		return TRUE;
	default:
		return FALSE;
	}
}

static gboolean
document_button_press (GtkWidget *widget, GdkEventButton *event, gpointer data)
{
	gtk_widget_grab_focus (widget);
	return FALSE;
}

static gboolean
document_scroll (GtkWidget *widget, GdkEventScroll *event, gpointer data)
{
	((NemoDocumentViewer *) data)->scroll_edge = 0;
	return FALSE;
}

static void
document_previous_clicked (GtkButton *button, gpointer data)
{
	NemoDocumentViewer *self = data;
	if (self->page)
		document_change_page (self, self->page - 1, 1);
}

static void
document_next_clicked (GtkButton *button, gpointer data)
{
	NemoDocumentViewer *self = data;
	if (self->page + 1 < self->pages)
		document_change_page (self, self->page + 1, 1);
}

static void
document_zoom_clicked (GtkButton *button, gpointer data)
{
	NemoDocumentViewer *self = data;
	if (self->destroyed)
		return;
	if (GTK_WIDGET (button) == self->fit)
		self->zoom = 1;
	else
		self->zoom = CLAMP (self->zoom * (GTK_WIDGET (button) == self->zoom_in ? 1.25 : 0.8), 0.25, 4);
	self->scroll_edge = 0;
	document_update_size (self);
	document_update_controls (self);
	document_queue_render (self);
}

static void
document_retry_clicked (GtkButton *button, gpointer data)
{
	NemoDocumentViewer *self = data;
	if (self->job)
		document_request_render (self);
	else if (self->file) {
		g_autoptr (GFile) file = g_object_ref (self->file);
		g_autofree char *mime = g_strdup (self->mime);
		nemo_document_viewer_load_file (self, file, mime);
	}
}

static void
document_reset (NemoDocumentViewer *self)
{
	self->generation++;
	self->render_serial++;
	self->search_serial++;
	document_cancel (&self->load_cancel);
	document_cancel (&self->render_cancel);
	document_cancel (&self->search_cancel);
	if (self->resize_source) {
		g_source_remove (self->resize_source);
		self->resize_source = 0;
	}
	if (self->theme_source) {
		g_source_remove (self->theme_source);
		self->theme_source = 0;
	}
	g_clear_object (&self->file);
	g_clear_object (&self->pixbuf);
	g_clear_pointer (&self->mime, g_free);
	g_clear_pointer (&self->needle, g_free);
	g_clear_pointer (&self->theme_background, g_free);
	g_clear_pointer (&self->theme_foreground, g_free);
	g_clear_error (&self->search_error);
	g_clear_pointer (&self->job, document_job_unref);
	self->page = self->pages = 0;
	self->loading = self->render_pending = FALSE;
	self->search_match = self->search_pending = self->deferred_search = FALSE;
	self->zoom = 1;
	self->aspect = 0.75;
	self->scroll_edge = 1;
}

void
nemo_document_viewer_close (NemoDocumentViewer *self)
{
	g_return_if_fail (NEMO_IS_DOCUMENT_VIEWER (self));
	document_reset (self);
	if (!self->destroyed) {
		document_status (self, NULL);
		document_update_controls (self);
		gtk_widget_set_size_request (self->drawing, 1, 1);
		gtk_widget_queue_draw (self->drawing);
		g_signal_emit (self, document_signals[SEARCH_CHANGED], 0);
	}
}

static void
document_begin_load (NemoDocumentViewer *self, GFile *file, const char *mime,
                     unsigned page, double zoom, const char *needle)
{
	g_return_if_fail (NEMO_IS_DOCUMENT_VIEWER (self));
	g_return_if_fail (G_IS_FILE (file));
	if (self->destroyed)
		return;
	g_autoptr (GFile) location = g_object_ref (file);
	g_autofree char *mime_copy = g_strdup (mime);
	g_autofree char *needle_copy = g_strdup (needle);
	document_reset (self);
	self->file = g_steal_pointer (&location);
	self->mime = g_steal_pointer (&mime_copy);
	self->needle = g_steal_pointer (&needle_copy);
	self->page = page;
	self->zoom = zoom;
	self->deferred_search = self->needle != NULL;
	self->search_pending = self->deferred_search;
	if (document_is_reflowable (document_kind (self->file, self->mime)))
		document_theme_colors (self, &self->theme_background, &self->theme_foreground);
	self->loading = TRUE;
	document_status (self, _("Preparing document preview…"));
	document_update_controls (self);
	gtk_widget_queue_draw (self->drawing);
	DocumentRequest *request = document_request_new (self, DOCUMENT_PREPARE);
	request->file = g_object_ref (self->file);
	request->kind = g_strdup (document_kind (self->file, self->mime));
	request->background = g_strdup (self->theme_background);
	request->foreground = g_strdup (self->theme_foreground);
	if (!request->kind) {
		request->kind = g_strdup ("unsupported");
	}
	document_dispatch (request, &self->load_cancel);
	g_signal_emit (self, document_signals[SEARCH_CHANGED], 0);
}

void
nemo_document_viewer_load_file (NemoDocumentViewer *self, GFile *file, const char *mime)
{
	document_begin_load (self, file, mime, 0, 1, NULL);
}

static gboolean
document_check_theme (gpointer data)
{
	NemoDocumentViewer *self = data;
	self->theme_source = 0;
	if (self->destroyed || self->file == NULL || !document_is_reflowable (document_kind (self->file, self->mime)))
		return G_SOURCE_REMOVE;
	g_autofree char *background = NULL;
	g_autofree char *foreground = NULL;
	document_theme_colors (self, &background, &foreground);
	if (g_strcmp0 (background, self->theme_background) != 0 ||
	    g_strcmp0 (foreground, self->theme_foreground) != 0)
		document_begin_load (self, self->file, self->mime, self->page, self->zoom, self->needle);
	return G_SOURCE_REMOVE;
}

static void
document_style_changed (GtkWidget *drawing, NemoDocumentViewer *self)
{
	if (!self->destroyed && self->file != NULL && self->theme_source == 0)
		self->theme_source = g_timeout_add (180, document_check_theme, self);
}

void
nemo_document_viewer_search_set_needle (NemoDocumentViewer *self, const char *needle)
{
	g_return_if_fail (NEMO_IS_DOCUMENT_VIEWER (self));
	if (self->destroyed)
		return;
	document_cancel (&self->search_cancel);
	document_cancel (&self->render_cancel);
	self->search_serial++;
	self->render_serial++;
	g_free (self->needle);
	self->needle = NULL;
	g_clear_error (&self->search_error);
	self->search_match = self->search_pending = self->deferred_search = FALSE;
	if (needle && *needle) {
		if (strlen (needle) > 1024 || !g_utf8_validate (needle, -1, NULL))
			self->search_error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				_("Document search must be valid UTF-8 and at most 1024 bytes"));
		else
			self->needle = g_strdup (needle);
	}
	if (self->job) {
		g_clear_object (&self->pixbuf);
		document_request_render (self);
	}
	g_signal_emit (self, document_signals[SEARCH_CHANGED], 0);
}

static gboolean
document_start_search (NemoDocumentViewer *self, gboolean backwards)
{
	if (self->destroyed || !self->needle || (!self->job && !self->loading))
		return FALSE;
	document_cancel (&self->search_cancel);
	document_cancel (&self->render_cancel);
	self->render_pending = FALSE;
	self->search_serial++;
	g_clear_error (&self->search_error);
	self->search_pending = TRUE;
	self->backwards = backwards;
	self->deferred_search = self->loading;
	if (self->job) {
		DocumentRequest *request = document_request_new (self, DOCUMENT_SEARCH);
		request->direction = backwards ? -1 : 1;
		if (self->search_match)
			request->page = backwards ? (self->page + self->pages - 1) % self->pages :
						    (self->page + 1) % self->pages;
		document_dispatch (request, &self->search_cancel);
	}
	g_signal_emit (self, document_signals[SEARCH_CHANGED], 0);
	return TRUE;
}

gboolean
nemo_document_viewer_search_find_next (NemoDocumentViewer *self)
{
	g_return_val_if_fail (NEMO_IS_DOCUMENT_VIEWER (self), FALSE);
	return document_start_search (self, FALSE);
}

gboolean
nemo_document_viewer_search_find_prev (NemoDocumentViewer *self)
{
	g_return_val_if_fail (NEMO_IS_DOCUMENT_VIEWER (self), FALSE);
	return document_start_search (self, TRUE);
}

void
nemo_document_viewer_search_clear (NemoDocumentViewer *self)
{
	nemo_document_viewer_search_set_needle (self, NULL);
}

gboolean
nemo_document_viewer_search_has_match (NemoDocumentViewer *self)
{
	g_return_val_if_fail (NEMO_IS_DOCUMENT_VIEWER (self), FALSE);
	return self->search_match;
}

gboolean
nemo_document_viewer_search_is_pending (NemoDocumentViewer *self)
{
	g_return_val_if_fail (NEMO_IS_DOCUMENT_VIEWER (self), FALSE);
	return self->search_pending;
}

const GError *
nemo_document_viewer_search_get_error (NemoDocumentViewer *self)
{
	g_return_val_if_fail (NEMO_IS_DOCUMENT_VIEWER (self), NULL);
	return self->search_error;
}

unsigned
nemo_document_viewer_get_page_count (NemoDocumentViewer *self)
{
	g_return_val_if_fail (NEMO_IS_DOCUMENT_VIEWER (self), 0);
	return self->pages;
}

unsigned
nemo_document_viewer_get_page (NemoDocumentViewer *self)
{
	g_return_val_if_fail (NEMO_IS_DOCUMENT_VIEWER (self), 0);
	return self->page;
}

static GtkWidget *
document_control (GtkWidget *box, const char *icon, const char *tooltip, GCallback callback,
		  NemoDocumentViewer *self)
{
	GtkWidget *button = gtk_button_new_from_icon_name (icon, GTK_ICON_SIZE_MENU);
	gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text (button, tooltip);
	gtk_box_pack_start (GTK_BOX (box), button, FALSE, FALSE, 0);
	g_signal_connect (button, "clicked", callback, self);
	return button;
}

static void
nemo_document_viewer_init (NemoDocumentViewer *self)
{
	self->zoom = 1;
	self->aspect = 0.75;
	self->viewport_width = 600;
	self->viewport_height = 800;
	gtk_orientable_set_orientation (GTK_ORIENTABLE (self), GTK_ORIENTATION_VERTICAL);
	GtkWidget *controls = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
	self->previous = document_control (controls, "go-previous-symbolic", _("Previous document page"),
		G_CALLBACK (document_previous_clicked), self);
	self->counter = gtk_label_new ("— / —");
	gtk_box_pack_start (GTK_BOX (controls), self->counter, TRUE, TRUE, 2);
	self->next = document_control (controls, "go-next-symbolic", _("Next document page"),
		G_CALLBACK (document_next_clicked), self);
	self->zoom_out = document_control (controls, "zoom-out-symbolic", _("Zoom out"),
		G_CALLBACK (document_zoom_clicked), self);
	self->zoom_in = document_control (controls, "zoom-in-symbolic", _("Zoom in"),
		G_CALLBACK (document_zoom_clicked), self);
	self->fit = document_control (controls, "zoom-fit-best-symbolic", _("Fit document width"),
		G_CALLBACK (document_zoom_clicked), self);
	self->retry = document_control (controls, "view-refresh-symbolic", _("Retry document preview"),
		G_CALLBACK (document_retry_clicked), self);
	gtk_box_pack_start (GTK_BOX (self), controls, FALSE, FALSE, 0);
	self->status = gtk_label_new (NULL);
	gtk_label_set_line_wrap (GTK_LABEL (self->status), TRUE);
	gtk_label_set_line_wrap_mode (GTK_LABEL (self->status), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_max_width_chars (GTK_LABEL (self->status), 60);
	gtk_label_set_lines (GTK_LABEL (self->status), 4);
	gtk_label_set_ellipsize (GTK_LABEL (self->status), PANGO_ELLIPSIZE_END);
	gtk_widget_set_no_show_all (self->status, TRUE);
	gtk_box_pack_start (GTK_BOX (self), self->status, FALSE, FALSE, 4);
	self->scroll = gtk_scrolled_window_new (NULL, NULL);
	gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (self->scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (self->scroll), GTK_SHADOW_IN);
	gtk_widget_set_hexpand (self->scroll, TRUE);
	gtk_widget_set_vexpand (self->scroll, TRUE);
	self->drawing = gtk_drawing_area_new ();
	gtk_style_context_add_class (gtk_widget_get_style_context (self->drawing), GTK_STYLE_CLASS_VIEW);
	gtk_widget_set_can_focus (self->drawing, TRUE);
	gtk_widget_add_events (self->drawing, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
	gtk_container_add (GTK_CONTAINER (self->scroll), self->drawing);
	gtk_box_pack_start (GTK_BOX (self), self->scroll, TRUE, TRUE, 0);
	g_signal_connect (self->drawing, "draw", G_CALLBACK (document_draw), self);
	g_signal_connect (self->drawing, "style-updated", G_CALLBACK (document_style_changed), self);
	g_signal_connect (self->drawing, "key-press-event", G_CALLBACK (document_key), self);
	g_signal_connect (self->drawing, "button-press-event", G_CALLBACK (document_button_press), self);
	g_signal_connect (self->drawing, "scroll-event", G_CALLBACK (document_scroll), self);
	g_signal_connect (self->scroll, "size-allocate", G_CALLBACK (document_size_allocate), self);
	g_signal_connect (self, "notify::scale-factor", G_CALLBACK (document_scale_changed), self);
	g_signal_connect (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scroll)),
			  "changed", G_CALLBACK (document_adjustment_changed), self);
	document_update_controls (self);
	gtk_widget_show_all (controls);
	gtk_widget_show_all (self->scroll);
}

static void
nemo_document_viewer_destroy (GtkWidget *widget)
{
	NemoDocumentViewer *self = NEMO_DOCUMENT_VIEWER (widget);
	self->destroyed = TRUE;
	document_reset (self);
	GTK_WIDGET_CLASS (nemo_document_viewer_parent_class)->destroy (widget);
}

static void
nemo_document_viewer_finalize (GObject *object)
{
	document_reset (NEMO_DOCUMENT_VIEWER (object));
	G_OBJECT_CLASS (nemo_document_viewer_parent_class)->finalize (object);
}

static void
nemo_document_viewer_class_init (NemoDocumentViewerClass *klass)
{
	GTK_WIDGET_CLASS (klass)->destroy = nemo_document_viewer_destroy;
	G_OBJECT_CLASS (klass)->finalize = nemo_document_viewer_finalize;
	document_signals[LOAD_FINISHED] = g_signal_new ("load-finished", G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);
	document_signals[SEARCH_CHANGED] = g_signal_new ("search-changed", G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

NemoDocumentViewer *
nemo_document_viewer_new (void)
{
	return g_object_new (NEMO_TYPE_DOCUMENT_VIEWER, NULL);
}
