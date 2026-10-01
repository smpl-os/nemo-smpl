#ifndef NEMO_WINDOW_ALPHA_H
#define NEMO_WINDOW_ALPHA_H

#include <gtk/gtk.h>

static void
nemo_window_update_native_alpha (GdkScreen *screen, GtkWidget *widget)
{
    GtkStyleContext *context = gtk_widget_get_style_context (widget);
    GdkVisual *visual = gdk_screen_get_rgba_visual (screen);

    if (visual != NULL && gtk_widget_get_visual (widget) == visual &&
        gdk_screen_is_composited (screen)) {
        gtk_style_context_add_class (context, "smplos-native-alpha");
    } else {
        gtk_style_context_remove_class (context, "smplos-native-alpha");
    }
}

/* Keep the RGBA visual even without a compositor, so a later compositor can
 * enable alpha without recreating the native window. CSS owns all painting. */
static inline void
nemo_window_enable_native_alpha (GtkWidget *widget)
{
    GdkVisual *visual;
    GdkScreen *screen = gtk_widget_get_screen (widget);
    GdkScreen *previous_screen = g_object_get_data (G_OBJECT (widget),
                                                   "nemo-alpha-screen");

    g_return_if_fail (!gtk_widget_get_realized (widget));

    if (previous_screen != NULL)
        g_signal_handlers_disconnect_by_func (previous_screen,
            nemo_window_update_native_alpha, widget);
    g_object_set_data_full (G_OBJECT (widget), "nemo-alpha-screen",
                           g_object_ref (screen), g_object_unref);
    g_signal_connect_object (screen, "composited-changed",
                             G_CALLBACK (nemo_window_update_native_alpha),
                             widget, 0);

    visual = gdk_screen_get_rgba_visual (screen);
    if (visual != NULL)
        gtk_widget_set_visual (widget, visual);
    nemo_window_update_native_alpha (screen, widget);
}

#endif
