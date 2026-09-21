/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * nemo-progress-ui-handler.c: file operation progress user interface.
 *
 * Copyright (C) 2007, 2011 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street - Suite 500,
 * Boston, MA 02110-1335, USA.
 *
 * Authors: Alexander Larsson <alexl@redhat.com>
 *          Cosimo Cecchi <cosimoc@redhat.com>
 *
 */

#include <config.h>

#include "nemo-progress-ui-handler.h"

#include "nemo-application.h"
#include "nemo-progress-info-widget.h"

#include <gio/gio.h>
#include <glib/gi18n.h>

#include <eel/eel-string.h>

#include <libnemo-private/nemo-progress-info.h>
#include <libnemo-private/nemo-progress-info-manager.h>

#include <libxapp/xapp-gtk-window.h>
#include <libxapp/xapp-status-icon.h>

struct _NemoProgressUIHandlerPriv {
	NemoProgressInfoManager *manager;

	GtkWidget *progress_window;
	GtkWidget *window_vbox;

    GtkWidget *list;

	guint active_infos;
    guint active_percent;
    gboolean active_indeterminate;
	GList *infos;

	XAppStatusIcon *status_icon;
    gboolean should_show_status_icon;
#ifdef NEMO_SMPL
    GHashTable *operations;
    GtkWidget *completed_list;
    GtkWidget *completed_scroll;
    GtkWidget *history_notice;
    GSimpleAction *show_action;
    guint completed_count;
    gboolean window_held;
    gboolean shutting_down;
#endif
};

G_DEFINE_TYPE (NemoProgressUIHandler, nemo_progress_ui_handler, G_TYPE_OBJECT);

#ifdef NEMO_SMPL
#define COMPLETION_HISTORY_LIMIT 50
#define COMPLETION_TEXT_LIMIT (64 * 1024)

static void progress_ui_handler_clear_completed (NemoProgressUIHandler *self);
static void progress_ui_handler_show_results (GSimpleAction *action,
                                             GVariant *parameter,
                                             NemoProgressUIHandler *self);
#endif

static void
status_icon_activate_cb (XAppStatusIcon        *icon,
                         guint                  button,
                         guint                  _time,
                         NemoProgressUIHandler *self)
{
#ifdef NEMO_SMPL
    if (self->priv->shutting_down)
        return;
#endif
    self->priv->should_show_status_icon = FALSE;
    xapp_status_icon_set_visible (icon, FALSE);
    gtk_window_present (GTK_WINDOW (self->priv->progress_window));
}

static void
progress_ui_handler_ensure_status_icon (NemoProgressUIHandler *self)
{
	XAppStatusIcon *status_icon;

	if (self->priv->status_icon != NULL) {
		return;
	}

    status_icon = xapp_status_icon_new ();
    xapp_status_icon_set_icon_name (status_icon, "nemo-progress-0-symbolic");
    g_signal_connect (status_icon, "activate",
                      (GCallback) status_icon_activate_cb,
                      self);

	xapp_status_icon_set_visible (status_icon, FALSE);

	self->priv->status_icon = status_icon;
}

static gchar *
get_icon_name_from_percent (guint pct)
{
    gchar *icon_name;
    guint rounded = 0;
    gint ones = pct % 10;

    if (ones < 5)
        rounded = pct - ones;
    else
        rounded = pct + (10 - ones);

#ifdef NEMO_SMPL
    if (pct < 100)
        rounded = MIN (rounded, 90);
#endif
    icon_name = g_strdup_printf ("nemo-progress-%d-symbolic", rounded);

    return icon_name;
}

static void
progress_ui_handler_update_status_icon (NemoProgressUIHandler *self)
{
	gchar *tooltip;

#ifdef NEMO_SMPL
    if (!self->priv->should_show_status_icon ||
        (self->priv->active_infos == 0 && self->priv->completed_count == 0)) {
        if (self->priv->status_icon != NULL)
            xapp_status_icon_set_visible (self->priv->status_icon, FALSE);
        return;
    }
#endif
	progress_ui_handler_ensure_status_icon (self);
#ifdef NEMO_SMPL
    if (self->priv->active_infos == 0) {
        xapp_status_icon_set_tooltip_text (self->priv->status_icon,
                                          _("Completed file operations"));
        xapp_status_icon_set_icon_name (self->priv->status_icon, "system-run");
        xapp_status_icon_set_visible (self->priv->status_icon,
                                     self->priv->should_show_status_icon &&
                                     self->priv->completed_count > 0);
        return;
    }
#endif
    gchar *launchpad_sucks = THOU_TO_STR (self->priv->active_infos);
    if (self->priv->active_indeterminate) {
        tooltip = g_strdup_printf (ngettext ("%s file operation active. Waiting for completion.",
                                             "%s file operations active. Waiting for completion.",
                                             self->priv->active_infos), launchpad_sucks);
    } else {
        tooltip = g_strdup_printf (ngettext ("%1$s file operation active.  %2$d%% complete.",
                                            "%1$s file operations active.  %2$d%% complete.",
                                            self->priv->active_infos),
                                    launchpad_sucks, self->priv->active_percent);
    }
	xapp_status_icon_set_tooltip_text (self->priv->status_icon, tooltip);
    gchar *name = get_icon_name_from_percent (self->priv->active_percent);
    xapp_status_icon_set_icon_name (self->priv->status_icon, name);
    g_free (name);
	g_free (tooltip);
    g_free (launchpad_sucks);

	xapp_status_icon_set_visible (self->priv->status_icon, self->priv->should_show_status_icon);
}

static gboolean
progress_window_delete_event (GtkWidget *widget,
			      GdkEvent *event,
			      NemoProgressUIHandler *self)
{
#ifdef NEMO_SMPL
    progress_ui_handler_clear_completed (self);
#endif
    gtk_widget_hide (widget);

    self->priv->should_show_status_icon = TRUE;
    progress_ui_handler_update_status_icon (self);

    return TRUE;
}

#ifdef NEMO_SMPL
static void
progress_window_close_clicked (GtkButton *button,
                               NemoProgressUIHandler *self)
{
    progress_window_delete_event (self->priv->progress_window, NULL, self);
}

static void
progress_window_shown (GtkWidget *widget, NemoProgressUIHandler *self)
{
    if (self->priv->shutting_down) {
        gtk_widget_hide (widget);
        return;
    }
    if (!self->priv->window_held) {
        g_application_hold (G_APPLICATION (nemo_application_get_singleton ()));
        self->priv->window_held = TRUE;
    }
}

static void
progress_window_hidden (GtkWidget *widget, NemoProgressUIHandler *self)
{
    if (self->priv->window_held) {
        self->priv->window_held = FALSE;
        g_application_release (G_APPLICATION (nemo_application_get_singleton ()));
    }
}
#endif

static void
ensure_first_separator_hidden (NemoProgressUIHandler *self)
{
    GList *l = gtk_container_get_children (GTK_CONTAINER (self->priv->list));

    if (l == NULL)
        return;

    NemoProgressInfoWidgetPriv *priv = NEMO_PROGRESS_INFO_WIDGET (l->data)->priv;

    gtk_widget_hide (GTK_WIDGET (priv->separator));

    g_list_free (l);
}

static void
progress_ui_handler_sort_by_active (NemoProgressUIHandler *self)
{
    gint first_pending = -1;
    gint current_index = 0;
    GList *iter;
    GList *l = gtk_container_get_children (GTK_CONTAINER (self->priv->list));

    if (l == NULL)
        return;

    for (iter = l; iter != NULL; iter = iter->next) {
        NemoProgressInfoWidgetPriv *priv = NEMO_PROGRESS_INFO_WIDGET (iter->data)->priv;

        if (nemo_progress_info_get_is_started (priv->info)) {
            if (first_pending > 0) {
                gtk_box_reorder_child (GTK_BOX (self->priv->list), GTK_WIDGET (iter->data), first_pending);
                break;
            }
        } else {
            if (first_pending == -1) {
                first_pending = current_index;
            }
        }

        current_index++;
    }

    g_list_free (l);
}

static void
progress_ui_handler_ensure_window (NemoProgressUIHandler *self)
{
    NemoProgressUIHandlerPriv *priv = NEMO_PROGRESS_UI_HANDLER (self)->priv;

    GtkWidget *main_box, *progress_window;
    GtkWidget *w, *frame;

	if (self->priv->progress_window != NULL) {
		return;
	}
	
	progress_window = xapp_gtk_window_new (GTK_WINDOW_TOPLEVEL);
	self->priv->progress_window = progress_window;

    gtk_window_set_type_hint (GTK_WINDOW (progress_window), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_resizable (GTK_WINDOW (progress_window), FALSE);
    gtk_window_set_default_size (GTK_WINDOW (progress_window), 500, -1);

	gtk_window_set_title (GTK_WINDOW (progress_window),
			      _("File Operations"));
	gtk_window_set_wmclass (GTK_WINDOW (progress_window),
				"file_progress", "Nemo");
	gtk_window_set_position (GTK_WINDOW (progress_window),
				 GTK_WIN_POS_CENTER);
	xapp_gtk_window_set_icon_name (XAPP_GTK_WINDOW (progress_window),
                                   "system-run");

	main_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
	gtk_container_add (GTK_CONTAINER (progress_window),
                       main_box);
	self->priv->window_vbox = main_box;

    frame = gtk_frame_new (NULL);
    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_NONE);

    w = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    priv->list = w;
    gtk_container_add (GTK_CONTAINER (frame), w);

    g_object_set (priv->list,
                  "margin-left", 5,
                  "margin-right", 5,
                  "margin-top", 5,
                  "margin-bottom", 5,
                  NULL);

    gtk_box_pack_start (GTK_BOX (main_box), frame, FALSE, FALSE, 0);
#ifdef NEMO_SMPL
    priv->completed_scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (priv->completed_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request (priv->completed_scroll, -1, 220);
    priv->completed_list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width (GTK_CONTAINER (priv->completed_list), 12);
    gtk_container_add (GTK_CONTAINER (priv->completed_scroll), priv->completed_list);
    gtk_box_pack_start (GTK_BOX (main_box), priv->completed_scroll, TRUE, TRUE, 0);
    priv->history_notice = gtk_label_new (_("Only the latest 50 completed operations are shown."));
    gtk_box_pack_start (GTK_BOX (main_box), priv->history_notice, FALSE, FALSE, 4);
    w = gtk_button_new_with_mnemonic (_("_Close"));
    gtk_widget_set_halign (w, GTK_ALIGN_END);
    gtk_widget_set_margin_end (w, 12);
    gtk_widget_set_margin_bottom (w, 12);
    gtk_box_pack_start (GTK_BOX (main_box), w, FALSE, FALSE, 0);
    g_signal_connect (w, "clicked", G_CALLBACK (progress_window_close_clicked), self);
    g_signal_connect (progress_window, "show", G_CALLBACK (progress_window_shown), self);
    g_signal_connect (progress_window, "hide", G_CALLBACK (progress_window_hidden), self);
#endif
    gtk_widget_show_all (main_box);
#ifdef NEMO_SMPL
    gtk_widget_hide (priv->completed_scroll);
    gtk_widget_hide (priv->history_notice);
#endif

	g_signal_connect (progress_window,
			  "delete-event",
			  (GCallback) progress_window_delete_event, self);
}

static void
progress_ui_handler_add_to_window (NemoProgressUIHandler *self,
				   NemoProgressInfo *info)
{
	GtkWidget *progress;

	progress = nemo_progress_info_widget_new (info);

	progress_ui_handler_ensure_window (self);

    gtk_box_pack_start (GTK_BOX (self->priv->list), progress, FALSE, FALSE, 0);
    gtk_widget_show (progress);

    ensure_first_separator_hidden (self);
    progress_ui_handler_sort_by_active (self);
}

#ifndef NEMO_SMPL
static void
progress_ui_handler_show_complete_notification (NemoProgressUIHandler *self)
{
	GNotification *complete_notification;

	complete_notification = g_notification_new (_("File Operations"));
	g_notification_set_body (complete_notification, _("All file operations have been successfully completed"));
	
	g_application_send_notification (G_APPLICATION (nemo_application_get_singleton ()), NULL, complete_notification);
	g_object_unref (complete_notification);
}
#endif

static void
progress_ui_handler_hide_status (NemoProgressUIHandler *self)
{
	if (self->priv->status_icon != NULL) {
        self->priv->should_show_status_icon = FALSE;
		xapp_status_icon_set_visible (self->priv->status_icon, FALSE);
	}
}

#ifndef NEMO_SMPL
static void
progress_info_finished_cb (NemoProgressInfo *info,
			   NemoProgressUIHandler *self)
{
	self->priv->active_infos--;
	self->priv->infos = g_list_remove (self->priv->infos, info);

	if (self->priv->active_infos > 0) {
		if (!gtk_widget_get_visible (self->priv->progress_window)) {
			progress_ui_handler_update_status_icon (self);
		}
        ensure_first_separator_hidden (self);
	} else {
		if (gtk_widget_get_visible (self->priv->progress_window)) {
			gtk_widget_hide (self->priv->progress_window);
		} else {
			progress_ui_handler_hide_status (self);
			progress_ui_handler_show_complete_notification (self);
		}
	}
}
#endif

static void
progress_info_changed_cb (NemoProgressInfo *info,
			   NemoProgressUIHandler *self)
{	
#ifdef NEMO_SMPL
    if (self->priv->progress_window == NULL)
        return;
#endif
	if (g_list_length(self->priv->infos) > 0) {
        NemoProgressInfo *first_info = (NemoProgressInfo *) g_list_first(self->priv->infos)->data;
        GList *l;
        g_autofree gchar *status = nemo_progress_info_get_status (first_info);
#ifdef NEMO_SMPL
        g_autofree gchar *details = nemo_progress_info_get_details (first_info);
        g_autofree gchar *phase = g_strdup_printf ("%s - %s", status, details);
        const gchar *title = phase;
#else
        const gchar *title = status;
#endif
        double progress = 0.0;
        int i = 0;
        gboolean indeterminate = FALSE;
        for (l = self->priv->infos; l != NULL; l = l->next) {
            if (nemo_progress_info_get_is_finished (l->data)) {
                continue;
            }
            double current = nemo_progress_info_get_progress (l->data);
            indeterminate |= current < 0;
            progress += MAX (current, 0);
            i++;
        }
        progress = i > 0 ? progress / i : 0;
        self->priv->active_indeterminate = indeterminate;
        self->priv->active_percent = 0;
        if (!indeterminate && progress > 0) {
            int iprogress = progress * 100;
            gchar *str = g_strdup_printf (_("%d%% %s"), iprogress, title);
            gtk_window_set_title (GTK_WINDOW (self->priv->progress_window), str);
            xapp_gtk_window_set_progress (XAPP_GTK_WINDOW (self->priv->progress_window), iprogress);
            g_free (str);
            self->priv->active_percent = iprogress;
        }
        else {
            gtk_window_set_title (GTK_WINDOW (self->priv->progress_window), title);
            xapp_gtk_window_set_progress (XAPP_GTK_WINDOW (self->priv->progress_window), 0);
        }
        if (self->priv->should_show_status_icon)
            progress_ui_handler_update_status_icon (self);
    } 
}

static void
progress_info_started_cb (NemoProgressUIHandler *self)
{
#ifdef NEMO_SMPL
    if (self->priv->list == NULL)
        return;
#endif
    progress_ui_handler_sort_by_active (self);
    ensure_first_separator_hidden (self);
}

#ifndef NEMO_SMPL
static void
handle_new_progress_info (NemoProgressUIHandler *self,
			  NemoProgressInfo *info)
{
	self->priv->infos = g_list_append (self->priv->infos, info);	
	
	g_signal_connect_after (info, "finished",
			  G_CALLBACK (progress_info_finished_cb), self);

    g_signal_connect_swapped (info, "started",
              G_CALLBACK (progress_info_started_cb), self);
			  
	g_signal_connect (info, "progress-changed",
			  G_CALLBACK (progress_info_changed_cb), self);

	self->priv->active_infos++;

	if (self->priv->active_infos == 1) {
		/* this is the only active operation, present the window */
		progress_ui_handler_add_to_window (self, info);
        gtk_window_present (GTK_WINDOW (self->priv->progress_window));
        gchar *details = nemo_progress_info_get_details (info);
		gtk_window_set_title (GTK_WINDOW (self->priv->progress_window), details);
        g_free (details);
        xapp_gtk_window_set_icon_name (XAPP_GTK_WINDOW (self->priv->progress_window), "system-run");
	} else {
		progress_ui_handler_add_to_window (self, info);
        if (self->priv->should_show_status_icon) {
            progress_ui_handler_update_status_icon (self);
        }
        if (gtk_widget_get_visible (GTK_WIDGET (self->priv->progress_window))) {
            gtk_window_present (GTK_WINDOW (self->priv->progress_window));
        }
	}
}

typedef struct {
	NemoProgressInfo *info;
	NemoProgressUIHandler *self;
} TimeoutData;

static void
timeout_data_free (TimeoutData *data)
{
	g_clear_object (&data->self);
	g_clear_object (&data->info);

	g_free (data);
}

static TimeoutData *
timeout_data_new (NemoProgressUIHandler *self,
		  NemoProgressInfo *info)
{
	TimeoutData *retval;

	retval = g_new0 (TimeoutData, 1);
	retval->self = g_object_ref (self);
	retval->info = g_object_ref (info);

	return retval;
}

static gboolean
new_op_queued_timeout (TimeoutData *data)
{
	NemoProgressInfo *info = data->info;
	NemoProgressUIHandler *self = data->self;

	if (nemo_progress_info_get_is_paused (info)) {
		return TRUE;
	}

	if (!nemo_progress_info_get_is_finished (info)) {
		handle_new_progress_info (self, info);
	}

	timeout_data_free (data);

	return FALSE;
}

static void
release_application (NemoProgressInfo *info,
		     NemoProgressUIHandler *self)
{
	NemoApplication *app;

	/* release the GApplication hold we acquired */
	app = nemo_application_get_singleton ();
	g_application_release (G_APPLICATION (app));
}

static void
progress_info_queued_cb (NemoProgressInfo *info,
			  NemoProgressUIHandler *self)
{
	NemoApplication *app;
	TimeoutData *data;

	/* hold GApplication so we never quit while there's an operation pending */
	app = nemo_application_get_singleton ();
	g_application_hold (G_APPLICATION (app));

	g_signal_connect (info, "finished",
			  G_CALLBACK (release_application), self);

	data = timeout_data_new (self, info);

	/* timeout for the progress window to appear */
	g_timeout_add_seconds (2,
			       (GSourceFunc) new_op_queued_timeout,
			       data);
}
#else
typedef struct {
    NemoProgressUIHandler *self;
    NemoProgressInfo *info;
    guint timeout_id;
} OperationWatch;

static void
operation_watch_free (gpointer data)
{
    OperationWatch *watch = data;

    if (watch->timeout_id != 0)
        g_source_remove (watch->timeout_id);
    g_signal_handlers_disconnect_by_data (watch->info, watch);
    g_signal_handlers_disconnect_by_data (watch->info, watch->self);
    g_object_unref (watch->info);
    g_application_release (G_APPLICATION (nemo_application_get_singleton ()));
    g_free (watch);
}

static void
progress_ui_handler_dismiss_result (GtkWidget *label)
{
    const char *id = g_object_get_data (G_OBJECT (label), "notification-id");

    if (id != NULL)
        g_application_withdraw_notification (G_APPLICATION (nemo_application_get_singleton ()), id);
    gtk_widget_destroy (label);
}

static void
progress_ui_handler_clear_completed (NemoProgressUIHandler *self)
{
    GList *children = gtk_container_get_children (GTK_CONTAINER (self->priv->completed_list));

    g_list_free_full (children, (GDestroyNotify) progress_ui_handler_dismiss_result);
    self->priv->completed_count = 0;
    gtk_widget_hide (self->priv->completed_scroll);
    gtk_widget_hide (self->priv->history_notice);
}

static GtkWidget *
progress_ui_handler_add_completed (NemoProgressUIHandler *self,
                                   const char *text)
{
    GtkWidget *label;

    progress_ui_handler_ensure_window (self);
    if (self->priv->completed_count == COMPLETION_HISTORY_LIMIT) {
        GList *children = gtk_container_get_children (GTK_CONTAINER (self->priv->completed_list));
        progress_ui_handler_dismiss_result (GTK_WIDGET (children->data));
        g_list_free (children);
        self->priv->completed_count--;
        gtk_widget_show (self->priv->history_notice);
    }
    label = gtk_label_new (text);
    g_object_set_data_full (G_OBJECT (label), "result-text", g_strdup (text), g_free);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
    gtk_label_set_line_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 65);
    gtk_label_set_selectable (GTK_LABEL (label), TRUE);
    gtk_box_pack_start (GTK_BOX (self->priv->completed_list), label, FALSE, FALSE, 0);
    gtk_widget_show (label);
    gtk_widget_show (self->priv->completed_scroll);
    self->priv->completed_count++;
    return label;
}

static void
progress_ui_handler_show_results (GSimpleAction *action,
                                  GVariant *parameter,
                                  NemoProgressUIHandler *self)
{
    const char *text = g_variant_get_string (parameter, NULL);
    gboolean found = FALSE;

    if (self->priv->shutting_down)
        return;
    if (strlen (text) > COMPLETION_TEXT_LIMIT || !g_utf8_validate (text, -1, NULL)) {
        g_warning ("Ignoring an invalid file-operation notification result");
        return;
    }

    /* The notification carries its result across application restarts. */
    if (self->priv->completed_list != NULL) {
        GList *children = gtk_container_get_children (GTK_CONTAINER (self->priv->completed_list));
        for (GList *l = children; l != NULL; l = l->next) {
            if (g_strcmp0 (text, g_object_get_data (G_OBJECT (l->data), "result-text")) == 0) {
                found = TRUE;
                break;
            }
        }
        g_list_free (children);
    }
    if (!found) {
        GtkWidget *label = progress_ui_handler_add_completed (self, text);
        g_autofree char *restored = g_strdup_printf (_("Earlier completion notification\n%s"), text);
        gtk_label_set_text (GTK_LABEL (label), restored);
    }
    self->priv->should_show_status_icon = FALSE;
    progress_ui_handler_hide_status (self);
    gtk_window_present (GTK_WINDOW (self->priv->progress_window));
}

static gboolean
progress_window_is_visible (NemoProgressUIHandler *self)
{
    GdkWindow *window;

    if (self->priv->progress_window == NULL ||
        !gtk_widget_get_visible (self->priv->progress_window))
        return FALSE;
    window = gtk_widget_get_window (self->priv->progress_window);
    return window == NULL || !(gdk_window_get_state (window) & GDK_WINDOW_STATE_ICONIFIED);
}

static void
operation_finished (NemoProgressInfo *info, OperationWatch *watch)
{
    NemoProgressUIHandler *self = watch->self;
    g_autofree char *text = nemo_progress_info_get_completion_text (info);
    gboolean visible = progress_window_is_visible (self);
    GtkWidget *label;

    self->priv->active_infos--;
    self->priv->infos = g_list_remove (self->priv->infos, info);
    label = progress_ui_handler_add_completed (self, text);
    ensure_first_separator_hidden (self);
    if (self->priv->active_infos == 0) {
        self->priv->active_percent = 0;
        self->priv->active_indeterminate = FALSE;
        gtk_window_set_title (GTK_WINDOW (self->priv->progress_window), _("File Operations"));
        xapp_gtk_window_set_progress (XAPP_GTK_WINDOW (self->priv->progress_window), 0);
    } else {
        progress_info_changed_cb (NULL, self);
    }
    if (!visible) {
        GNotification *notification = g_notification_new (_("File operation finished"));
        GIcon *icon = g_themed_icon_new ("system-file-manager");
        char *id = g_strdup_printf ("file-operation-%" G_GINT64_FORMAT,
                                    g_get_monotonic_time ());

        g_object_set_data_full (G_OBJECT (label), "notification-id", id, g_free);
        g_notification_set_body (notification, text);
        g_notification_set_icon (notification, icon);
        g_notification_set_default_action_and_target (notification,
                                                      "app.show-file-operation-results",
                                                      "s", text);
        g_application_send_notification (G_APPLICATION (nemo_application_get_singleton ()),
                                         id, notification);
        g_object_unref (icon);
        g_object_unref (notification);
        self->priv->should_show_status_icon = TRUE;
    }
    progress_ui_handler_update_status_icon (self);

    /* Release the job hold only after a visible summary has acquired its hold. */
    g_hash_table_remove (self->priv->operations, info);
}

static gboolean
operation_show_timeout (gpointer data)
{
    OperationWatch *watch = data;
    NemoProgressUIHandler *self = watch->self;
    gboolean first_window = self->priv->progress_window == NULL;

    if (nemo_progress_info_get_is_paused (watch->info))
        return G_SOURCE_CONTINUE;
    watch->timeout_id = 0;
    if (!nemo_progress_info_get_is_finished (watch->info)) {
        progress_ui_handler_add_to_window (self, watch->info);
        progress_info_changed_cb (watch->info, self);
        if (first_window || !self->priv->should_show_status_icon)
            gtk_window_present (GTK_WINDOW (self->priv->progress_window));
        progress_ui_handler_update_status_icon (self);
    }
    return G_SOURCE_REMOVE;
}

static void
progress_info_queued_cb (NemoProgressInfo *info,
                         NemoProgressUIHandler *self)
{
    OperationWatch *watch;
    NemoProgressResult result;
    gboolean immediate, new_batch;

    if (self->priv->shutting_down)
        return;
    if (g_hash_table_contains (self->priv->operations, info))
        return;
    immediate = nemo_progress_info_get_result (info, &result) &&
                (result.operation == NEMO_PROGRESS_OPERATION_COPY ||
                 result.operation == NEMO_PROGRESS_OPERATION_MOVE);
    new_batch = self->priv->active_infos == 0;
    if (new_batch)
        self->priv->should_show_status_icon = FALSE;
    watch = g_new0 (OperationWatch, 1);
    watch->self = self;
    watch->info = g_object_ref (info);
    g_application_hold (G_APPLICATION (nemo_application_get_singleton ()));
    g_hash_table_insert (self->priv->operations, info, watch);
    self->priv->infos = g_list_append (self->priv->infos, info);
    self->priv->active_infos++;
    g_signal_connect_after (info, "finished", G_CALLBACK (operation_finished), watch);
    g_signal_connect_swapped (info, "started", G_CALLBACK (progress_info_started_cb), self);
    g_signal_connect (info, "progress-changed", G_CALLBACK (progress_info_changed_cb), self);
    g_signal_connect (info, "changed", G_CALLBACK (progress_info_changed_cb), self);
    if (immediate) {
        /* Fast jobs can already be finished when their queued signal arrives.
         * Show their summary too, without relying on a tray or notifications. */
        if (!nemo_progress_info_get_is_finished (info))
            progress_ui_handler_add_to_window (self, info);
        else
            progress_ui_handler_ensure_window (self);
        progress_info_changed_cb (info, self);
        if (new_batch || (!gtk_widget_get_visible (self->priv->progress_window) &&
                          !self->priv->should_show_status_icon))
            gtk_window_present (GTK_WINDOW (self->priv->progress_window));
        progress_ui_handler_update_status_icon (self);
    } else {
        progress_info_changed_cb (info, self);
        watch->timeout_id = g_timeout_add_seconds (2, operation_show_timeout, watch);
    }
}
#endif

static void
new_progress_info_cb (NemoProgressInfoManager *manager,
		      NemoProgressInfo *info,
		      NemoProgressUIHandler *self)
{
    g_signal_connect_object (info, "queued",
                             G_CALLBACK (progress_info_queued_cb), self, 0);
}

#ifdef NEMO_SMPL
void
nemo_progress_ui_handler_shutdown (NemoProgressUIHandler *self)
{
    if (self->priv->shutting_down)
        return;
    self->priv->shutting_down = TRUE;
    if (self->priv->manager != NULL) {
        g_signal_handlers_disconnect_by_data (self->priv->manager, self);
        GList *infos = nemo_progress_info_manager_get_all_infos (self->priv->manager);
        for (GList *l = infos; l != NULL; l = l->next)
            g_signal_handlers_disconnect_by_data (l->data, self);
    }
    if (self->priv->show_action != NULL) {
        g_simple_action_set_enabled (self->priv->show_action, FALSE);
        g_action_map_remove_action (G_ACTION_MAP (nemo_application_get_singleton ()),
                                    "show-file-operation-results");
        g_clear_object (&self->priv->show_action);
    }
    g_clear_pointer (&self->priv->operations, g_hash_table_unref);
    g_clear_pointer (&self->priv->infos, g_list_free);
    self->priv->active_infos = 0;
    if (self->priv->progress_window != NULL)
        gtk_widget_hide (self->priv->progress_window);
    if (self->priv->status_icon != NULL) {
        g_signal_handlers_disconnect_by_data (self->priv->status_icon, self);
        xapp_status_icon_set_visible (self->priv->status_icon, FALSE);
    }
}
#endif

static void
nemo_progress_ui_handler_dispose (GObject *obj)
{
	NemoProgressUIHandler *self = NEMO_PROGRESS_UI_HANDLER (obj);

#ifdef NEMO_SMPL
    nemo_progress_ui_handler_shutdown (self);
    if (self->priv->progress_window != NULL) {
        progress_window_hidden (self->priv->progress_window, self);
        gtk_widget_destroy (self->priv->progress_window);
        self->priv->progress_window = NULL;
    }
    g_clear_object (&self->priv->status_icon);
#endif
    if (self->priv->manager != NULL) {
        g_signal_handlers_disconnect_by_data (self->priv->manager, self);
    }
	g_clear_object (&self->priv->manager);

	G_OBJECT_CLASS (nemo_progress_ui_handler_parent_class)->dispose (obj);
}

static void
nemo_progress_ui_handler_init (NemoProgressUIHandler *self)
{
	self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, NEMO_TYPE_PROGRESS_UI_HANDLER,
						  NemoProgressUIHandlerPriv);

	self->priv->manager = nemo_progress_info_manager_new ();
	g_signal_connect (self->priv->manager, "new-progress-info",
			  G_CALLBACK (new_progress_info_cb), self);
    self->priv->should_show_status_icon = FALSE;
#ifdef NEMO_SMPL
    self->priv->operations = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                   NULL, operation_watch_free);
    self->priv->show_action = g_simple_action_new ("show-file-operation-results", G_VARIANT_TYPE_STRING);
    g_signal_connect_object (self->priv->show_action, "activate",
                             G_CALLBACK (progress_ui_handler_show_results), self, 0);
    g_action_map_add_action (G_ACTION_MAP (nemo_application_get_singleton ()),
                             G_ACTION (self->priv->show_action));
#endif
}

static void
nemo_progress_ui_handler_class_init (NemoProgressUIHandlerClass *klass)
{
	GObjectClass *oclass;

	oclass = G_OBJECT_CLASS (klass);
	oclass->dispose = nemo_progress_ui_handler_dispose;
	
	g_type_class_add_private (klass, sizeof (NemoProgressUIHandlerPriv));
}

NemoProgressUIHandler *
nemo_progress_ui_handler_new (void)
{
	return g_object_new (NEMO_TYPE_PROGRESS_UI_HANDLER, NULL);
}
