#include "nemo-smpl-theme.h"

#include <gtk/gtk.h>

#define SMPLOS_CSS_DEBOUNCE_MS 200

static GtkCssProvider *theme_provider;
static GdkScreen *theme_screen;
typedef struct {
    GFile *file;
    GFile *directory;
    GFileMonitor *monitor;
    gboolean stale;
} ThemeWatch;

/* Preferred XDG path first, then the legacy smplOS writer's HOME path. */
static ThemeWatch theme_watches[2];
static guint reload_timer;

static void
remove_theme_provider (void)
{
    if (theme_provider == NULL)
        return;

    gtk_style_context_remove_provider_for_screen (
        theme_screen, GTK_STYLE_PROVIDER (theme_provider));
    g_clear_object (&theme_provider);
    gtk_style_context_reset_widgets (theme_screen);
}

static void
clear_theme_monitor (ThemeWatch *watch)
{
    if (watch->monitor != NULL) {
        g_signal_handlers_disconnect_by_data (watch->monitor, watch);
        g_file_monitor_cancel (watch->monitor);
        g_clear_object (&watch->monitor);
    }
    g_clear_object (&watch->directory);
}

static gboolean reload_theme (gpointer user_data);

static gboolean
affects_theme (ThemeWatch *watch, GFile *file)
{
    return file != NULL &&
        (g_file_equal (watch->file, file) || g_file_has_prefix (watch->file, file));
}

static void
theme_changed (GFileMonitor *monitor,
               GFile *file,
               GFile *other_file,
               GFileMonitorEvent event,
               gpointer user_data)
{
    ThemeWatch *watch = user_data;

    (void) monitor;

    switch (event) {
    case G_FILE_MONITOR_EVENT_CHANGED:
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
    case G_FILE_MONITOR_EVENT_MOVED:
    case G_FILE_MONITOR_EVENT_RENAMED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
        if (!affects_theme (watch, file) && !affects_theme (watch, other_file))
            return;
        if (file != NULL && g_file_equal (file, watch->directory) &&
            (event == G_FILE_MONITOR_EVENT_DELETED ||
             event == G_FILE_MONITOR_EVENT_MOVED ||
             event == G_FILE_MONITOR_EVENT_RENAMED ||
             event == G_FILE_MONITOR_EVENT_MOVED_OUT))
            watch->stale = TRUE;
        if (reload_timer != 0)
            g_source_remove (reload_timer);
        reload_timer = g_timeout_add (SMPLOS_CSS_DEBOUNCE_MS, reload_theme, NULL);
        break;
    default:
        break;
    }
}

static void
refresh_theme_monitor (ThemeWatch *watch)
{
    GFile *directory = g_file_get_parent (watch->file);
    GError *error = NULL;

    /* Watch the closest existing ancestor; never create configuration folders.
     * Each hierarchy change moves this watch closer to (or away from) the CSS. */
    while (directory != NULL) {
        GFileInfo *info = g_file_query_info (directory, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                           G_FILE_QUERY_INFO_NONE, NULL, &error);
        if (info != NULL) {
            gboolean is_directory = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
            g_object_unref (info);
            if (!is_directory) {
                char *path = g_file_get_path (directory);
                g_warning ("smplOS: could not watch theme directory %s: not a directory", path);
                g_free (path);
                g_object_unref (directory);
                return;
            }
            break;
        }
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
            g_warning ("smplOS: could not inspect theme directory: %s", error->message);
            g_clear_error (&error);
            g_object_unref (directory);
            return;
        }
        g_clear_error (&error);
        GFile *parent = g_file_get_parent (directory);
        g_object_unref (directory);
        directory = parent;
    }

    if (directory == NULL) {
        g_warning ("smplOS: no existing ancestor for theme directory");
        return;
    }
    if (!watch->stale && watch->monitor != NULL &&
        g_file_equal (directory, watch->directory)) {
        g_object_unref (directory);
        return;
    }

    GFileMonitor *monitor = g_file_monitor_directory (
        directory, G_FILE_MONITOR_WATCH_MOVES, NULL, &error);
    if (monitor == NULL) {
        g_warning ("smplOS: could not watch theme directory: %s", error->message);
        g_clear_error (&error);
        g_object_unref (directory);
        return;
    }
    clear_theme_monitor (watch);
    watch->monitor = monitor;
    watch->directory = directory;
    watch->stale = FALSE;
    g_signal_connect (watch->monitor, "changed", G_CALLBACK (theme_changed), watch);
}

static GFile *
select_theme_file (GError **error)
{
    for (guint i = 0; i < G_N_ELEMENTS (theme_watches); i++) {
        GFile *file = theme_watches[i].file;
        GFileInfo *info;

        if (file == NULL)
            continue;
        /* GTK wraps IO errors in CSS import errors. Only NOT_FOUND allows
         * fallback; an inaccessible preferred file must retain the last theme. */
        info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                  G_FILE_QUERY_INFO_NONE, NULL, error);
        if (info != NULL) {
            g_object_unref (info);
            return file;
        }
        if (!g_error_matches (*error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
            return NULL;
        g_clear_error (error);
    }
    return NULL;
}

static gboolean
reload_theme (gpointer user_data)
{
    char *stylesheet;
    GError *error = NULL;
    GtkCssProvider *candidate;
    GFile *file;

    (void) user_data;
    reload_timer = 0;
    for (guint i = 0; i < G_N_ELEMENTS (theme_watches); i++) {
        if (theme_watches[i].file != NULL)
            refresh_theme_monitor (&theme_watches[i]);
    }

    file = select_theme_file (&error);
    if (error != NULL) {
        g_warning ("smplOS: failed to inspect nemo-theme.css: %s", error->message);
        g_clear_error (&error);
        return G_SOURCE_REMOVE;
    }
    if (file == NULL) {
        remove_theme_provider ();
        return G_SOURCE_REMOVE;
    }

    /* Keep the file's base URI so relative @imports and image URLs still work. */
    candidate = gtk_css_provider_new ();
    if (!gtk_css_provider_load_from_file (candidate, file, &error)) {
        g_warning ("smplOS: failed to load nemo-theme.css: %s", error->message);
        g_clear_error (&error);
        g_object_unref (candidate);
        return G_SOURCE_REMOVE;
    }

    /* An empty file can be a writer's transient O_TRUNC state. Deletion, not
     * truncation, is the explicit request to return to the GTK fallback.
     * Check parsed contents so whitespace and comment-only files behave alike. */
    stylesheet = gtk_css_provider_to_string (candidate);
    if (*stylesheet == '\0') {
        g_warning ("smplOS: empty nemo-theme.css; retaining the previous theme");
        g_free (stylesheet);
        g_object_unref (candidate);
        return G_SOURCE_REMOVE;
    }
    g_free (stylesheet);

    /* Parse on a fresh provider before touching the installed stylesheet. */
    remove_theme_provider ();
    theme_provider = candidate;
    gtk_style_context_add_provider_for_screen (
        theme_screen, GTK_STYLE_PROVIDER (theme_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER);
    gtk_style_context_reset_widgets (theme_screen);
    return G_SOURCE_REMOVE;
}

static void
init_theme_paths (const char *home, const char *xdg_config_home)
{
    char *legacy_path;
    char *preferred_path;
    GFile *legacy_file;

    if (theme_watches[0].file != NULL)
        return;

    g_return_if_fail (gdk_screen_get_default () != NULL);
    theme_screen = g_object_ref (gdk_screen_get_default ());
    legacy_path = g_build_filename (home, ".config", "smplos", "nemo-theme.css", NULL);
    legacy_file = g_file_new_for_path (legacy_path);
    if (xdg_config_home != NULL && g_path_is_absolute (xdg_config_home)) {
        preferred_path = g_build_filename (xdg_config_home, "smplos", "nemo-theme.css", NULL);
        theme_watches[0].file = g_file_new_for_path (preferred_path);
        g_free (preferred_path);
    } else {
        /* GLib accepts relative XDG_CONFIG_HOME; the XDG specification does not. */
        theme_watches[0].file = g_object_ref (legacy_file);
    }
    if (!g_file_equal (theme_watches[0].file, legacy_file))
        theme_watches[1].file = g_object_ref (legacy_file);
    g_object_unref (legacy_file);
    g_free (legacy_path);
    reload_theme (NULL);
}

void
nemo_smpl_theme_init (void)
{
    init_theme_paths (g_get_home_dir (), g_getenv ("XDG_CONFIG_HOME"));
}

void
nemo_smpl_theme_shutdown (void)
{
    if (reload_timer != 0) {
        g_source_remove (reload_timer);
        reload_timer = 0;
    }
    for (guint i = 0; i < G_N_ELEMENTS (theme_watches); i++) {
        clear_theme_monitor (&theme_watches[i]);
        g_clear_object (&theme_watches[i].file);
        theme_watches[i].stale = FALSE;
    }
    remove_theme_provider ();
    g_clear_object (&theme_screen);
}
