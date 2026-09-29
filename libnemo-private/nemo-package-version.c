/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include "nemo-package-version.h"

#include <glib/gi18n.h>
#include <string.h>

static void
cancel_query (GCancellable *cancellable, GSubprocess *process)
{
	g_subprocess_force_exit (process);
}

static int
run_query (const char * const *argv, GCancellable *cancellable,
	   char **output, char **diagnostic, GError **error)
{
	g_autoptr (GSubprocessLauncher) launcher = g_subprocess_launcher_new (
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_setenv (launcher, "LC_ALL", "C", TRUE);
	if (g_cancellable_set_error_if_cancelled (cancellable, error))
		return -1;
	g_autoptr (GSubprocess) process = g_subprocess_launcher_spawnv (launcher, argv, error);
	if (process == NULL)
		return -1;
	gulong cancelled = cancellable != NULL ?
		g_cancellable_connect (cancellable, G_CALLBACK (cancel_query), process, NULL) : 0;
	gboolean communicated = g_subprocess_communicate_utf8 (
		process, NULL, cancellable, output, diagnostic, error);
	if (cancelled != 0)
		g_cancellable_disconnect (cancellable, cancelled);
	if (!communicated)
		return -1;
	if (!g_subprocess_get_if_exited (process)) {
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			     _("Package query terminated unexpectedly: %s"), argv[0]);
		return -1;
	}
	return g_subprocess_get_exit_status (process);
}

static gboolean
single_word (const char *text)
{
	if (text == NULL || *text == '\0' || !g_utf8_validate (text, -1, NULL))
		return FALSE;
	for (const char *p = text; *p != '\0'; p++)
		if (g_ascii_isspace (*p) || g_ascii_iscntrl (*p))
			return FALSE;
	return TRUE;
}

static char *
lookup_package_version (const char *path, GCancellable *cancellable, GError **error)
{
	const char *managers[] = { "pacman", "dpkg-query", "rpm" };

	for (guint i = 0; i < G_N_ELEMENTS (managers); i++) {
		g_autofree char *program = g_find_program_in_path (managers[i]);
		g_autofree char *output = NULL;
		g_autofree char *diagnostic = NULL;
		g_autofree char *owner = NULL;
		const char *pacman_owner[] = { program, "-Qoq", "--", path, NULL };
		const char *debian_owner[] = { program, "-S", "--", path, NULL };
		const char *rpm_version[] = { program, "-qf", "--qf",
			"%{EPOCHNUM}:%{VERSION}-%{RELEASE}\\n", "--", path, NULL };
		if (program == NULL)
			continue;
		int status = run_query (i == 0 ? pacman_owner : i == 1 ? debian_owner : rpm_version,
					cancellable, &output, &diagnostic, error);
		if (status < 0)
			return NULL;
		g_strstrip (output);
		g_strstrip (diagnostic);
		if (status != 0) {
			gboolean unowned = status == 1 &&
				((i == 0 && g_str_has_prefix (diagnostic, "error: No package owns ")) ||
				 (i == 1 && g_str_has_prefix (diagnostic, "dpkg-query: no path found matching pattern ")) ||
				 (i == 2 && (strstr (output, " is not owned by any package") != NULL ||
					     strstr (diagnostic, " is not owned by any package") != NULL)));
			if (unowned)
				continue;
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
				     _("Unable to query the installed package with %s: %s"),
				     managers[i], *diagnostic != '\0' ? diagnostic : output);
			return NULL;
		}
		if (i == 0) {
			if (!single_word (output))
				goto invalid_reply;
			owner = g_strdup (output);
		} else if (i == 1) {
			char *separator = strstr (output, ": ");
			if (separator == NULL || g_strcmp0 (separator + 2, path) != 0)
				goto invalid_reply;
			owner = g_strndup (output, separator - output);
			if (!single_word (owner))
				goto invalid_reply;
		} else {
			const char *version = g_str_has_prefix (output, "0:") ? output + 2 : output;
			if (single_word (version))
				return g_strdup (version);
			goto invalid_reply;
		}
		g_clear_pointer (&output, g_free);
		g_clear_pointer (&diagnostic, g_free);
		const char *pacman_version[] = { program, "-Q", "--", owner, NULL };
		const char *debian_version[] = { program, "-W", "-f=${Version}", "--", owner, NULL };
		status = run_query (i == 0 ? pacman_version : debian_version,
				    cancellable, &output, &diagnostic, error);
		if (status < 0)
			return NULL;
		if (status != 0) {
			g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
				     _("Unable to read the installed version of %s: %s"), owner, diagnostic);
			return NULL;
		}
		g_strstrip (output);
		const char *version = output;
		if (i == 0) {
			if (!g_str_has_prefix (output, owner) || output[strlen (owner)] != ' ')
				goto invalid_reply;
			version = output + strlen (owner) + 1;
		}
		if (single_word (version))
			return g_strdup (version);
invalid_reply:
		g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			     _("Invalid package-version response from %s"), managers[i]);
		return NULL;
	}
	return NULL;
}

char *
nemo_get_package_version (GCancellable *cancellable, GError **error)
{
	g_autofree char *path = g_file_read_link ("/proc/self/exe", error);
	if (path == NULL)
		return NULL;
	gboolean replaced = g_str_has_suffix (path, " (deleted)");
	if (replaced)
		path[strlen (path) - strlen (" (deleted)")] = '\0';
	GError *query_error = NULL;
	char *version = lookup_package_version (path, cancellable, &query_error);
	if (query_error != NULL) {
		g_propagate_error (error, query_error);
		return NULL;
	}
	if (version != NULL && replaced) {
		char *message = g_strdup_printf (_("%s (restart required)"), version);
		g_free (version);
		return message;
	}
	if (version != NULL)
		return version;
	return g_strdup_printf (_("%s (unpackaged build)"), VERSION);
}
