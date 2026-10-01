#ifndef NEMO_STYLE_UTILITIES_H
#define NEMO_STYLE_UTILITIES_H

#include <gtk/gtk.h>

static inline void
nemo_label_set_secondary (GtkWidget *label, gint percent)
{
#ifdef NEMO_SMPL
    GtkStyleContext *context = gtk_widget_get_style_context (label);
    gchar *class_name = g_strdup_printf ("nemo-dim-%d", percent);
    gtk_style_context_add_class (context, "nemo-secondary-label");
    gtk_style_context_add_class (context, class_name);
    g_free (class_name);
#else
    gtk_widget_set_opacity (label, percent / 100.0);
#endif
}

#endif
