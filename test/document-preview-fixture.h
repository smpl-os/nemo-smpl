#ifndef NEMO_DOCUMENT_PREVIEW_FIXTURE_H
#define NEMO_DOCUMENT_PREVIEW_FIXTURE_H

#include "../src/nemo-document-viewer.h"

typedef struct {
	gboolean finished;
	GError *error;
} DocumentResult;

static void
document_finished_cb (NemoDocumentViewer *viewer, GError *error, DocumentResult *result)
{
	result->finished = TRUE;
	g_clear_error (&result->error);
	if (error != NULL)
		result->error = g_error_copy (error);
}

static gboolean
document_sandbox_available (void)
{
	char *argv[] = { "bwrap", "--unshare-all", "--die-with-parent",
		"--ro-bind", "/usr", "/usr", "--symlink", "usr/lib", "/lib",
		"--symlink", "usr/lib", "/lib64", "--proc", "/proc", "--dev", "/dev",
		"--chdir", "/", "/usr/bin/true", NULL };
	GError *error = NULL;
	gint status;
	char *diagnostic = NULL;
	gboolean available = g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
	                                  NULL, &diagnostic, &status, &error);
	if (available)
		available = g_spawn_check_wait_status (status, &error);
	if (!available)
		g_test_message ("Sandbox unavailable; expecting an explicit preview error: %s %s",
		                error != NULL ? error->message : "", diagnostic != NULL ? diagnostic : "");
	g_clear_error (&error);
	g_free (diagnostic);
	return available;
}

#endif
