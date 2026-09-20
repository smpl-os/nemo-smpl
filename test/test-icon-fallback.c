#include <config.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <libnemo-private/nemo-file-utilities.h>
#include <libnemo-private/nemo-icon-fallback.h>
#include <libnemo-private/nemo-icon-info.h>

typedef struct {
    GObject parent;
    GIcon *icon;
} IconVolume;
typedef GObjectClass IconVolumeClass;

static void icon_volume_iface_init (GVolumeIface *iface);
G_DEFINE_TYPE_WITH_CODE (IconVolume, icon_volume, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME, icon_volume_iface_init))

static void
icon_volume_finalize (GObject *object)
{
    g_clear_object (&((IconVolume *) object)->icon);
    G_OBJECT_CLASS (icon_volume_parent_class)->finalize (object);
}

static void
icon_volume_class_init (IconVolumeClass *klass)
{
    klass->finalize = icon_volume_finalize;
}

static void icon_volume_init (IconVolume *volume) {}
static gchar *icon_volume_name (GVolume *volume) { return g_strdup ("Device icon fixture"); }
static GIcon *icon_volume_icon (GVolume *volume) { return g_object_ref (((IconVolume *) volume)->icon); }

static void
icon_volume_iface_init (GVolumeIface *iface)
{
    iface->get_name = icon_volume_name;
    iface->get_icon = icon_volume_icon;
    iface->get_symbolic_icon = icon_volume_icon;
}

static GtkIconTheme *theme;
static gchar *fixture;

gboolean __real_gtk_icon_theme_has_icon (GtkIconTheme *, const gchar *);
gboolean
__wrap_gtk_icon_theme_has_icon (GtkIconTheme *icon_theme, const gchar *name)
{
    if (g_str_has_prefix (name, "xsi-unusable-cache-entry")) {
        return TRUE;
    }
    return __real_gtk_icon_theme_has_icon (icon_theme, name);
}

static void
use_theme (const gchar *name)
{
    const gchar *paths[] = { fixture };
    gtk_icon_theme_set_search_path (theme, paths, G_N_ELEMENTS (paths));
    g_object_set (gtk_settings_get_default (), "gtk-icon-theme-name", name, NULL);
    while (g_main_context_pending (NULL)) {
        g_main_context_iteration (NULL, FALSE);
    }
}

static GtkIconInfo *
assert_loadable (const gchar *name, gint size)
{
    GError *error = NULL;
    GtkIconInfo *info = gtk_icon_theme_lookup_icon (theme, name, size, GTK_ICON_LOOKUP_FORCE_SIZE);
    g_assert_nonnull (info);
    GdkPixbuf *pixbuf = gtk_icon_info_load_icon (info, &error);
    g_assert_no_error (error);
    g_assert_nonnull (pixbuf);
    g_assert_cmpint (gdk_pixbuf_get_width (pixbuf), >, 0);
    g_assert_cmpint (gdk_pixbuf_get_height (pixbuf), >, 0);
    g_object_unref (pixbuf);
    return info;
}

static void
test_no_installed_theme (void)
{
    GError *error = NULL;
    use_theme ("NemoMissingTheme");
    gchar **icons = g_resources_enumerate_children (NEMO_ICON_FALLBACK_RESOURCE_PATH, 0, &error);
    g_assert_no_error (error);
    g_assert_nonnull (icons);
    g_assert_cmpuint (g_strv_length (icons), >, 1000);
    for (guint i = 0; icons[i] != NULL; i++) {
        gchar *resource = g_strconcat (NEMO_ICON_FALLBACK_RESOURCE_PATH "/", icons[i], NULL);
        GdkPixbuf *embedded = gdk_pixbuf_new_from_resource (resource, &error);
        g_assert_no_error (error);
        g_assert_nonnull (embedded);
        g_object_unref (embedded);
        g_free (resource);
        gchar *name = g_strdup (icons[i]);
        gchar *extension = strrchr (name, '.');
        g_assert_nonnull (extension);
        *extension = '\0';
        g_test_message ("Loading %s", name);
        GtkIconInfo *info = assert_loadable (name, 16);
        g_assert_true (g_resources_get_info (gtk_icon_info_get_filename (info),
                                            0, NULL, NULL, &error));
        g_assert_no_error (error);
        g_object_unref (info);
        g_free (name);
    }
    g_strfreev (icons);
}

static gchar *
device_name (const gchar **names)
{
    IconVolume *volume = g_object_new (icon_volume_get_type (), NULL);
    volume->icon = g_themed_icon_new_from_names ((gchar **) names, -1);
    gchar *name = nemo_get_volume_icon_name (G_VOLUME (volume));
    g_object_unref (volume);
    return name;
}

static void
test_dynamic_devices (void)
{
    use_theme ("NemoMissingTheme");
    const gchar *names[] = {
        "drive-harddisk-usb-symbolic", "drive-removable-media-usb-symbolic",
        "drive-harddisk-solidstate-symbolic", "multimedia-player-symbolic",
        "phone-symbolic", "camera-photo-symbolic", NULL
    };
    for (guint i = 0; names[i] != NULL; i++) {
        const gchar *candidates[] = { names[i], NULL };
        gchar *name = device_name (candidates);
        gchar *expected = g_strconcat ("xsi-", names[i], NULL);
        g_assert_cmpstr (name, ==, expected);
        GtkIconInfo *info = assert_loadable (name, 16);
        g_assert_true (gtk_icon_info_is_symbolic (info));
        g_object_unref (info);
        g_free (name);
        g_free (expected);
    }
    /* Reproduce a cached name whose file cannot actually be resolved. */
    gchar *directory = g_build_filename (fixture, "hicolor", "16x16", "actions", NULL);
    g_assert_cmpint (g_mkdir_with_parents (directory, 0700), ==, 0);
    gchar *index = g_build_filename (fixture, "hicolor", "index.theme", NULL);
    g_assert_true (g_file_set_contents (index,
        "[Icon Theme]\nName=Hicolor\nDirectories=16x16/actions\n"
        "[16x16/actions]\nSize=16\nType=Fixed\nContext=Actions\n", -1, NULL));
    use_theme ("NemoMissingThemeBadCache");
    const gchar *bad_cache[] = { "unusable-cache-entry-symbolic", "phone-symbolic", NULL };
    gchar *name = device_name (bad_cache);
    g_assert_cmpstr (name, ==, "xsi-phone-symbolic");
    g_free (name);
    g_assert_cmpint (g_unlink (index), ==, 0);
    g_assert_cmpint (g_rmdir (directory), ==, 0);
    gchar *parent = g_path_get_dirname (directory);
    g_assert_cmpint (g_rmdir (parent), ==, 0);
    g_free (parent);
    parent = g_path_get_dirname (index);
    g_assert_cmpint (g_rmdir (parent), ==, 0);
    g_free (parent);
    g_free (directory);
    g_free (index);
}

static void
test_theme_precedence (void)
{
    GError *error = NULL;
    gchar *directory = g_build_filename (fixture, "FixtureTheme", "16x16", "actions", NULL);
    g_assert_cmpint (g_mkdir_with_parents (directory, 0700), ==, 0);
    gchar *index = g_build_filename (fixture, "FixtureTheme", "index.theme", NULL);
    g_assert_true (g_file_set_contents (index,
        "[Icon Theme]\nName=Fixture\nDirectories=16x16/actions\n"
        "[16x16/actions]\nSize=16\nType=Fixed\nContext=Actions\n", -1, &error));
    g_assert_no_error (error);
    gchar *image = g_build_filename (directory, "phone-symbolic.svg", NULL);
    g_assert_true (g_file_set_contents (image,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\">"
        "<path fill=\"#2e3436\" d=\"M2 2h12v12H2z\"/></svg>", -1, &error));
    g_assert_no_error (error);
    use_theme ("FixtureTheme");

    const gchar *candidates[] = { "phone-symbolic", NULL };
    gchar *name = device_name (candidates);
    g_assert_cmpstr (name, ==, "phone-symbolic");
    GtkIconInfo *info = assert_loadable (name, 16);
    g_assert_cmpstr (gtk_icon_info_get_filename (info), ==, image);
    g_object_unref (info);
    g_free (name);
    info = assert_loadable ("xsi-drive-harddisk-usb-symbolic", 16);
    g_assert_true (g_str_has_prefix (gtk_icon_info_get_filename (info),
                                   NEMO_ICON_FALLBACK_RESOURCE_PATH "/"));
    g_object_unref (info);

    nemo_icon_fallback_init ();
    use_theme ("NemoMissingThemeAgain");
    info = assert_loadable ("phone-symbolic", 32);
    g_assert_true (g_str_has_prefix (gtk_icon_info_get_filename (info),
                                   NEMO_ICON_FALLBACK_RESOURCE_PATH "/"));
    g_object_unref (info);
    g_assert_cmpint (g_unlink (image), ==, 0);
    g_assert_cmpint (g_unlink (index), ==, 0);
    g_assert_cmpint (g_rmdir (directory), ==, 0);
    gchar *parent = g_path_get_dirname (directory);
    g_assert_cmpint (g_rmdir (parent), ==, 0);
    g_free (parent);
    parent = g_path_get_dirname (index);
    g_assert_cmpint (g_rmdir (parent), ==, 0);
    g_free (parent);
    g_free (image);
    g_free (index);
    g_free (directory);
}

static void
test_file_icons (void)
{
    use_theme ("NemoMissingTheme");
    const gchar *names[] = { "folder", "text-x-generic", "image-x-generic",
                            "audio-x-generic", "video-x-generic", "user-home", NULL };
    for (guint i = 0; names[i] != NULL; i++) {
        GIcon *gicon = g_themed_icon_new (names[i]);
        NemoIconInfo *info = nemo_icon_info_lookup (gicon, 48, 1);
        g_assert_nonnull (info);
        g_assert_false (nemo_icon_info_is_fallback (info));
        GdkPixbuf *pixbuf = nemo_icon_info_get_pixbuf (info);
        g_assert_nonnull (pixbuf);
        g_object_unref (pixbuf);
        nemo_icon_info_unref (info);
        g_object_unref (gicon);
    }
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
        g_printerr ("Run through test/run-isolated-regression.py\n");
        return 77;
    }
    g_test_init (&argc, &argv, NULL);
    gtk_init (&argc, &argv);
    fixture = g_dir_make_tmp ("nemo-icon-theme-XXXXXX", NULL);
    g_assert_nonnull (fixture);
    theme = gtk_icon_theme_get_default ();
    nemo_icon_fallback_init ();
    g_test_add_func ("/icons/no-installed-theme", test_no_installed_theme);
    g_test_add_func ("/icons/dynamic-devices", test_dynamic_devices);
    g_test_add_func ("/icons/theme-precedence", test_theme_precedence);
    g_test_add_func ("/icons/file-icons", test_file_icons);
    gint result = g_test_run ();
    g_assert_cmpint (g_rmdir (fixture), ==, 0);
    g_free (fixture);
    return result;
}
