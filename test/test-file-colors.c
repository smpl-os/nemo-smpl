#include <config.h>
#include <libnemo-private/nemo-file-colors.h>
#include <libnemo-private/nemo-global-preferences.h>
#include <math.h>

static double
channel (double value)
{
    return value <= 0.04045 ? value / 12.92 : pow ((value + 0.055) / 1.055, 2.4);
}

static double
contrast_ratio (GdkRGBA foreground, GdkRGBA background)
{
    double a = 0.2126 * channel (foreground.red) + 0.7152 * channel (foreground.green) +
               0.0722 * channel (foreground.blue);
    double b = 0.2126 * channel (background.red) + 0.7152 * channel (background.green) +
               0.0722 * channel (background.blue);
    return (MAX (a, b) + 0.05) / (MIN (a, b) + 0.05);
}

static GtkStyleContext *
context_with_background (const char *background, GtkCssProvider **provider_out)
{
    GtkStyleContext *context = gtk_style_context_new ();
    GtkWidgetPath *path = gtk_widget_path_new ();
    gtk_widget_path_append_type (path, GTK_TYPE_TREE_VIEW);
    gtk_widget_path_iter_add_class (path, -1, "view");
    gtk_style_context_set_path (context, path);
    gtk_widget_path_unref (path);
    GtkCssProvider *provider = gtk_css_provider_new ();
    g_autofree char *css = g_strdup_printf ("* { background-color: %s; color: white; }", background);
    GError *error = NULL;
    g_assert_true (gtk_css_provider_load_from_data (provider, css, -1, &error));
    g_assert_no_error (error);
    gtk_style_context_add_provider (context, GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    if (provider_out != NULL)
        *provider_out = provider;
    else
        g_object_unref (provider);
    return context;
}

static void
test_categories (void)
{
    static const struct { const char *name, *mime; gboolean folder; NemoFileColorKind kind; } cases[] = {
        { "Documents", "inode/directory", TRUE, NEMO_FILE_COLOR_FOLDER },
        { "image.JPG", "image/jpeg", FALSE, NEMO_FILE_COLOR_IMAGE },
        { "photo.dng", "image/x-adobe-dng", FALSE, NEMO_FILE_COLOR_IMAGE },
        { "movie", "video/mp4", FALSE, NEMO_FILE_COLOR_VIDEO },
        { "song.flac", "audio/flac", FALSE, NEMO_FILE_COLOR_AUDIO },
        { "backup.TAR.ZST", "application/octet-stream", FALSE, NEMO_FILE_COLOR_ARCHIVE },
        { "script.SH", "text/plain", FALSE, NEMO_FILE_COLOR_EXECUTABLE },
        { "program", "application/x-pie-executable", FALSE, NEMO_FILE_COLOR_EXECUTABLE },
        { "notes.md", "text/markdown", FALSE, NEMO_FILE_COLOR_DOCUMENT },
        { "report", "application/pdf", FALSE, NEMO_FILE_COLOR_DOCUMENT },
        { "config.JSON", "application/json", FALSE, NEMO_FILE_COLOR_SOURCE },
        { "source.rs", "text/plain", FALSE, NEMO_FILE_COLOR_SOURCE },
        { "folder.zip", "application/zip", TRUE, NEMO_FILE_COLOR_FOLDER },
        { "unknown.bin", "application/octet-stream", FALSE, NEMO_FILE_COLOR_DEFAULT },
        { NULL, NULL, FALSE, NEMO_FILE_COLOR_DEFAULT },
    };
    for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
        g_assert_cmpint (nemo_file_color_kind_for_type (cases[i].name, cases[i].mime, cases[i].folder),
                         ==, cases[i].kind);
}

static void
test_contrast (void)
{
    const char *backgrounds[] = {
        "#000000", "#ffffff", "#1e1e2e", "#f4e8cd", "#737373",
        "#777777", "#354d29", "#664533", "#f0ffff", "#d7b7ee"
    };
    for (guint i = 0; i < G_N_ELEMENTS (backgrounds); i++) {
        GtkStyleContext *context = context_with_background (backgrounds[i], NULL);
        GdkRGBA background;
        g_assert_true (gdk_rgba_parse (&background, backgrounds[i]));
        for (gint kind = NEMO_FILE_COLOR_FOLDER; kind < NEMO_FILE_COLOR_COUNT; kind++) {
            GdkRGBA color;
            g_assert_true (nemo_file_color_for_kind (kind, context, GTK_STATE_FLAG_NORMAL, &color));
            color.red = (guint16) (color.red * 65535) / 65535.0;
            color.green = (guint16) (color.green * 65535) / 65535.0;
            color.blue = (guint16) (color.blue * 65535) / 65535.0;
            g_assert_cmpfloat (contrast_ratio (color, background), >=, 4.5);
            g_assert_cmpfloat (color.alpha, ==, 1.0);
        }
        g_object_unref (context);
    }
}

static void
test_brighter_dark_palette (void)
{
    static const struct { NemoFileColorKind kind; GdkRGBA previous; } changed[] = {
        { NEMO_FILE_COLOR_FOLDER, { .15, .64, .87, 1 } },
        { NEMO_FILE_COLOR_IMAGE, { .76, .63, .12, 1 } },
        { NEMO_FILE_COLOR_VIDEO, { .66, .40, .80, 1 } },
        { NEMO_FILE_COLOR_AUDIO, { .80, .42, .65, 1 } },
        { NEMO_FILE_COLOR_ARCHIVE, { .86, .32, .22, 1 } },
        { NEMO_FILE_COLOR_SOURCE, { .70, .59, .20, 1 } },
    };
    const char *backgrounds[] = { "#000000", "#202020", "#1e1e2e" };
    for (guint b = 0; b < G_N_ELEMENTS (backgrounds); b++) {
        GtkStyleContext *context = context_with_background (backgrounds[b], NULL);
        GdkRGBA background, color;
        g_assert_true (gdk_rgba_parse (&background, backgrounds[b]));
        for (guint i = 0; i < G_N_ELEMENTS (changed); i++) {
            g_assert_true (nemo_file_color_for_kind (changed[i].kind, context, 0, &color));
            g_assert_cmpfloat (contrast_ratio (color, background), >=, 7.0);
            g_assert_cmpfloat (contrast_ratio (color, background), >,
                               1.25 * contrast_ratio (changed[i].previous, background));
        }
        GdkRGBA green = { .30, .66, .30, 1 }, teal = { .14, .64, .55, 1 };
        g_assert_true (nemo_file_color_for_kind (NEMO_FILE_COLOR_EXECUTABLE, context, 0, &color));
        g_assert_true (gdk_rgba_equal (&green, &color));
        g_assert_true (nemo_file_color_for_kind (NEMO_FILE_COLOR_DOCUMENT, context, 0, &color));
        g_assert_true (gdk_rgba_equal (&teal, &color));
        g_object_unref (context);
    }
    GtkStyleContext *light = context_with_background ("white", NULL);
    GdkRGBA color;
    g_assert_true (nemo_file_color_for_kind (NEMO_FILE_COLOR_FOLDER, light, 0, &color));
    g_assert_cmpfloat_with_epsilon (color.red, .114871725, .000001);
    g_assert_cmpfloat_with_epsilon (color.green, .490119362, .000001);
    g_assert_cmpfloat_with_epsilon (color.blue, .666256008, .000001);
    g_assert_true (nemo_file_color_for_kind (NEMO_FILE_COLOR_ARCHIVE, light, 0, &color));
    g_assert_cmpfloat_with_epsilon (color.red, .791550274, .000001);
    g_assert_cmpfloat_with_epsilon (color.green, .294530334, .000001);
    g_assert_cmpfloat_with_epsilon (color.blue, .202489605, .000001);
    g_object_unref (light);
}

static void
test_grey_text (void)
{
    GtkCssProvider *provider;
    GtkStyleContext *context = context_with_background ("black", &provider);
    GdkRGBA color, black = { 0, 0, 0, 1 };
    gtk_css_provider_load_from_data (provider,
        "* { background-color: black; color: rgba(128,128,128,0.6); }", -1, NULL);
    g_assert_true (nemo_file_color_for_kind (NEMO_FILE_COLOR_DEFAULT, context, 0, &color));
    g_assert_cmpfloat (color.red, >=, .81);
    g_assert_cmpfloat (color.alpha, ==, 1.0);
    g_assert_cmpfloat (contrast_ratio (color, black), >=, 7.0);
    g_assert_false (nemo_file_color_for_kind (NEMO_FILE_COLOR_DEFAULT, context, GTK_STATE_FLAG_SELECTED, &color));
    g_assert_false (nemo_file_color_for_kind (NEMO_FILE_COLOR_DEFAULT, context, GTK_STATE_FLAG_INSENSITIVE, &color));
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, FALSE);
    g_assert_false (nemo_file_color_for_kind (NEMO_FILE_COLOR_DEFAULT, context, 0, &color));
    g_settings_reset (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS);
    gtk_css_provider_load_from_data (provider, "* { background-color: black; color: lime; }", -1, NULL);
    g_assert_false (nemo_file_color_for_kind (NEMO_FILE_COLOR_DEFAULT, context, 0, &color));
    gtk_css_provider_load_from_data (provider, "* { background-color: white; color: grey; }", -1, NULL);
    g_assert_false (nemo_file_color_for_kind (NEMO_FILE_COLOR_DEFAULT, context, 0, &color));
    g_object_unref (provider);
    g_object_unref (context);
}

static void
test_theme_and_states (void)
{
    GtkCssProvider *provider;
    GtkStyleContext *context = context_with_background ("#000000", &provider);
    GdkRGBA dark, light;
    g_assert_true (nemo_file_color_for_kind (NEMO_FILE_COLOR_FOLDER, context, 0, &dark));
    gtk_css_provider_load_from_data (provider, "* { background-color: white; color: black; }", -1, NULL);
    g_assert_true (nemo_file_color_for_kind (NEMO_FILE_COLOR_FOLDER, context, 0, &light));
    g_assert_false (gdk_rgba_equal (&dark, &light));
    g_assert_false (nemo_file_color_for_kind (NEMO_FILE_COLOR_FOLDER, context, GTK_STATE_FLAG_SELECTED, &light));
    g_assert_false (nemo_file_color_for_kind (NEMO_FILE_COLOR_FOLDER, context, GTK_STATE_FLAG_INSENSITIVE, &light));
    g_assert_false (nemo_file_color_for_kind (NEMO_FILE_COLOR_DEFAULT, context, 0, &light));
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS, FALSE);
    g_assert_false (nemo_file_color_for_kind (NEMO_FILE_COLOR_FOLDER, context, 0, &light));
    g_settings_reset (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS);
    g_assert_true (nemo_file_color_for_kind (NEMO_FILE_COLOR_FOLDER, context, 0, &light));
    GtkSettings *settings = gtk_settings_get_default ();
    char *theme;
    g_object_get (settings, "gtk-theme-name", &theme, NULL);
    g_object_set (settings, "gtk-theme-name", "HighContrast", NULL);
    g_assert_false (nemo_file_color_for_kind (NEMO_FILE_COLOR_FOLDER, context, 0, &light));
    g_object_set (settings, "gtk-theme-name", theme, NULL);
    g_free (theme);
    g_object_unref (provider);
    g_object_unref (context);
}

static void
test_transparent_background (void)
{
    GtkStyleContext *parent = context_with_background ("white", NULL);
    GtkStyleContext *child = context_with_background ("rgba(0,0,0,0.5)", NULL);
    gtk_style_context_set_parent (child, parent);
    GdkRGBA color, background = { 0.5, 0.5, 0.5, 1 };
    g_assert_true (nemo_file_color_for_kind (NEMO_FILE_COLOR_IMAGE, child, 0, &color));
    g_assert_cmpfloat (contrast_ratio (color, background), >=, 4.5);
    g_object_unref (child);
    g_object_unref (parent);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0)
        return 77;
    gtk_test_init (&argc, &argv, NULL);
    nemo_global_preferences_init ();
    g_test_add_func ("/file-colors/categories", test_categories);
    g_test_add_func ("/file-colors/contrast", test_contrast);
    g_test_add_func ("/file-colors/brighter-dark-palette", test_brighter_dark_palette);
    g_test_add_func ("/file-colors/grey-text", test_grey_text);
    g_test_add_func ("/file-colors/theme-states-preference", test_theme_and_states);
    g_test_add_func ("/file-colors/transparent-background", test_transparent_background);
    return g_test_run ();
}
