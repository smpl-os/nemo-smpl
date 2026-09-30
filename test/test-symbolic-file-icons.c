/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <libnemo-private/nemo-file-private.h>
#include <libnemo-private/nemo-global-preferences.h>
#include <libnemo-private/nemo-icon-fallback.h>
#include <libnemo-private/nemo-icon-names.h>

static char *fixture;
static GtkIconTheme *theme;
static GtkStyleContext *light;
static GtkStyleContext *dark;

static void
drain_events (void)
{
    while (g_main_context_pending (NULL))
        g_main_context_iteration (NULL, FALSE);
}

static GtkStyleContext *
new_context (const char *css)
{
    GtkStyleContext *context = gtk_style_context_new ();
    GtkWidgetPath *path = gtk_widget_path_new ();
    GtkCssProvider *provider = gtk_css_provider_new ();
    GError *error = NULL;

    gtk_widget_path_append_type (path, GTK_TYPE_ICON_VIEW);
    gtk_style_context_set_path (context, path);
    gtk_widget_path_unref (path);
    gtk_css_provider_load_from_data (provider, css, -1, &error);
    g_assert_no_error (error);
    gtk_style_context_add_provider (context, GTK_STYLE_PROVIDER (provider),
                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (provider);
    return context;
}

static void
use_theme (const char *name)
{
    const char *paths[] = { fixture };
    gtk_icon_theme_set_search_path (theme, paths, G_N_ELEMENTS (paths));
    g_object_set (gtk_settings_get_default (), "gtk-icon-theme-name", name, NULL);
    drain_events ();
}

static guint32
opaque_pixel (GdkPixbuf *pixbuf)
{
    int stride = gdk_pixbuf_get_rowstride (pixbuf);
    int channels = gdk_pixbuf_get_n_channels (pixbuf);
    guchar *pixels = gdk_pixbuf_get_pixels (pixbuf);

    for (int y = 0; y < gdk_pixbuf_get_height (pixbuf); y++) {
        for (int x = 0; x < gdk_pixbuf_get_width (pixbuf); x++) {
            guchar *p = pixels + y * stride + x * channels;
            if (channels == 3 || p[3] > 250)
                return p[0] << 16 | p[1] << 8 | p[2];
        }
    }
    g_assert_not_reached ();
    return 0;
}

static GdkPixbuf *
lookup (const char *name, GtkStyleContext *context, int scale)
{
    GIcon *icon = g_themed_icon_new (name);
    NemoIconInfo *info = nemo_icon_info_lookup_for_context (icon, 32, scale, context);
    g_assert_nonnull (info);
    g_assert_false (nemo_icon_info_is_fallback (info));
    GdkPixbuf *pixbuf = nemo_icon_info_get_pixbuf (info);
    g_object_unref (icon);
    nemo_icon_info_unref (info);
    return pixbuf;
}

static void
test_context_colors (void)
{
    use_theme ("NemoMissingSymbolicTheme");
    GdkPixbuf *a = lookup ("text-x-generic-symbolic", light, 1);
    GdkPixbuf *b = lookup ("text-x-generic-symbolic", dark, 1);
    GdkPixbuf *a_again = lookup ("text-x-generic-symbolic", light, 1);
    g_assert_cmphex (opaque_pixel (a), ==, 0x203040);
    g_assert_cmphex (opaque_pixel (b), ==, 0xe0d0c0);
    g_assert_cmphex (opaque_pixel (a_again), ==, opaque_pixel (a));

    gtk_style_context_save (light);
    gtk_style_context_set_state (light, GTK_STATE_FLAG_SELECTED);
    GdkPixbuf *selected = lookup ("text-x-generic-symbolic", light, 1);
    g_assert_cmphex (opaque_pixel (selected), ==, 0xffffff);
    gtk_style_context_restore (light);

    GtkCssProvider *provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (provider, "* { color: #80a020; }", -1, NULL);
    gtk_style_context_add_provider (light, GTK_STYLE_PROVIDER (provider),
                                    GTK_STYLE_PROVIDER_PRIORITY_USER);
    GdkPixbuf *changed = lookup ("text-x-generic-symbolic", light, 2);
    g_assert_cmphex (opaque_pixel (changed), ==, 0x80a020);
    g_assert_cmpint (gdk_pixbuf_get_width (changed), ==, 64);
    g_assert_cmphex (opaque_pixel (a), ==, 0x203040);
    gtk_style_context_remove_provider (light, GTK_STYLE_PROVIDER (provider));
    g_object_unref (provider);
    g_object_unref (changed);
    g_object_unref (selected);
    g_object_unref (a_again);
    g_object_unref (a);
    g_object_unref (b);
}

static guint64
alpha_sum (GdkPixbuf *pixbuf)
{
    guint64 sum = 0;
    g_assert_true (gdk_pixbuf_get_has_alpha (pixbuf));
    for (int y = 0; y < gdk_pixbuf_get_height (pixbuf); y++) {
        guchar *p = gdk_pixbuf_get_pixels (pixbuf) + y * gdk_pixbuf_get_rowstride (pixbuf);
        for (int x = 0; x < gdk_pixbuf_get_width (pixbuf); x++)
            sum += p[4 * x + 3];
    }
    return sum;
}

static void
test_theme_change_and_missing (void)
{
    use_theme ("SymbolicFixtureA");
    GdkPixbuf *a = lookup ("nemo-fixture-symbolic", dark, 1);
    use_theme ("SymbolicFixtureB");
    GdkPixbuf *b = lookup ("nemo-fixture-symbolic", dark, 1);
    g_assert_cmpuint (alpha_sum (a), >, alpha_sum (b));
    g_assert_cmphex (opaque_pixel (a), ==, opaque_pixel (b));
    g_object_unref (a);
    g_object_unref (b);

    use_theme ("NemoMissingSymbolicTheme");
    a = lookup ("nemo-nonexistent-type-symbolic", dark, 1);
    b = lookup ("text-x-generic-symbolic", dark, 1);
    g_assert_cmpuint (alpha_sum (a), ==, alpha_sum (b));
    g_assert_cmphex (opaque_pixel (a), ==, 0xe0d0c0);
    g_object_unref (a);
    g_object_unref (b);
}

static NemoFile *
file_with_info (const char *path, GFileType type, const char *mime)
{
    GFile *location = g_file_new_for_path (path);
    NemoFile *file = nemo_file_get (location);
    GFileInfo *info = g_file_info_new ();
    char *name = g_file_get_basename (location);
    GIcon *icon = g_themed_icon_new (type == G_FILE_TYPE_DIRECTORY ? "folder" : "text-x-generic");

    g_file_info_set_name (info, name);
    g_file_info_set_display_name (info, name);
    g_file_info_set_file_type (info, type);
    g_file_info_set_size (info, 32);
    g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ, TRUE);
    if (mime != NULL)
        g_file_info_set_content_type (info, mime);
    g_file_info_set_icon (info, icon);
    nemo_file_update_info (file, info);
    g_object_unref (icon);
    g_object_unref (info);
    g_object_unref (location);
    g_free (name);
    return file;
}

static void
test_special_folders (void)
{
    static const char *names[] = {
        NEMO_ICON_SYMBOLIC_DESKTOP, "folder-documents-symbolic", "folder-download-symbolic",
        "folder-music-symbolic", "folder-pictures-symbolic", "folder-publicshare-symbolic",
        "folder-templates-symbolic", "folder-videos-symbolic"
    };
    static const GUserDirectory kinds[] = {
        G_USER_DIRECTORY_DESKTOP, G_USER_DIRECTORY_DOCUMENTS, G_USER_DIRECTORY_DOWNLOAD,
        G_USER_DIRECTORY_MUSIC, G_USER_DIRECTORY_PICTURES, G_USER_DIRECTORY_PUBLIC_SHARE,
        G_USER_DIRECTORY_TEMPLATES, G_USER_DIRECTORY_VIDEOS
    };
    use_theme ("NemoMissingSymbolicTheme");
    for (guint i = 0; i <= G_N_ELEMENTS (names); i++) {
        const char *path = i == G_N_ELEMENTS (names) ? g_get_home_dir () :
                           g_get_user_special_dir (kinds[i]);
        const char *expected = i == G_N_ELEMENTS (names) ? "user-home-symbolic" : names[i];
        g_assert_nonnull (path);
        NemoFile *file = file_with_info (path, G_FILE_TYPE_DIRECTORY, "inode/directory");
        NemoIconInfo *info = nemo_file_get_icon_for_context (file, 32, 0, 1, 0, light);
        g_assert_cmpstr (nemo_icon_info_get_used_name (info), ==, expected);
        nemo_icon_info_unref (info);
        nemo_file_unref (file);
    }
}

static void
test_preference_and_legacy (void)
{
    char *path = g_build_filename (fixture, "unknown.fixture", NULL);
    NemoFile *file = file_with_info (path, G_FILE_TYPE_REGULAR, "application/x-nemo-unknown");
    NemoIconInfo *symbolic = nemo_file_get_icon_for_context (file, 32, 0, 1, 0, dark);
    GdkPixbuf *pixbuf = nemo_icon_info_get_pixbuf (symbolic);
    g_assert_true (g_str_has_suffix (nemo_icon_info_get_used_name (symbolic), "-symbolic"));
    g_assert_cmphex (opaque_pixel (pixbuf), ==, 0xe0d0c0);
    g_object_unref (pixbuf);
    nemo_icon_info_unref (symbolic);

    NemoIconInfo *legacy = nemo_file_get_icon (file, 32, 0, 1, 0);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_SYMBOLIC_FILE_ICONS, FALSE);
    NemoIconInfo *off = nemo_file_get_icon_for_context (file, 32, 0, 1, 0, dark);
    g_assert_true (legacy == off);
    nemo_icon_info_unref (off);
    nemo_icon_info_unref (legacy);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_SYMBOLIC_FILE_ICONS, TRUE);
    symbolic = nemo_file_get_icon_for_context (file, 32, 0, 1, 0, dark);
    g_assert_true (g_str_has_suffix (nemo_icon_info_get_used_name (symbolic), "-symbolic"));
    nemo_icon_info_unref (symbolic);
    nemo_file_unref (file);
    g_free (path);
}

static GdkPixbuf *
solid_pixbuf (guint32 rgba)
{
    GdkPixbuf *pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 32, 32);
    gdk_pixbuf_fill (pixbuf, rgba);
    return pixbuf;
}

static void
save_cover_color (const char *path, guint32 rgba)
{
    GdkPixbuf *pixbuf = solid_pixbuf (rgba);
    g_assert_true (gdk_pixbuf_save (pixbuf, path, "png", NULL, NULL));
    g_object_unref (pixbuf);
}

static void
assert_file_color_eventually (NemoFile *file, guint32 expected)
{
    gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
    guint32 actual;

    do {
        NemoIconInfo *info = nemo_file_get_icon_for_context (file, 32, 0, 1, 0, dark);
        GdkPixbuf *pixbuf = nemo_icon_info_get_pixbuf (info);
        actual = opaque_pixel (pixbuf);
        g_object_unref (pixbuf);
        nemo_icon_info_unref (info);
        if (actual == expected)
            return;
        drain_events ();
        g_usleep (1000);
    } while (g_get_monotonic_time () < deadline);
    g_assert_cmphex (actual, ==, expected);
}

static void
test_thumbnail_custom_priority (void)
{
    const char *mime_types[] = { "image/png", "video/mp4" };
    g_settings_set_enum (nemo_preferences, NEMO_PREFERENCES_SHOW_IMAGE_FILE_THUMBNAILS,
                         NEMO_SPEED_TRADEOFF_ALWAYS);
    for (guint i = 0; i < G_N_ELEMENTS (mime_types); i++) {
        char *path = g_build_filename (fixture, i == 0 ? "photo.png" : "movie.mp4", NULL);
        NemoFile *file = file_with_info (path, G_FILE_TYPE_REGULAR, mime_types[i]);
        file->details->thumbnail = solid_pixbuf (0xb06020ff);
        file->details->thumbnail_path = g_strdup ("cached-thumbnail");
        file->details->thumbnail_is_up_to_date = TRUE;
        NemoFileIconFlags flags = NEMO_FILE_ICON_FLAGS_USE_THUMBNAILS |
                                  NEMO_FILE_ICON_FLAGS_FORCE_THUMBNAIL_SIZE;
        NemoIconInfo *info = nemo_file_get_icon_for_context (file, 32, 0, 1, flags, dark);
        GdkPixbuf *pixbuf = nemo_icon_info_get_pixbuf (info);
        g_assert_cmphex (opaque_pixel (pixbuf), ==, 0xb06020);
        g_object_unref (pixbuf);
        nemo_icon_info_unref (info);

        file->details->got_link_info = TRUE;
        file->details->custom_icon = G_ICON (solid_pixbuf (0x3080b0ff));
        info = nemo_file_get_icon_for_context (file, 32, 0, 1, flags, dark);
        pixbuf = nemo_icon_info_get_pixbuf (info);
        g_assert_cmphex (opaque_pixel (pixbuf), ==, 0x3080b0);
        g_object_unref (pixbuf);
        nemo_icon_info_unref (info);
        nemo_file_unref (file);
        g_free (path);
    }
}

static void
test_cover_and_emblems (void)
{
    char *directory = g_build_filename (fixture, "album", NULL);
    char *cover = g_build_filename (directory, "cover.png", NULL);
    GdkPixbuf *art = solid_pixbuf (0xc07020ff);
    g_assert_cmpint (g_mkdir (directory, 0700), ==, 0);
    NemoFile *file = file_with_info (directory, G_FILE_TYPE_DIRECTORY, "inode/directory");
    NemoIconInfo *initial = nemo_file_get_icon_for_context (file, 32, 0, 1, 0, dark);
    g_assert_cmpstr (nemo_icon_info_get_used_name (initial), ==, "folder-symbolic");
    nemo_icon_info_unref (initial);
    for (guint i = 0; i < 20; i++) {
        drain_events ();
        g_usleep (5000);
    }
    g_assert_true (gdk_pixbuf_save (art, cover, "png", NULL, NULL));
    gint64 deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
    GdkPixbuf *pixbuf = NULL;
    gboolean found = FALSE;
    do {
        NemoIconInfo *info = nemo_file_get_icon_for_context (file, 32, 0, 1, 0, dark);
        g_clear_object (&pixbuf);
        pixbuf = nemo_icon_info_get_pixbuf (info);
        nemo_icon_info_unref (info);
        found = opaque_pixel (pixbuf) == 0xc07020;
        if (!found) {
            drain_events ();
            g_usleep (1000);
        }
    } while (!found && g_get_monotonic_time () < deadline);
    g_assert_true (found);

    GIcon *emblem_icon = g_themed_icon_new ("emblem-symbolic-link");
    GEmblem *emblem = g_emblem_new (emblem_icon);
    GIcon *emblemed = g_emblemed_icon_new (G_ICON (pixbuf), emblem);
    NemoIconInfo *info = nemo_icon_info_lookup_for_context (emblemed, 32, 1, dark);
    GdkPixbuf *with_emblem = nemo_icon_info_get_pixbuf (info);
    g_assert_cmphex (opaque_pixel (with_emblem), ==, 0xc07020);
    g_assert_cmpuint (gdk_pixbuf_get_byte_length (with_emblem), ==,
                     gdk_pixbuf_get_byte_length (pixbuf));
    g_assert_cmpint (memcmp (gdk_pixbuf_get_pixels (with_emblem), gdk_pixbuf_get_pixels (pixbuf),
                            gdk_pixbuf_get_byte_length (pixbuf)), !=, 0);
    g_object_unref (with_emblem);
    nemo_icon_info_unref (info);
    g_object_unref (emblemed);
    g_object_unref (emblem);
    g_object_unref (emblem_icon);
    g_object_unref (pixbuf);
    nemo_file_unref (file);
    g_object_unref (art);
    drain_events ();
    g_assert_cmpint (g_unlink (cover), ==, 0);
    g_assert_cmpint (g_rmdir (directory), ==, 0);
    g_free (cover);
    g_free (directory);
}

static void
test_cover_cache_lifecycle (void)
{
    char *directory = g_build_filename (fixture, "cover-lifecycle", NULL);
    char *renamed = g_build_filename (fixture, "cover-renamed", NULL);
    char *cover = g_build_filename (directory, "cover.png", NULL);
    g_assert_cmpint (g_mkdir (directory, 0700), ==, 0);
    save_cover_color (cover, 0xc07020ff);
    NemoFile *file = file_with_info (directory, G_FILE_TYPE_DIRECTORY, "inode/directory");
    assert_file_color_eventually (file, 0xc07020);
    NemoIconInfo *old = nemo_file_get_icon_for_context (file, 32, 0, 1, 0, dark);
    NemoIconInfo *cached = nemo_file_get_icon_for_context (file, 32, 0, 1, 0, light);
    g_assert_true (old == cached);
    nemo_icon_info_unref (cached);

    save_cover_color (cover, 0x3080b0ff);
    assert_file_color_eventually (file, 0x3080b0);
    GdkPixbuf *old_pixels = nemo_icon_info_get_pixbuf (old);
    g_assert_cmphex (opaque_pixel (old_pixels), ==, 0xc07020);
    g_object_unref (old_pixels);
    nemo_icon_info_unref (old);
    g_assert_cmpint (g_unlink (cover), ==, 0);
    assert_file_color_eventually (file, 0xe0d0c0);
    save_cover_color (cover, 0x70a030ff);
    assert_file_color_eventually (file, 0x70a030);

    g_assert_cmpint (g_rename (directory, renamed), ==, 0);
    g_assert_true (nemo_file_update_name (file, "cover-renamed"));
    g_free (cover);
    cover = g_build_filename (renamed, "cover.png", NULL);
    assert_file_color_eventually (file, 0x70a030);

    file->details->got_link_info = TRUE;
    file->details->custom_icon = G_ICON (solid_pixbuf (0x402080ff));
    save_cover_color (cover, 0x207060ff);
    for (guint i = 0; i < 30; i++) {
        drain_events ();
        assert_file_color_eventually (file, 0x402080);
        g_usleep (5000);
    }
    g_clear_object (&file->details->custom_icon);
    file->details->custom_icon = G_ICON (solid_pixbuf (0x603050ff));
    assert_file_color_eventually (file, 0x603050);
    g_clear_object (&file->details->custom_icon);
    assert_file_color_eventually (file, 0x207060);
    nemo_file_invalidate_attributes (file, NEMO_FILE_ATTRIBUTE_INFO);
    assert_file_color_eventually (file, 0x207060);

    nemo_file_unref (file);
    drain_events ();
    g_assert_cmpint (g_unlink (cover), ==, 0);
    g_assert_cmpint (g_rmdir (renamed), ==, 0);
    g_free (cover);
    g_free (renamed);
    g_free (directory);
}

static void
test_cover_pending_preference_and_destroy (void)
{
    char *directory = g_build_filename (fixture, "cover-pending", NULL);
    char *cover = g_build_filename (directory, "cover.png", NULL);
    g_assert_cmpint (g_mkdir (directory, 0700), ==, 0);
    save_cover_color (cover, 0xa04030ff);
    NemoFile *file = file_with_info (directory, G_FILE_TYPE_DIRECTORY, "inode/directory");
    GFile *cover_location = g_file_new_for_path (cover);
    NemoFile *pending;
    gint64 deadline;

    for (guint i = 0; i < 3; i++) {
        g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_SYMBOLIC_FILE_ICONS, i != 1);
        NemoIconInfo *info = nemo_file_get_icon_for_context (file, 32, 0, 1, 0, dark);
        if (i != 1)
            g_assert_cmpstr (nemo_icon_info_get_used_name (info), ==, "folder-symbolic");
        nemo_icon_info_unref (info);
        /* Rendering only queued the idle; no metadata discovery ran here. */
        g_assert_null (nemo_file_get_existing (cover_location));
    }
    deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
    do {
        g_main_context_iteration (NULL, FALSE);
        pending = nemo_file_get_existing (cover_location);
    } while (pending == NULL && g_get_monotonic_time () < deadline);
    g_assert_nonnull (pending);
    nemo_file_unref (pending);

    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_SYMBOLIC_FILE_ICONS, FALSE);
    assert_file_color_eventually (file, 0xa04030);
    drain_events ();
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_SYMBOLIC_FILE_ICONS, TRUE);
    assert_file_color_eventually (file, 0xa04030);
    nemo_file_unref (file);
    drain_events ();

    for (guint stage = 0; stage < 2; stage++) {
        file = file_with_info (directory, G_FILE_TYPE_DIRECTORY, "inode/directory");
        NemoIconInfo *info = nemo_file_get_icon_for_context (file, 32, 0, 1, 0, dark);
        nemo_icon_info_unref (info);
        if (stage == 1) {
            deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
            do {
                g_main_context_iteration (NULL, FALSE);
                pending = nemo_file_get_existing (cover_location);
            } while (pending == NULL && g_get_monotonic_time () < deadline);
            g_assert_nonnull (pending);
            nemo_file_unref (pending);
        }
        g_object_add_weak_pointer (G_OBJECT (file), (gpointer *) &file);
        nemo_file_unref (file);
        drain_events ();
        g_assert_null (file);
    }
    g_object_unref (cover_location);
    g_assert_cmpint (g_unlink (cover), ==, 0);
    g_assert_cmpint (g_rmdir (directory), ==, 0);
    g_free (cover);
    g_free (directory);
}

static void
prepare_fixture (void)
{
    char *relative = g_strdup_printf ("symbolic-icons-fixture-%u", (guint) getpid ());
    fixture = g_canonicalize_filename (relative, NULL);
    g_free (relative);
    g_assert_cmpint (g_mkdir (fixture, 0700), ==, 0);
    const char *directories[] = { "home", "config" };
    for (guint i = 0; i < G_N_ELEMENTS (directories); i++) {
        char *path = g_build_filename (fixture, directories[i], NULL);
        g_assert_cmpint (g_mkdir (path, 0700), ==, 0);
        g_setenv (i == 0 ? "HOME" : "XDG_CONFIG_HOME", path, TRUE);
        g_free (path);
    }
    char *config = g_build_filename (fixture, "config", "user-dirs.dirs", NULL);
    g_assert_true (g_file_set_contents (config,
        "XDG_DESKTOP_DIR=\"$HOME/Desktop\"\nXDG_DOCUMENTS_DIR=\"$HOME/Documents\"\n"
        "XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\nXDG_MUSIC_DIR=\"$HOME/Music\"\n"
        "XDG_PICTURES_DIR=\"$HOME/Pictures\"\nXDG_PUBLICSHARE_DIR=\"$HOME/Public\"\n"
        "XDG_TEMPLATES_DIR=\"$HOME/Templates\"\nXDG_VIDEOS_DIR=\"$HOME/Videos\"\n", -1, NULL));
    g_free (config);

    for (guint i = 0; i < 2; i++) {
        char *directory = g_build_filename (fixture, i == 0 ? "SymbolicFixtureA" : "SymbolicFixtureB",
                                            "scalable", NULL);
        g_assert_cmpint (g_mkdir_with_parents (directory, 0700), ==, 0);
        char *index = g_build_filename (directory, "..", "index.theme", NULL);
        g_assert_true (g_file_set_contents (index,
            "[Icon Theme]\nName=Fixture\nDirectories=scalable\n"
            "[scalable]\nSize=16\nType=Scalable\nMinSize=8\nMaxSize=256\nContext=MimeTypes\n",
            -1, NULL));
        char *image = g_build_filename (directory, "nemo-fixture-symbolic.svg", NULL);
        g_assert_true (g_file_set_contents (image, i == 0 ?
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\">"
            "<path fill=\"#2e3436\" d=\"M2 2h12v12H2z\"/></svg>" :
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\">"
            "<path fill=\"#2e3436\" d=\"M6 6h4v4H6z\"/></svg>", -1, NULL));
        g_free (image);
        g_free (index);
        g_free (directory);
    }
}

static void
remove_fixture (const char *path)
{
    GDir *dir = g_dir_open (path, 0, NULL);
    const char *name;
    g_assert_nonnull (dir);
    while ((name = g_dir_read_name (dir)) != NULL) {
        char *child = g_build_filename (path, name, NULL);
        if (g_file_test (child, G_FILE_TEST_IS_DIR))
            remove_fixture (child);
        else
            g_assert_cmpint (g_unlink (child), ==, 0);
        g_free (child);
    }
    g_dir_close (dir);
    g_assert_cmpint (g_rmdir (path), ==, 0);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    prepare_fixture ();
    gtk_test_init (&argc, &argv, NULL);
    nemo_global_preferences_init ();
    nemo_icon_fallback_init ();
    theme = gtk_icon_theme_get_default ();
    light = new_context ("* { color: #203040; } *:selected { color: #ffffff; }");
    dark = new_context ("* { color: #e0d0c0; }");
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_SYMBOLIC_FILE_ICONS, TRUE);
    g_test_add_func ("/symbolic-icons/context-colors-selection-scale", test_context_colors);
    g_test_add_func ("/symbolic-icons/theme-change-missing", test_theme_change_and_missing);
    g_test_add_func ("/symbolic-icons/xdg-special-folders", test_special_folders);
    g_test_add_func ("/symbolic-icons/preference-legacy", test_preference_and_legacy);
    g_test_add_func ("/symbolic-icons/thumbnail-custom-priority", test_thumbnail_custom_priority);
    g_test_add_func ("/symbolic-icons/cover-emblems", test_cover_and_emblems);
    g_test_add_func ("/symbolic-icons/cover-cache-lifecycle", test_cover_cache_lifecycle);
    g_test_add_func ("/symbolic-icons/cover-pending-preference-destroy", test_cover_pending_preference_and_destroy);
    int result = g_test_run ();
    g_object_unref (light);
    g_object_unref (dark);
    drain_events ();
    remove_fixture (fixture);
    g_free (fixture);
    return result;
}
