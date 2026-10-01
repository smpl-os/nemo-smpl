#include <config.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <stdint.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#endif
#include "../src/nemo-main-application.h"
#include "../src/nemo-window-private.h"
#include "../src/nemo-window-slot.h"
#include "../src/nemo-view.h"
#include "../src/nemo-preview-pane.h"
#include "../src/nemo-quick-preview.h"
#include "../src/nemo-window-alpha.h"
#include "../src/nemo-style-utilities.h"

static gboolean without_rgba;

GdkVisual *__real_gdk_screen_get_rgba_visual (GdkScreen *screen);

GdkVisual *
__wrap_gdk_screen_get_rgba_visual (GdkScreen *screen)
{
    return without_rgba ? NULL : __real_gdk_screen_get_rgba_visual (screen);
}

static void
settle (void)
{
    gint64 deadline = g_get_monotonic_time () + 300 * G_TIME_SPAN_MILLISECOND;
    do {
        while (g_main_context_iteration (NULL, FALSE));
        g_usleep (1000);
    } while (g_get_monotonic_time () < deadline);
}

static cairo_surface_t *
snapshot (GtkWidget *widget)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel (widget);
    if (GTK_IS_CONTAINER (toplevel))
        gtk_container_check_resize (GTK_CONTAINER (toplevel));
    cairo_surface_t *surface = cairo_image_surface_create (
        CAIRO_FORMAT_ARGB32, gtk_widget_get_allocated_width (widget),
        gtk_widget_get_allocated_height (widget));
    cairo_t *cr = cairo_create (surface);
    gtk_widget_draw (widget, cr);
    cairo_destroy (cr);
    cairo_surface_flush (surface);
    return surface;
}

/* Claiming the X11 compositor selection tests GDK's real availability events,
 * not desktop blending: pixels below are still rendered to Cairo surfaces. */
static void
set_compositor_available (gboolean available)
{
#ifdef GDK_WINDOWING_X11
    static Window owner;
    GdkScreen *screen = gdk_screen_get_default ();
    Display *display = gdk_x11_display_get_xdisplay (gdk_screen_get_display (screen));
    gchar *name = g_strdup_printf ("_NET_WM_CM_S%d",
                                   gdk_x11_screen_get_screen_number (screen));
    Atom selection = XInternAtom (display, name, False);
    g_free (name);
    if (available) {
        g_assert_cmpuint (owner, ==, None);
        owner = XCreateSimpleWindow (display, DefaultRootWindow (display),
                                    0, 0, 1, 1, 0, 0, 0);
        XSetSelectionOwner (display, selection, owner, CurrentTime);
    } else {
        XSetSelectionOwner (display, selection, None, CurrentTime);
        if (owner != None)
            XDestroyWindow (display, owner);
        owner = None;
    }
    XFlush (display);
    settle ();
    g_assert_cmpint (gdk_screen_is_composited (screen), ==, available);
#endif
}

static guint32
pixel_at (cairo_surface_t *surface, gint x, gint y)
{
    return *(guint32 *) (cairo_image_surface_get_data (surface) +
        y * cairo_image_surface_get_stride (surface) + x * 4);
}

static void
load_css (GtkCssProvider *provider, const gchar *css)
{
    GError *error = NULL;
    gtk_css_provider_load_from_data (provider, css, -1, &error);
    g_assert_no_error (error);
    settle ();
}

static void
assert_opaque_region (GtkWidget *widget, gboolean opaque)
{
#ifdef GDK_WINDOWING_X11
    GdkDisplay *display = gtk_widget_get_display (widget);
    if (GDK_IS_X11_DISPLAY (display)) {
        Display *xdisplay = gdk_x11_display_get_xdisplay (display);
        Atom property = XInternAtom (xdisplay, "_NET_WM_OPAQUE_REGION", False);
        Atom type;
        gint format;
        gulong count, remaining;
        guchar *data = NULL;
        g_assert_cmpint (XGetWindowProperty (xdisplay,
            gdk_x11_window_get_xid (gtk_widget_get_window (widget)),
            property, 0, 4, False, XA_CARDINAL, &type, &format,
            &count, &remaining, &data), ==, Success);
        g_assert_cmpuint (count, ==, opaque ? 4 : 0);
        if (opaque) {
            gulong *rect = (gulong *) data;
            g_assert_cmpuint (rect[2], ==, gtk_widget_get_allocated_width (widget));
            g_assert_cmpuint (rect[3], ==, gtk_widget_get_allocated_height (widget));
        }
        if (data != NULL)
            XFree (data);
    }
#endif
}

static void
test_pixels (void)
{
    GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label = gtk_label_new ("MMMM");
    GdkPixbuf *icon = gdk_pixbuf_new (GDK_COLORSPACE_RGB, TRUE, 8, 16, 16);
    GtkCssProvider *provider = gtk_css_provider_new ();
    GtkCssProvider *fallback = gtk_css_provider_new ();
    GtkStyleContext *context = gtk_widget_get_style_context (window);
    const gdouble alphas[] = { 0.55, 1.0, 0.55 };

    gtk_window_set_decorated (GTK_WINDOW (window), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (window), 240, 100);
    gtk_style_context_add_class (context, "nemo-window");
    set_compositor_available (TRUE);
    without_rgba = TRUE;
    nemo_window_enable_native_alpha (window);
    g_assert_false (gtk_style_context_has_class (context, "smplos-native-alpha"));
    without_rgba = FALSE;
    set_compositor_available (FALSE);
    nemo_window_enable_native_alpha (window);
    g_assert_false (gtk_style_context_has_class (context, "smplos-native-alpha"));
    set_compositor_available (TRUE);
    g_assert_true (gtk_style_context_has_class (context, "smplos-native-alpha"));
    g_assert_true (gtk_widget_get_visual (window) ==
                   gdk_screen_get_rgba_visual (gtk_widget_get_screen (window)));
    gtk_container_add (GTK_CONTAINER (window), box);
    nemo_label_set_secondary (label, 70);
    gtk_box_pack_start (GTK_BOX (box), label, TRUE, TRUE, 0);
    gdk_pixbuf_fill (icon, 0x00ff00ff);
    gtk_box_pack_end (GTK_BOX (box), gtk_image_new_from_pixbuf (icon), FALSE, FALSE, 10);
    gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
        GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
    gtk_css_provider_load_from_resource (fallback, "/org/nemo/nemo-style-application.css");
    gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
        GTK_STYLE_PROVIDER (fallback), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    gtk_widget_show_all (window);

    for (guint i = 0; i < G_N_ELEMENTS (alphas); i++) {
        gchar *css = g_strdup_printf (
            "window {background-image:none;background-color:#14283c;} "
            ".nemo-window.smplos-native-alpha {background-color:rgba(20,40,60,%.2f);} "
            "box {background-image:none;background-color:transparent;} "
            "label {font:bold 28px monospace;color:white;} "
            ".smplos-native-alpha .nemo-secondary-label {opacity:1;}", alphas[i]);
        load_css (provider, css);
        g_free (css);
        for (guint backdrop = 0; backdrop < 2; backdrop++) {
            guint white = 0, green = 0;
            if (backdrop)
                gtk_widget_set_state_flags (window, GTK_STATE_FLAG_BACKDROP, FALSE);
            else
                gtk_widget_unset_state_flags (window, GTK_STATE_FLAG_BACKDROP);
            settle ();
            assert_opaque_region (window, alphas[i] == 1.0);
            cairo_surface_t *surface = snapshot (window);
            guint alpha = pixel_at (surface, 0, 0) >> 24;
            g_assert_cmpint (ABS ((gint) alpha - (gint) (alphas[i] * 255 + 0.5)), <=, 1);
            for (gint y = 0; y < 100; y++)
                for (gint x = 0; x < 240; x++) {
                    guint32 pixel = pixel_at (surface, x, y);
                    white += pixel == 0xffffffff;
                    green += pixel == 0xff00ff00;
                }
            g_assert_cmpuint (white, >, 0);
            g_assert_cmpuint (green, ==, 256);
            g_assert_cmpfloat (gtk_widget_get_opacity (window), ==, 1.0);
            cairo_surface_destroy (surface);
        }
    }

    /* A second translucent owner would silently turn .55 into about .80. */
    load_css (provider, "window {background-image:none;background-color:rgba(20,40,60,.55);} "
                       "box {background-color:rgba(20,40,60,.55);}");
    cairo_surface_t *surface = snapshot (window);
    g_assert_cmpint (ABS ((gint) (pixel_at (surface, 0, 0) >> 24) - 203), <=, 1);
    cairo_surface_destroy (surface);

    load_css (provider, "window {background-image:none;background-color:rgba(20,40,60,.55);} "
                       "box {background-color:transparent;} "
                       "label {font:bold 28px monospace;color:white;}");
    surface = snapshot (window);
    guint max_label_alpha = 0;
    for (gint y = 0; y < 100; y++)
        for (gint x = 0; x < 200; x++)
            max_label_alpha = MAX (max_label_alpha, pixel_at (surface, x, y) >> 24);
    g_assert_cmpint (ABS ((gint) max_label_alpha - 221), <=, 1);
    cairo_surface_destroy (surface);

    gtk_style_context_remove_provider_for_screen (gdk_screen_get_default (),
                                                   GTK_STYLE_PROVIDER (provider));
    gtk_style_context_remove_provider_for_screen (gdk_screen_get_default (),
                                                   GTK_STYLE_PROVIDER (fallback));
    gtk_widget_destroy (window);
    g_object_unref (provider);
    g_object_unref (fallback);
    g_object_unref (icon);
    set_compositor_available (FALSE);
}

static void
assert_background (GtkWidget *window, GtkWidget *child, guint expected)
{
    gint x, y;
    g_assert_true (gtk_widget_translate_coordinates (child, window,
        gtk_widget_get_allocated_width (child) / 2,
        gtk_widget_get_allocated_height (child) - 25, &x, &y));
    cairo_surface_t *surface = snapshot (window);
    guint alpha = pixel_at (surface, x, y) >> 24;
    g_test_message ("%s background alpha: %u at %d,%d",
                    G_OBJECT_TYPE_NAME (child), alpha, x, y);
    g_assert_cmpint (ABS ((gint) alpha - (gint) expected), <=, 1);
    cairo_surface_destroy (surface);
}

static void
dump_tree (GtkWidget *widget, gpointer unused)
{
    if (g_getenv ("NEMO_TEST_DUMP_WIDGETS") != NULL) {
        gchar *path = gtk_widget_path_to_string (gtk_widget_get_path (widget));
        g_print ("%s: %s\n", G_OBJECT_TYPE_NAME (widget), path);
        g_free (path);
    }
    if (GTK_IS_CONTAINER (widget))
        gtk_container_foreach (GTK_CONTAINER (widget), dump_tree, NULL);
}

static void
assert_dim_label_ink (GtkWidget *widget, gpointer count)
{
    if (GTK_IS_LABEL (widget) && gtk_widget_is_drawable (widget) &&
        gtk_style_context_has_class (gtk_widget_get_style_context (widget), "dim-label") &&
        *gtk_label_get_text (GTK_LABEL (widget)) != '\0') {
        guint max_alpha = 0;
        cairo_surface_t *surface = snapshot (widget);
        for (gint y = 0; y < gtk_widget_get_allocated_height (widget); y++)
            for (gint x = 0; x < gtk_widget_get_allocated_width (widget); x++)
                max_alpha = MAX (max_alpha, pixel_at (surface, x, y) >> 24);
        g_test_message ("Secondary glyph '%s' maximum alpha: %u",
                        gtk_label_get_text (GTK_LABEL (widget)), max_alpha);
        g_assert_cmpuint (max_alpha, ==, 255);
        (*(guint *) count)++;
        cairo_surface_destroy (surface);
    }
    if (GTK_IS_CONTAINER (widget))
        gtk_container_foreach (GTK_CONTAINER (widget), assert_dim_label_ink, count);
}

static void
test_nemo_hierarchy (void)
{
    GError *error = NULL;
    set_compositor_available (TRUE);
    NemoApplication *app = nemo_main_application_get_singleton ();
    g_assert_true (g_application_register (G_APPLICATION (app), NULL, &error));
    g_assert_no_error (error);
    GtkCssProvider *provider = gtk_css_provider_new ();
    const gchar *external_css = g_getenv ("NEMO_TEST_THEME_CSS");
    const gchar *external_alpha = g_getenv ("NEMO_TEST_THEME_ALPHA");
    guint expected = external_alpha == NULL ? 140 :
        (guint) (g_ascii_strtod (external_alpha, NULL) * 255 + 0.5);
    if (external_css != NULL) {
        g_assert_true (gtk_css_provider_load_from_path (provider, external_css, &error));
        g_assert_no_error (error);
    } else {
        load_css (provider,
            ".nemo-window.smplos-native-alpha, .nemo-quick-preview.smplos-native-alpha "
            "{background-image:none;background-color:rgba(20,40,60,.55);}"
            ".smplos-native-alpha box, .smplos-native-alpha grid, "
            ".smplos-native-alpha paned, .smplos-native-alpha notebook, "
            ".smplos-native-alpha stack, .smplos-native-alpha scrolledwindow, "
            ".smplos-native-alpha decoration, "
            ".smplos-native-alpha viewport, .smplos-native-alpha .view, "
            ".smplos-native-alpha treeview, .smplos-native-alpha .sidebar "
            "{background-image:none;background-color:transparent;}");
    }
    gtk_style_context_add_provider_for_screen (gdk_screen_get_default (),
        GTK_STYLE_PROVIDER (provider), GTK_STYLE_PROVIDER_PRIORITY_USER);

    without_rgba = TRUE;
    NemoWindow *fallback_window = nemo_application_create_window (app, gdk_screen_get_default ());
    GFile *location = g_file_new_for_path (g_getenv ("NEMO_TEST_PROFILE"));
    NemoWindowSlot *fallback_slot = nemo_window_get_active_slot (fallback_window);
    nemo_window_slot_open_location (fallback_slot, location, 0);
    gtk_widget_show (GTK_WIDGET (fallback_window));
    gint64 deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
    do {
        settle ();
    } while ((fallback_slot->content_view == NULL ||
              nemo_view_get_loading (fallback_slot->content_view)) &&
             g_get_monotonic_time () < deadline);
    g_assert_nonnull (fallback_slot->content_view);
    g_assert_false (gtk_style_context_has_class (
        gtk_widget_get_style_context (GTK_WIDGET (fallback_window)), "smplos-native-alpha"));
    cairo_surface_t *fallback_surface = snapshot (GTK_WIDGET (fallback_window));
    g_assert_cmpuint (pixel_at (fallback_surface, 100, 100) >> 24, ==, 255);
    cairo_surface_destroy (fallback_surface);
    without_rgba = FALSE;
    set_compositor_available (FALSE);

    NemoWindow *window = nemo_application_create_window (app, gdk_screen_get_default ());
    GtkWidget *widget = GTK_WIDGET (window);
    gtk_window_set_default_size (GTK_WINDOW (window), 1000, 700);
    NemoWindowSlot *slot = nemo_window_get_active_slot (window);
    nemo_window_slot_open_location (slot, location, 0);
    gtk_widget_show (widget);
    deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
    do {
        settle ();
    } while ((slot->content_view == NULL || nemo_view_get_loading (slot->content_view)) &&
             g_get_monotonic_time () < deadline);
    g_assert_nonnull (slot->content_view);
    g_assert_false (nemo_view_get_loading (slot->content_view));
    gtk_widget_destroy (GTK_WIDGET (fallback_window));
    g_assert_false (gtk_style_context_has_class (gtk_widget_get_style_context (widget),
                                                "smplos-native-alpha"));
    assert_background (widget, GTK_WIDGET (slot->content_view), 255);
    set_compositor_available (TRUE);
    g_assert_true (gtk_style_context_has_class (gtk_widget_get_style_context (widget),
                                               "smplos-native-alpha"));
    g_assert_true (gtk_widget_get_visual (widget) ==
                   gdk_screen_get_rgba_visual (gtk_widget_get_screen (widget)));

    const gchar *views[] = { "OAFIID:Nemo_File_Manager_Icon_View",
                            "OAFIID:Nemo_File_Manager_List_View",
                            "OAFIID:Nemo_File_Manager_Compact_View" };
    for (guint i = 0; i < G_N_ELEMENTS (views); i++) {
        nemo_window_slot_set_content_view (slot, views[i]);
        deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
        do {
            settle ();
        } while ((slot->new_content_view != NULL || nemo_view_get_loading (slot->content_view)) &&
                 g_get_monotonic_time () < deadline);
        g_assert_cmpstr (nemo_window_slot_get_content_view_id (slot), ==, views[i]);
        for (guint backdrop = 0; backdrop < 2; backdrop++) {
            if (backdrop)
                gtk_widget_set_state_flags (widget, GTK_STATE_FLAG_BACKDROP, FALSE);
            else
                gtk_widget_unset_state_flags (widget, GTK_STATE_FLAG_BACKDROP);
            settle ();
            assert_background (widget, GTK_WIDGET (slot->content_view), expected);
        }
    }
    nemo_window_preview_pane_on (window);
    settle ();
    assert_background (widget, window->details->preview_pane, expected);
    assert_background (widget, window->details->sidebar, expected);
    if (external_css != NULL) {
        guint labels = 0;
        gchar *image_path = g_build_filename (g_getenv ("NEMO_TEST_PROFILE"),
                                              "native-alpha.png", NULL);
        GdkPixbuf *image = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 16, 16);
        gdk_pixbuf_fill (image, 0x00ff00ff);
        g_assert_true (gdk_pixbuf_save (image, image_path, "png", &error, NULL));
        g_assert_no_error (error);
        g_object_unref (image);
        GFile *image_location = g_file_new_for_path (image_path);
        NemoFile *image_file = nemo_file_get (image_location);
        nemo_preview_pane_set_file (NEMO_PREVIEW_PANE (window->details->preview_pane),
                                   image_file);
        /* Current previews create metadata labels asynchronously, only after
         * a real file is selected; the empty pane exercises just two labels. */
        deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
        do {
            settle ();
            labels = 0;
            assert_dim_label_ink (window->details->preview_pane, &labels);
        } while (labels < 7 && g_get_monotonic_time () < deadline);
        g_assert_cmpuint (labels, >=, 7);
        nemo_preview_pane_clear (NEMO_PREVIEW_PANE (window->details->preview_pane));
        nemo_file_unref (image_file);
        g_object_unref (image_location);
        g_assert_cmpint (g_remove (image_path), ==, 0);
        g_free (image_path);
        settle ();
    }
    dump_tree (widget, NULL);

    NemoQuickPreview *preview = nemo_quick_preview_get_instance ();
    gtk_widget_show (GTK_WIDGET (preview));
    settle ();
    g_assert_true (gtk_style_context_has_class (gtk_widget_get_style_context (GTK_WIDGET (preview)),
                                               "smplos-native-alpha"));
    g_assert_true (gtk_widget_get_visual (GTK_WIDGET (preview)) ==
                   gdk_screen_get_rgba_visual (gtk_widget_get_screen (GTK_WIDGET (preview))));
    dump_tree (GTK_WIDGET (preview), NULL);
    assert_background (GTK_WIDGET (preview),
                       gtk_bin_get_child (GTK_BIN (preview)), expected);
    GdkVisual *main_visual = gtk_widget_get_visual (widget);
    GdkVisual *preview_visual = gtk_widget_get_visual (GTK_WIDGET (preview));
    set_compositor_available (FALSE);
    g_assert_false (gtk_style_context_has_class (gtk_widget_get_style_context (widget),
                                                "smplos-native-alpha"));
    g_assert_false (gtk_style_context_has_class (gtk_widget_get_style_context (GTK_WIDGET (preview)),
                                                "smplos-native-alpha"));
    assert_background (widget, GTK_WIDGET (slot->content_view), 255);
    assert_background (GTK_WIDGET (preview),
                       gtk_bin_get_child (GTK_BIN (preview)), 255);
    set_compositor_available (TRUE);
    g_assert_true (gtk_widget_get_visual (widget) == main_visual);
    g_assert_true (gtk_widget_get_visual (GTK_WIDGET (preview)) == preview_visual);
    assert_background (widget, GTK_WIDGET (slot->content_view), expected);
    assert_background (GTK_WIDGET (preview),
                       gtk_bin_get_child (GTK_BIN (preview)), expected);
    gtk_widget_destroy (GTK_WIDGET (preview));
    g_object_unref (preview);
    g_object_unref (location);
    gtk_style_context_remove_provider_for_screen (gdk_screen_get_default (),
                                                   GTK_STYLE_PROVIDER (provider));
    settle ();
    assert_background (widget, GTK_WIDGET (slot->content_view), 255);
    assert_background (widget, window->details->sidebar, 255);
    gtk_widget_destroy (widget);
    g_object_unref (provider);
    set_compositor_available (FALSE);
}

int
main (int argc, char **argv)
{
    g_assert_cmpstr (g_getenv ("NEMO_TEST_ISOLATED"), ==, "1");
    gtk_test_init (&argc, &argv, NULL);
    /* Installed GTK themes/extensions may warn; criticals remain fatal. */
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    g_test_add_func ("/native-alpha/pixels", test_pixels);
    g_test_add_func ("/native-alpha/nemo-hierarchy", test_nemo_hierarchy);
    return g_test_run ();
}
