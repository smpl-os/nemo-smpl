/* Run only through run-isolated-regression.py. Including the implementation
 * exposes lifecycle state without adding test-only production entry points. */
#include "../src/nemo-smpl-theme.c"

#include <glib/gstdio.h>
#include <math.h>

#define WAIT_FOR(condition) G_STMT_START { \
    gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND; \
    while (!(condition) && g_get_monotonic_time () < deadline) { \
        while (g_main_context_iteration (NULL, FALSE)); \
        g_usleep (1000); \
    } \
    g_assert_true (condition); \
} G_STMT_END

static char *test_root;

static void
write_css (const char *path, const char *contents)
{
    GError *error = NULL;
    g_assert_true (g_file_set_contents (path, contents, -1, &error));
    g_assert_no_error (error);
}

static double
background_alpha (GtkWidget *widget)
{
    GdkRGBA *color = NULL;
    double alpha;

    gtk_style_context_get (gtk_widget_get_style_context (widget),
                           GTK_STATE_FLAG_NORMAL, "background-color", &color, NULL);
    g_assert_nonnull (color);
    alpha = color->alpha;
    gdk_rgba_free (color);
    return alpha;
}

static void
wait_for_reload (void)
{
    WAIT_FOR (reload_timer != 0);
    WAIT_FOR (reload_timer == 0);
}

static void
assert_watch_path (guint index, const char *path)
{
    ThemeWatch *watch = &theme_watches[index];
    GFile *directory = g_file_new_for_path (path);
    g_assert_nonnull (watch->monitor);
    g_assert_false (g_file_monitor_is_cancelled (watch->monitor));
    g_assert_true (g_file_equal (directory, watch->directory));
    g_object_unref (directory);
}

static void
assert_watching (const char *path)
{
    assert_watch_path (0, path);
}

static void
test_theme_lifecycle (void)
{
    const char *opaque = "#smpl-theme-probe { background-color: rgba(10,20,30,1); }";
    const char *transparent = "#smpl-theme-probe { background-color: rgba(10,20,30,.55); }";
    char *directory = g_build_filename (g_get_user_config_dir (), "smplos", NULL);
    char *path = g_build_filename (directory, "nemo-theme.css", NULL);
    char *replacement = g_build_filename (directory, "replacement.css", NULL);
    char *imported = g_build_filename (directory, "imported.css", NULL);
    char *moved = g_build_filename (g_get_user_config_dir (), "smplos-moved", NULL);
    char *moved_css = g_build_filename (moved, "nemo-theme.css", NULL);
    GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    GtkCssProvider *fallback = gtk_css_provider_new ();
    GtkCssProvider *previous;
    GFileMonitor *monitor;
    guint pending;
    GError *error = NULL;

    gtk_widget_set_name (window, "smpl-theme-probe");
    g_assert_true (gtk_css_provider_load_from_data (
        fallback, "#smpl-theme-probe { background-color: rgba(10,20,30,.8);"
                  "background-image: none; transition: none; }", -1, &error));
    g_assert_no_error (error);
    gtk_style_context_add_provider_for_screen (
        gdk_screen_get_default (), GTK_STYLE_PROVIDER (fallback),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_widget_show (window);
    WAIT_FOR (fabs (background_alpha (window) - .8) < .0001);

    nemo_smpl_theme_init ();
    g_assert_null (theme_provider);
    g_assert_false (g_file_test (g_get_user_config_dir (), G_FILE_TEST_EXISTS));
    assert_watching (test_root);
    nemo_smpl_theme_init ();

    /* Configuration and theme directories may each appear after startup. */
    g_assert_cmpint (g_mkdir (g_get_user_config_dir (), 0700), ==, 0);
    wait_for_reload ();
    assert_watching (g_get_user_config_dir ());
    g_assert_false (g_file_test (directory, G_FILE_TEST_EXISTS));
    g_assert_cmpint (g_mkdir (directory, 0700), ==, 0);
    write_css (path, opaque);
    wait_for_reload ();
    assert_watching (directory);
    g_assert_nonnull (theme_provider);
    WAIT_FOR (fabs (background_alpha (window) - 1.) < .0001);

    /* Atomic rename must create a fresh stylesheet, including alpha changes. */
    previous = g_object_ref (theme_provider);
    write_css (replacement, transparent);
    g_assert_cmpint (g_rename (replacement, path), ==, 0);
    wait_for_reload ();
    g_assert_true (theme_provider != previous);
    g_object_unref (previous);
    WAIT_FOR (fabs (background_alpha (window) - .55) < .0001);

    previous = g_object_ref (theme_provider);
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*failed to load nemo-theme.css:*");
    write_css (path, "#smpl-theme-probe { background-color: definitely-not-a-color; }");
    wait_for_reload ();
    g_test_assert_expected_messages ();
    g_assert_true (theme_provider == previous);
    g_assert_cmpfloat_with_epsilon (background_alpha (window), .55, .0001);

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*empty nemo-theme.css*retaining*");
    write_css (path, "");
    wait_for_reload ();
    g_test_assert_expected_messages ();
    g_assert_true (theme_provider == previous);
    g_assert_cmpfloat_with_epsilon (background_alpha (window), .55, .0001);

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*empty nemo-theme.css*retaining*");
    write_css (path, " \n/* writer has not supplied any rules yet */\n");
    wait_for_reload ();
    g_test_assert_expected_messages ();
    g_assert_true (theme_provider == previous);
    g_object_unref (previous);

    g_assert_cmpint (g_unlink (path), ==, 0);
    wait_for_reload ();
    g_assert_null (theme_provider);
    WAIT_FOR (fabs (background_alpha (window) - .8) < .0001);

    /* An unreadable CSS source must warn, not masquerade as deletion. */
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*failed to load nemo-theme.css:*");
    g_assert_cmpint (g_mkdir (path, 0700), ==, 0);
    wait_for_reload ();
    g_test_assert_expected_messages ();
    g_assert_null (theme_provider);
    g_assert_cmpint (g_rmdir (path), ==, 0);
    wait_for_reload ();

    write_css (imported, transparent);
    write_css (path, "@import url(\"imported.css\");");
    wait_for_reload ();
    WAIT_FOR (fabs (background_alpha (window) - .55) < .0001);
    g_assert_cmpint (g_unlink (path), ==, 0);
    wait_for_reload ();
    g_assert_cmpint (g_unlink (imported), ==, 0);

    /* Rename into an absent target and then rename the entire watched folder. */
    write_css (replacement, transparent);
    g_assert_cmpint (g_rename (replacement, path), ==, 0);
    wait_for_reload ();
    WAIT_FOR (fabs (background_alpha (window) - .55) < .0001);
    g_assert_cmpint (g_rename (directory, moved), ==, 0);
    wait_for_reload ();
    assert_watching (g_get_user_config_dir ());
    g_assert_null (theme_provider);
    WAIT_FOR (fabs (background_alpha (window) - .8) < .0001);
    g_assert_cmpint (g_rename (moved, directory), ==, 0);
    wait_for_reload ();
    assert_watching (directory);
    WAIT_FOR (fabs (background_alpha (window) - .55) < .0001);

    /* Recreating the watched directory before the debounce fires must not
     * leave a monitor attached to the old inode at the same pathname. */
    g_assert_cmpint (g_rename (directory, moved), ==, 0);
    g_assert_cmpint (g_mkdir (directory, 0700), ==, 0);
    write_css (path, opaque);
    wait_for_reload ();
    WAIT_FOR (fabs (background_alpha (window) - 1.) < .0001);
    write_css (path, transparent);
    wait_for_reload ();
    WAIT_FOR (fabs (background_alpha (window) - .55) < .0001);
    g_assert_cmpint (g_unlink (moved_css), ==, 0);
    g_assert_cmpint (g_rmdir (moved), ==, 0);

    monitor = g_object_ref (theme_watches[0].monitor);
    write_css (path, opaque);
    WAIT_FOR (reload_timer != 0);
    pending = reload_timer;
    nemo_smpl_theme_shutdown ();
    g_assert_true (g_file_monitor_is_cancelled (monitor));
    g_object_unref (monitor);
    g_assert_null (g_main_context_find_source_by_id (NULL, pending));
    g_assert_cmpuint (reload_timer, ==, 0);
    g_assert_null (theme_provider);
    for (guint i = 0; i < G_N_ELEMENTS (theme_watches); i++) {
        g_assert_null (theme_watches[i].monitor);
        g_assert_null (theme_watches[i].file);
    }
    g_assert_null (theme_screen);
    WAIT_FOR (fabs (background_alpha (window) - .8) < .0001);
    nemo_smpl_theme_shutdown ();

    /* Startup loads an already-present small valid stylesheet synchronously. */
    nemo_smpl_theme_init ();
    g_assert_nonnull (theme_provider);
    WAIT_FOR (fabs (background_alpha (window) - 1.) < .0001);
    nemo_smpl_theme_shutdown ();
    g_assert_cmpint (g_unlink (path), ==, 0);
    g_assert_cmpint (g_rmdir (directory), ==, 0);
    g_assert_cmpint (g_rmdir (g_get_user_config_dir ()), ==, 0);
    gtk_widget_destroy (window);
    gtk_style_context_remove_provider_for_screen (
        gdk_screen_get_default (), GTK_STYLE_PROVIDER (fallback));
    g_object_unref (fallback);
    g_free (moved_css);
    g_free (moved);
    g_free (replacement);
    g_free (imported);
    g_free (path);
    g_free (directory);
}

static void
test_path_precedence (void)
{
    const char *legacy_css = "#smpl-theme-probe { background-color: rgba(10,20,30,.25); }";
    const char *preferred_css = "#smpl-theme-probe { background-color: rgba(10,20,30,.55); }";
    const char *edited_css = "#smpl-theme-probe { background-color: rgba(10,20,30,.4); }";
    char *home = g_build_filename (test_root, "compat-home", NULL);
    char *config = g_build_filename (home, ".config", NULL);
    char *legacy_dir = g_build_filename (config, "smplos", NULL);
    char *legacy = g_build_filename (legacy_dir, "nemo-theme.css", NULL);
    char *xdg = g_build_filename (test_root, "compat-xdg", NULL);
    char *preferred_dir = g_build_filename (xdg, "smplos", NULL);
    char *preferred = g_build_filename (preferred_dir, "nemo-theme.css", NULL);
    GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    GtkCssProvider *previous;
    GFileMonitor *monitors[2];
    GFile *preferred_file;
    GError *error = NULL;

    gtk_widget_set_name (window, "smpl-theme-probe");
    gtk_widget_show (window);
    g_assert_cmpint (g_mkdir_with_parents (legacy_dir, 0700), ==, 0);
    write_css (legacy, legacy_css);

    /* Explicit paths exercise selection without changing GLib's cached dirs. */
    init_theme_paths (home, xdg);
    assert_watch_path (0, test_root);
    assert_watch_path (1, legacy_dir);
    WAIT_FOR (fabs (background_alpha (window) - .25) < .0001);

    g_assert_cmpint (g_mkdir_with_parents (preferred_dir, 0700), ==, 0);
    write_css (preferred, preferred_css);
    wait_for_reload ();
    assert_watch_path (0, preferred_dir);
    WAIT_FOR (fabs (background_alpha (window) - .55) < .0001);
    write_css (legacy, legacy_css);
    wait_for_reload ();
    g_assert_cmpfloat_with_epsilon (background_alpha (window), .55, .0001);
    g_assert_cmpint (g_unlink (preferred), ==, 0);
    wait_for_reload ();
    WAIT_FOR (fabs (background_alpha (window) - .25) < .0001);
    write_css (preferred, preferred_css);
    wait_for_reload ();
    WAIT_FOR (fabs (background_alpha (window) - .55) < .0001);

    /* An actual ENOTDIR during selection is not equivalent to a missing file. */
    preferred_file = theme_watches[0].file;
    theme_watches[0].file = g_file_get_child (preferred_file, "not-a-directory");
    g_assert_null (select_theme_file (&error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_DIRECTORY);
    g_clear_error (&error);
    g_object_unref (theme_watches[0].file);
    theme_watches[0].file = preferred_file;
    previous = g_object_ref (theme_provider);

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*failed to load nemo-theme.css:*");
    write_css (preferred, "#smpl-theme-probe { background-color: invalid; }");
    wait_for_reload ();
    g_test_assert_expected_messages ();
    g_assert_true (theme_provider == previous);
    g_assert_cmpfloat_with_epsilon (background_alpha (window), .55, .0001);

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*empty nemo-theme.css*retaining*");
    write_css (preferred, "");
    wait_for_reload ();
    g_test_assert_expected_messages ();
    g_assert_true (theme_provider == previous);
    g_assert_cmpfloat_with_epsilon (background_alpha (window), .55, .0001);

    /* Existing but unreadable input must not enable legacy fallback either. */
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*failed to load nemo-theme.css:*");
    g_assert_cmpint (g_unlink (preferred), ==, 0);
    g_assert_cmpint (g_mkdir (preferred, 0700), ==, 0);
    wait_for_reload ();
    g_test_assert_expected_messages ();
    g_assert_true (theme_provider == previous);
    g_assert_cmpfloat_with_epsilon (background_alpha (window), .55, .0001);
    g_object_unref (previous);

    g_assert_cmpint (g_rmdir (preferred), ==, 0);
    wait_for_reload ();
    WAIT_FOR (fabs (background_alpha (window) - .25) < .0001);
    write_css (legacy, edited_css);
    wait_for_reload ();
    WAIT_FOR (fabs (background_alpha (window) - .4) < .0001);

    g_assert_cmpint (g_unlink (legacy), ==, 0);
    g_assert_cmpint (g_rmdir (legacy_dir), ==, 0);
    g_assert_cmpint (g_rmdir (config), ==, 0);
    wait_for_reload ();
    g_assert_null (theme_provider);
    assert_watch_path (1, home);
    g_assert_cmpint (g_mkdir_with_parents (legacy_dir, 0700), ==, 0);
    write_css (legacy, legacy_css);
    wait_for_reload ();
    assert_watch_path (1, legacy_dir);
    WAIT_FOR (fabs (background_alpha (window) - .25) < .0001);

    for (guint i = 0; i < G_N_ELEMENTS (monitors); i++)
        monitors[i] = g_object_ref (theme_watches[i].monitor);
    nemo_smpl_theme_shutdown ();
    for (guint i = 0; i < G_N_ELEMENTS (monitors); i++) {
        g_assert_true (g_file_monitor_is_cancelled (monitors[i]));
        g_object_unref (monitors[i]);
    }
    gtk_widget_destroy (window);
    g_assert_cmpint (g_unlink (legacy), ==, 0);
    g_assert_cmpint (g_rmdir (legacy_dir), ==, 0);
    g_assert_cmpint (g_rmdir (config), ==, 0);
    g_assert_cmpint (g_rmdir (home), ==, 0);
    g_assert_cmpint (g_rmdir (preferred_dir), ==, 0);
    g_assert_cmpint (g_rmdir (xdg), ==, 0);
    g_free (preferred);
    g_free (preferred_dir);
    g_free (xdg);
    g_free (legacy);
    g_free (legacy_dir);
    g_free (config);
    g_free (home);
}

static void
test_relative_xdg (void)
{
    char *home = g_build_filename (test_root, "relative-home", NULL);
    char *config = g_build_filename (home, ".config", NULL);
    char *directory = g_build_filename (config, "smplos", NULL);
    char *path = g_build_filename (directory, "nemo-theme.css", NULL);
    GFile *expected = g_file_new_for_path (path);
    GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    const char *xdg_values[] = { "relative-config", "", NULL, config };

    gtk_widget_set_name (window, "smpl-theme-probe");
    gtk_widget_show (window);
    g_assert_cmpint (g_mkdir (home, 0700), ==, 0);
    init_theme_paths (home, "relative-config");
    g_assert_true (g_file_equal (theme_watches[0].file, expected));
    g_assert_null (theme_watches[1].file);
    assert_watching (home);
    g_assert_null (theme_provider);
    g_assert_cmpint (g_mkdir_with_parents (directory, 0700), ==, 0);
    write_css (path, "#smpl-theme-probe { background-color: rgba(10,20,30,.65); }");
    wait_for_reload ();
    WAIT_FOR (fabs (background_alpha (window) - .65) < .0001);
    nemo_smpl_theme_shutdown ();

    for (guint i = 0; i < G_N_ELEMENTS (xdg_values); i++) {
        init_theme_paths (home, xdg_values[i]);
        g_assert_true (g_file_equal (theme_watches[0].file, expected));
        g_assert_null (theme_watches[1].file);
        WAIT_FOR (fabs (background_alpha (window) - .65) < .0001);
        nemo_smpl_theme_shutdown ();
    }
    gtk_widget_destroy (window);
    g_object_unref (expected);
    g_assert_cmpint (g_unlink (path), ==, 0);
    g_assert_cmpint (g_rmdir (directory), ==, 0);
    g_assert_cmpint (g_rmdir (config), ==, 0);
    g_assert_cmpint (g_rmdir (home), ==, 0);
    g_free (path);
    g_free (directory);
    g_free (config);
    g_free (home);
}

int
main (int argc, char **argv)
{
    const char *profile = g_getenv ("NEMO_TEST_PROFILE");
    const char *config = g_getenv ("XDG_CONFIG_HOME");
    const char *home = g_getenv ("HOME");
    char *config_path;
    GFile *profile_file;
    GFile *config_file;
    GFile *home_file;
    int result;

    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0 ||
        profile == NULL || config == NULL || home == NULL) {
        g_printerr ("Run this test through test/run-isolated-regression.py\n");
        return 77;
    }
    profile_file = g_file_new_for_path (profile);
    config_file = g_file_new_for_path (config);
    home_file = g_file_new_for_path (home);
    g_assert_true (g_file_has_prefix (config_file, profile_file));
    g_assert_true (g_file_has_prefix (home_file, profile_file));
    g_object_unref (home_file);
    g_object_unref (config_file);
    g_object_unref (profile_file);
    test_root = g_build_filename (config, "smpl-theme-regression", NULL);
    g_assert_cmpint (g_mkdir (test_root, 0700), ==, 0);
    config_path = g_build_filename (test_root, "config", NULL);
    g_setenv ("XDG_CONFIG_HOME", config_path, TRUE);
    g_free (config_path);
    g_test_init (&argc, &argv, NULL);
    if (!gtk_init_check (&argc, &argv)) {
        g_assert_cmpint (g_rmdir (test_root), ==, 0);
        g_free (test_root);
        return 77;
    }
    g_test_add_func ("/smpl-theme/lifecycle", test_theme_lifecycle);
    g_test_add_func ("/smpl-theme/path-precedence", test_path_precedence);
    g_test_add_func ("/smpl-theme/relative-xdg", test_relative_xdg);
    result = g_test_run ();
    g_assert_cmpint (g_rmdir (test_root), ==, 0);
    g_free (test_root);
    return result;
}
