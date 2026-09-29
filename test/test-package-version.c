#include <config.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

static char *query_program;
static const char *manager_name;
static const char *executable_path = "/usr/bin/nemo";

static char *
fixture_find_program (const char *name)
{
	return g_strcmp0 (name, manager_name) == 0 ? g_strdup (query_program) : NULL;
}

static char *
fixture_executable (const char *path, GError **error)
{
	g_assert_cmpstr (path, ==, "/proc/self/exe");
	return g_strdup (executable_path);
}

#define g_find_program_in_path fixture_find_program
#define g_file_read_link fixture_executable
#include "../libnemo-private/nemo-package-version.c"
#undef g_find_program_in_path
#undef g_file_read_link

static void
set_query (const char *manager, const char *body)
{
	g_autofree char *script = g_strconcat ("#!/bin/sh\n", body, "\n", NULL);
	GError *error = NULL;
	manager_name = manager;
	g_assert_true (g_file_set_contents (query_program, script, -1, &error));
	g_assert_no_error (error);
	g_assert_cmpint (g_chmod (query_program, 0700), ==, 0);
}

static void
assert_version (const char *expected)
{
	GError *error = NULL;
	g_autofree char *version = nemo_get_package_version (NULL, &error);
	g_assert_no_error (error);
	g_assert_cmpstr (version, ==, expected);
}

static void
test_pacman (void)
{
	set_query ("pacman", "case \"$1\" in\n"
		"-Qoq) printf 'nemo-smpl\\n';;\n"
		"-Q) printf 'nemo-smpl 9.8.7-4\\n';;\n"
		"*) exit 2;;\nesac");
	assert_version ("9.8.7-4");
	executable_path = "/usr/bin/nemo (deleted)";
	assert_version ("9.8.7-4 (restart required)");
	executable_path = "/usr/bin/nemo";
}

static void
test_debian (void)
{
	set_query ("dpkg-query", "case \"$1\" in\n"
		"-S) printf 'nemo:amd64: /usr/bin/nemo\\n';;\n"
		"-W) printf '2:9.8.7-3+smpl1';;\n"
		"*) exit 2;;\nesac");
	assert_version ("2:9.8.7-3+smpl1");
}

static void
test_rpm (void)
{
	set_query ("rpm", "printf '0:9.8.7-2.fc44\\n'");
	assert_version ("9.8.7-2.fc44");
	set_query ("rpm", "printf '1:9.8.7-2.fc44\\n'");
	assert_version ("1:9.8.7-2.fc44");
}

static void
test_unowned (void)
{
	set_query ("pacman", "echo 'error: No package owns /worktree/nemo' >&2\nexit 1");
	assert_version (VERSION " (unpackaged build)");
	manager_name = NULL;
	assert_version (VERSION " (unpackaged build)");
}

static void
test_errors (void)
{
	GError *error = NULL;
	set_query ("pacman", "echo 'error: cannot read package database' >&2\nexit 1");
	g_assert_null (nemo_get_package_version (NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
	g_assert_nonnull (strstr (error->message, "cannot read package database"));
	g_clear_error (&error);
	set_query ("pacman", "printf 'not a package version\\n'");
	g_assert_null (nemo_get_package_version (NULL, &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error (&error);
}

static void
version_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	GError *error = NULL;
	char *version = nemo_get_package_version (cancel, &error);
	if (version != NULL)
		g_task_return_pointer (task, version, g_free);
	else
		g_task_return_error (task, error);
}

static void
version_cancelled (GObject *source, GAsyncResult *result, gpointer data)
{
	GError *error = NULL;
	g_assert_null (g_task_propagate_pointer (G_TASK (result), &error));
	g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error (&error);
	*(gboolean *) data = TRUE;
}

static void
test_cancel (void)
{
	set_query ("pacman", "exec /bin/sleep 30");
	GCancellable *cancel = g_cancellable_new ();
	gboolean done = FALSE;
	GTask *task = g_task_new (NULL, cancel, version_cancelled, &done);
	g_task_run_in_thread (task, version_worker);
	g_usleep (50000);
	g_cancellable_cancel (cancel);
	gint64 deadline = g_get_monotonic_time () + 2000000;
	while (!done && g_get_monotonic_time () < deadline) {
		while (g_main_context_iteration (NULL, FALSE));
		g_usleep (1000);
	}
	g_assert_true (done);
	g_object_unref (task);
	g_object_unref (cancel);
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);
	g_autofree char *directory = g_dir_make_tmp ("nemo-package-version-XXXXXX", NULL);
	query_program = g_build_filename (directory, "package-query", NULL);
	g_test_add_func ("/package-version/pacman", test_pacman);
	g_test_add_func ("/package-version/debian", test_debian);
	g_test_add_func ("/package-version/rpm", test_rpm);
	g_test_add_func ("/package-version/unpackaged", test_unowned);
	g_test_add_func ("/package-version/errors", test_errors);
	g_test_add_func ("/package-version/cancel", test_cancel);
	int status = g_test_run ();
	g_assert_cmpint (g_remove (query_program), ==, 0);
	g_assert_cmpint (g_rmdir (directory), ==, 0);
	g_free (query_program);
	return status;
}
