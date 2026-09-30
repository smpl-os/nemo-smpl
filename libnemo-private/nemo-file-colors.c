/* File-type hints use cached metadata and adapt to the actual view background. */
#include <config.h>
#include "nemo-file-colors.h"
#include "nemo-global-preferences.h"
#include "nemo-ui-utilities.h"
#include <math.h>
#include <string.h>

static gboolean
extension_matches (const char *extension, const char *const *extensions)
{
    for (guint i = 0; extensions[i] != NULL; i++)
        if (g_ascii_strcasecmp (extension, extensions[i]) == 0)
            return TRUE;
    return FALSE;
}

NemoFileColorKind
nemo_file_color_kind_for_type (const char *name, const char *mime_type, gboolean directory)
{
    static const char *const archives[] = {
        "zip", "7z", "rar", "tar", "gz", "bz2", "xz", "zst", "lz", "lz4", "tgz", "iso", NULL
    };
    static const char *const scripts[] = {
        "sh", "bash", "zsh", "fish", "py", "pyw", "rb", "pl", "ps1", "bat", "cmd", "appimage", NULL
    };
    static const char *const source[] = {
        "c", "h", "cc", "cpp", "cxx", "hpp", "rs", "go", "java", "js", "jsx", "ts", "tsx",
        "html", "htm", "css", "scss", "json", "yaml", "yml", "toml", "xml", "ini", "conf", "sql", NULL
    };
    static const char *const documents[] = {
        "pdf", "txt", "md", "rst", "rtf", "doc", "docx", "odt", "ods", "odp", "xls", "xlsx", "csv", NULL
    };
    if (directory)
        return NEMO_FILE_COLOR_FOLDER;
    const char *extension = name != NULL ? strrchr (name, '.') : NULL;
    extension = extension != NULL ? extension + 1 : "";
    const char *mime = mime_type != NULL ? mime_type : "";
    if (g_str_has_prefix (mime, "image/"))
        return NEMO_FILE_COLOR_IMAGE;
    if (g_str_has_prefix (mime, "video/"))
        return NEMO_FILE_COLOR_VIDEO;
    if (g_str_has_prefix (mime, "audio/"))
        return NEMO_FILE_COLOR_AUDIO;
    if (extension_matches (extension, archives) ||
        g_str_equal (mime, "application/zip") || g_str_equal (mime, "application/x-tar") ||
        g_str_equal (mime, "application/gzip") || g_str_equal (mime, "application/x-7z-compressed") ||
        g_str_equal (mime, "application/x-rar"))
        return NEMO_FILE_COLOR_ARCHIVE;
    if (extension_matches (extension, scripts) ||
        g_str_equal (mime, "application/x-executable") ||
        g_str_equal (mime, "application/x-pie-executable") ||
        g_str_equal (mime, "application/x-shellscript"))
        return NEMO_FILE_COLOR_EXECUTABLE;
    if (extension_matches (extension, source) ||
        g_str_equal (mime, "application/json") || g_str_equal (mime, "application/xml"))
        return NEMO_FILE_COLOR_SOURCE;
    if (extension_matches (extension, documents) || g_str_has_prefix (mime, "text/") ||
        g_str_equal (mime, "application/pdf") ||
        g_str_has_prefix (mime, "application/vnd.oasis.opendocument.") ||
        g_str_has_prefix (mime, "application/vnd.openxmlformats-officedocument."))
        return NEMO_FILE_COLOR_DOCUMENT;
    return NEMO_FILE_COLOR_DEFAULT;
}

NemoFileColorKind
nemo_file_get_color_kind (NemoFile *file)
{
    g_return_val_if_fail (NEMO_IS_FILE (file), NEMO_FILE_COLOR_DEFAULT);
    g_autofree char *name = nemo_file_get_name (file);
    g_autofree char *mime = nemo_file_get_mime_type (file);
    return nemo_file_color_kind_for_type (name, mime, nemo_file_is_directory (file));
}

#ifdef NEMO_SMPL
static double
linear_component (double value)
{
    return value <= 0.04045 ? value / 12.92 : pow ((value + 0.055) / 1.055, 2.4);
}

static double
luminance (const GdkRGBA *color)
{
    return 0.2126 * linear_component (color->red) +
           0.7152 * linear_component (color->green) +
           0.0722 * linear_component (color->blue);
}

static double
contrast (const GdkRGBA *foreground, const GdkRGBA *background)
{
    double a = luminance (foreground), b = luminance (background);
    return (MAX (a, b) + 0.05) / (MIN (a, b) + 0.05);
}

#endif

gboolean
nemo_file_color_for_kind (NemoFileColorKind kind, GtkStyleContext *context,
                          GtkStateFlags state, GdkRGBA *color)
{
    g_return_val_if_fail (kind >= NEMO_FILE_COLOR_DEFAULT && kind < NEMO_FILE_COLOR_COUNT, FALSE);
    g_return_val_if_fail (GTK_IS_STYLE_CONTEXT (context) && color != NULL, FALSE);
#ifdef NEMO_SMPL
    static const GdkRGBA palette[NEMO_FILE_COLOR_COUNT] = {
        { 0, 0, 0, 1 }, { 0.15, 0.64, 0.87, 1 }, { 0.76, 0.63, 0.12, 1 },
        { 0.66, 0.40, 0.80, 1 }, { 0.80, 0.42, 0.65, 1 }, { 0.86, 0.32, 0.22, 1 },
        { 0.14, 0.64, 0.55, 1 }, { 0.70, 0.59, 0.20, 1 }, { 0.30, 0.66, 0.30, 1 }
    };
    static const GdkRGBA bright_palette[NEMO_FILE_COLOR_COUNT] = {
        [NEMO_FILE_COLOR_FOLDER] = { 0.42, 0.82, 1.00, 1 },
        [NEMO_FILE_COLOR_IMAGE] = { 1.00, 0.86, 0.38, 1 },
        [NEMO_FILE_COLOR_VIDEO] = { 0.82, 0.66, 1.00, 1 },
        [NEMO_FILE_COLOR_AUDIO] = { 1.00, 0.65, 0.84, 1 },
        [NEMO_FILE_COLOR_ARCHIVE] = { 1.00, 0.56, 0.49, 1 },
        [NEMO_FILE_COLOR_SOURCE] = { 0.95, 0.83, 0.43, 1 }
    };
    if ((state & (GTK_STATE_FLAG_SELECTED | GTK_STATE_FLAG_INSENSITIVE)) != 0 ||
        nemo_preferences == NULL ||
        !g_settings_get_boolean (nemo_preferences, NEMO_PREFERENCES_FILE_TYPE_COLORS))
        return FALSE;

    GtkSettings *settings = gtk_settings_get_for_screen (gtk_style_context_get_screen (context));
    g_autofree char *theme = NULL;
    g_object_get (settings, "gtk-theme-name", &theme, NULL);
    g_autofree char *lower_theme = g_ascii_strdown (theme != NULL ? theme : "", -1);
    if (strstr (lower_theme, "highcontrast") != NULL || strstr (lower_theme, "high-contrast") != NULL)
        return FALSE;

    GdkRGBA background;
    if (!nemo_ui_get_background_color (context, state, &background))
        return FALSE;

    gboolean dark = luminance (&background) < 0.179;
    gboolean brighten = dark && kind != NEMO_FILE_COLOR_DOCUMENT && kind != NEMO_FILE_COLOR_EXECUTABLE;
    *color = palette[kind];
    if (kind == NEMO_FILE_COLOR_DEFAULT) {
        if (!dark)
            return FALSE;
        GdkRGBA foreground;
        gtk_style_context_get_color (context, state, &foreground);
        double highest = MAX (foreground.red, MAX (foreground.green, foreground.blue));
        double lowest = MIN (foreground.red, MIN (foreground.green, foreground.blue));
        /* Only neutral text is lifted; theme greens and other accents stay intact. */
        if (foreground.alpha <= 0.0 || highest - lowest > highest * 0.12)
            return FALSE;
        *color = (GdkRGBA) {
            foreground.red * foreground.alpha + background.red * (1.0 - foreground.alpha),
            foreground.green * foreground.alpha + background.green * (1.0 - foreground.alpha),
            foreground.blue * foreground.alpha + background.blue * (1.0 - foreground.alpha), 1
        };
        highest = MAX (color->red, MAX (color->green, color->blue));
        if (highest >= 0.82 && contrast (color, &background) >= 7.1)
            return FALSE;
        if (highest < 0.82) {
            double amount = (0.82 - highest) / (1.0 - highest);
            color->red += amount * (1.0 - color->red);
            color->green += amount * (1.0 - color->green);
            color->blue += amount * (1.0 - color->blue);
        }
    } else if (brighten) {
        *color = bright_palette[kind];
    }
    GdkRGBA white = { 1, 1, 1, 1 };
    double goal = brighten ? MIN (7.1, contrast (&white, &background)) : 4.6;
    if (contrast (color, &background) >= goal)
        return TRUE;

    /* Keep headroom for Pango channel rounding; mid-grey backgrounds may
     * require pure white, since even white cannot reach 7:1 there. */
    double pole = dark ? 1.0 : 0.0;
    double low = 0.0, high = 1.0;
    for (guint i = 0; i < 24; i++) {
        double amount = (low + high) / 2.0;
        GdkRGBA candidate = {
            color->red + amount * (pole - color->red),
            color->green + amount * (pole - color->green),
            color->blue + amount * (pole - color->blue), 1.0
        };
        if (contrast (&candidate, &background) >= goal)
            high = amount;
        else
            low = amount;
    }
    color->red += high * (pole - color->red);
    color->green += high * (pole - color->green);
    color->blue += high * (pole - color->blue);
    return TRUE;
#else
    return FALSE;
#endif
}
