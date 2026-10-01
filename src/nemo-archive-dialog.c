/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include "nemo-archive-dialog.h"

#include <eel/eel-stock-dialogs.h>
#include <glib/gi18n.h>
#include <libnemo-private/nemo-archive-create.h>
#include <libnemo-private/nemo-directory.h>
#include <libnemo-private/nemo-progress-info.h>
#include <string.h>

typedef struct {
	GList *sources;
	GFile *destination;
	GCancellable *cancellable;
	gboolean move;
	gboolean closed;
	gboolean checking;
} ArchiveDialog;

typedef struct {
	NemoProgressInfo *progress;
	GApplication *application;
	GFile *source_parent;
	GFile *destination_parent;
	gboolean move;
} ArchiveJob;

static void
archive_dialog_free (gpointer data)
{
	ArchiveDialog *dialog = data;
	g_list_free_full (dialog->sources, g_object_unref);
	g_clear_object (&dialog->destination);
	g_clear_object (&dialog->cancellable);
	g_free (dialog);
}

static void
archive_dialog_destroyed (GtkWidget *widget, gpointer unused)
{
	ArchiveDialog *dialog = g_object_get_data (G_OBJECT (widget), "archive-dialog");
	dialog->closed = TRUE;
	g_cancellable_cancel (dialog->cancellable);
}

static void
archive_reload_directory (GFile *location)
{
	if (location) {
		NemoDirectory *directory = nemo_directory_get (location);
		nemo_directory_force_reload (directory);
		nemo_directory_unref (directory);
	}
}

static void
archive_finished (GObject *source, GAsyncResult *result, gpointer data)
{
	ArchiveJob *job = data;
	g_autoptr (GError) error = NULL;
	NemoArchiveCreateResult outcome = { 0 };

	if (!nemo_archive_create_finish (result, &outcome, &error) &&
	    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_autoptr (GtkWindow) parent = nemo_progress_info_get_parent_window (job->progress);
		eel_show_error_dialog (outcome.archive_ok ? _("Archive created, but the operation is incomplete") :
		                                          _("Could not complete archive creation"),
		                       error->message, parent);
	}
	if (outcome.published) {
		archive_reload_directory (job->destination_parent);
		if (job->move && job->source_parent &&
		    !g_file_equal (job->source_parent, job->destination_parent))
			archive_reload_directory (job->source_parent);
	}
	if (job->application) {
		g_application_release (job->application);
		g_object_unref (job->application);
	}
	g_object_unref (job->progress);
	g_clear_object (&job->source_parent);
	g_clear_object (&job->destination_parent);
	g_free (job);
}

static void
archive_dialog_submit (GtkWidget *widget, gboolean overwrite)
{
	ArchiveDialog *dialog = g_object_get_data (G_OBJECT (widget), "archive-dialog");
	ArchiveJob *job = g_new0 (ArchiveJob, 1);
	GApplication *application = g_application_get_default ();

	job->source_parent = g_file_get_parent (dialog->sources->data);
	job->destination_parent = g_file_get_parent (dialog->destination);
	job->move = dialog->move;
	if (application) {
		job->application = g_object_ref (application);
		g_application_hold (application);
	}
	job->progress = nemo_archive_create_async (
		dialog->sources, dialog->destination, dialog->move, overwrite,
		gtk_window_get_transient_for (GTK_WINDOW (widget)), archive_finished, job);
	gtk_widget_destroy (widget);
}

static void
archive_confirm_response (GtkDialog *confirmation, gint response, gpointer data)
{
	GtkWidget *widget = g_object_ref (data);
	ArchiveDialog *dialog = g_object_get_data (G_OBJECT (widget), "archive-dialog");
	gboolean overwrite = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (confirmation), "overwrite"));

	gtk_widget_destroy (GTK_WIDGET (confirmation));
	if (!dialog->closed) {
		dialog->checking = FALSE;
		gtk_dialog_set_response_sensitive (GTK_DIALOG (widget), GTK_RESPONSE_ACCEPT, TRUE);
		if (response == GTK_RESPONSE_ACCEPT)
			archive_dialog_submit (widget, overwrite);
	}
	g_object_unref (widget);
}

static void
archive_destination_checked (GObject *source, GAsyncResult *result, gpointer data)
{
	GtkWidget *widget = data;
	ArchiveDialog *dialog = g_object_get_data (G_OBJECT (widget), "archive-dialog");
	g_autoptr (GError) error = NULL;
	g_autoptr (GFileInfo) info = g_file_query_info_finish (G_FILE (source), result, &error);

	if (dialog->closed)
		goto out;
	if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
		eel_show_error_dialog (_("Could not inspect the archive destination"), error->message,
		                       GTK_WINDOW (widget));
		goto retry;
	}
	if (info && g_file_info_get_file_type (info) != G_FILE_TYPE_REGULAR) {
		eel_show_error_dialog (_("Choose a different archive name"),
		                       _("The destination already exists and is not a regular file."),
		                       GTK_WINDOW (widget));
		goto retry;
	}
	if (dialog->move || info) {
		g_autofree char *name = g_file_get_parse_name (dialog->destination);
		GtkWidget *confirmation = gtk_message_dialog_new (
			GTK_WINDOW (widget), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
			GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s",
			dialog->move ? _("Create archive and remove the originals?") :
			               _("Replace the existing archive?"));
		g_autofree char *details = g_strdup_printf ("%s\n\n%s%s", name,
			dialog->move ?
			    _("The selected originals will be permanently removed only after the archive "
			      "is complete and its contents have been verified.\n") : "",
			info ? _("The existing archive will be replaced only after the new archive is ready.") : "");
		gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (confirmation), "%s", details);
		gtk_dialog_add_buttons (GTK_DIALOG (confirmation), _("_Cancel"), GTK_RESPONSE_CANCEL,
		                        dialog->move ? _("Create and _Move") : _("_Replace"),
		                        GTK_RESPONSE_ACCEPT, NULL);
		gtk_dialog_set_default_response (GTK_DIALOG (confirmation), GTK_RESPONSE_CANCEL);
		g_object_set_data (G_OBJECT (confirmation), "overwrite", GINT_TO_POINTER (info != NULL));
		g_object_set_data_full (G_OBJECT (confirmation), "archive-chooser", widget, g_object_unref);
		g_signal_connect (confirmation, "response", G_CALLBACK (archive_confirm_response), widget);
		gtk_widget_show (confirmation);
		return;
	}
	archive_dialog_submit (widget, FALSE);
	goto out;

retry:
	dialog->checking = FALSE;
	gtk_dialog_set_response_sensitive (GTK_DIALOG (widget), GTK_RESPONSE_ACCEPT, TRUE);
out:
	g_object_unref (widget);
}

static void
archive_dialog_response (GtkDialog *widget, gint response, gpointer unused)
{
	ArchiveDialog *dialog = g_object_get_data (G_OBJECT (widget), "archive-dialog");
	g_autoptr (GFile) destination = NULL;
	g_autofree char *name = NULL;

	if (response != GTK_RESPONSE_ACCEPT) {
		gtk_widget_destroy (GTK_WIDGET (widget));
		return;
	}
	if (dialog->checking)
		return;
	destination = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (widget));
	if (!destination || !g_file_is_native (destination)) {
		eel_show_error_dialog (_("Choose a local archive destination"),
		                       _("New archives must be saved to a supported local filesystem."),
		                       GTK_WINDOW (widget));
		return;
	}
	name = g_file_get_basename (destination);
	if (!name || !*name || g_str_equal (name, ".") || g_str_equal (name, "..")) {
		eel_show_error_dialog (_("Choose an archive filename"), _("Enter a name for the new archive."),
		                       GTK_WINDOW (widget));
		return;
	}
	if (strlen (name) < 3 || g_ascii_strcasecmp (name + strlen (name) - 3, ".7z") != 0) {
		g_autoptr (GFile) parent = g_file_get_parent (destination);
		g_autofree char *archive_name = g_strconcat (name, ".7z", NULL);
		if (!parent) {
			eel_show_error_dialog (_("Choose an archive filename"),
			                       _("The archive must have a containing folder."),
			                       GTK_WINDOW (widget));
			return;
		}
		g_clear_object (&destination);
		destination = g_file_get_child (parent, archive_name);
	}
	g_set_object (&dialog->destination, destination);
	dialog->checking = TRUE;
	gtk_dialog_set_response_sensitive (widget, GTK_RESPONSE_ACCEPT, FALSE);
	g_file_query_info_async (destination, G_FILE_ATTRIBUTE_STANDARD_TYPE,
	                        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT,
	                        dialog->cancellable, archive_destination_checked,
	                        g_object_ref (widget));
}

void
nemo_archive_dialog_show (GtkWindow *parent, GList *sources,
                         GFile *default_directory, gboolean move)
{
	ArchiveDialog *dialog;
	GtkWidget *widget, *label;
	GtkFileFilter *filter;
	g_autofree char *basename = NULL;
	g_autofree char *name = NULL;
	g_autoptr (GFile) source_directory = NULL;
	g_autoptr (GError) error = NULL;

	if (!sources) {
		eel_show_error_dialog (_("No files selected"),
		                       _("Select files or folders to put in the archive."), parent);
		return;
	}
	dialog = g_new0 (ArchiveDialog, 1);
	for (GList *item = sources; item; item = item->next)
		dialog->sources = g_list_prepend (dialog->sources, g_object_ref (item->data));
	dialog->sources = g_list_reverse (dialog->sources);
	dialog->cancellable = g_cancellable_new ();
	dialog->move = move;
	widget = gtk_file_chooser_dialog_new (
		move ? _("Move into New Archive") : _("Copy into New Archive"), parent,
		GTK_FILE_CHOOSER_ACTION_SAVE, _("_Cancel"), GTK_RESPONSE_CANCEL,
		move ? _("Create and _Move") : _("_Create Archive"), GTK_RESPONSE_ACCEPT, NULL);
	gtk_window_set_modal (GTK_WINDOW (widget), TRUE);
	gtk_window_set_destroy_with_parent (GTK_WINDOW (widget), TRUE);
	gtk_dialog_set_default_response (GTK_DIALOG (widget), GTK_RESPONSE_ACCEPT);
	gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (widget), FALSE);
	g_object_set_data_full (G_OBJECT (widget), "archive-dialog", dialog, archive_dialog_free);
	g_signal_connect (widget, "destroy", G_CALLBACK (archive_dialog_destroyed), NULL);
	g_signal_connect (widget, "response", G_CALLBACK (archive_dialog_response), NULL);

	source_directory = g_file_get_parent (sources->data);
	basename = sources->next ? (source_directory ? g_file_get_basename (source_directory) : NULL) :
	                           g_file_get_basename (sources->data);
	if (!basename || !*basename || strchr (basename, '/'))
		name = g_strdup ("Archive.7z");
	else {
		g_autofree char *display_name = g_filename_display_name (basename);
		name = g_strconcat (display_name, ".7z", NULL);
	}
	filter = gtk_file_filter_new ();
	gtk_file_filter_set_name (filter, _("7z archives"));
	gtk_file_filter_add_pattern (filter, "*.7z");
	gtk_file_filter_add_pattern (filter, "*.7Z");
	gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (widget), filter);
	gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (widget), name);
	label = gtk_label_new (_("7z, LZMA2, maximum compression\n"
	                        "Folder structure and modification times are kept.\n"
	                        "Original permissions, ownership, ACLs and extended attributes are not stored."));
	gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
	gtk_label_set_xalign (GTK_LABEL (label), 0.0);
	gtk_file_chooser_set_extra_widget (GTK_FILE_CHOOSER (widget), label);
	gtk_widget_show (label);
	gtk_widget_show (widget);
	if (default_directory &&
	    !gtk_file_chooser_set_current_folder_file (GTK_FILE_CHOOSER (widget),
	                                              default_directory, &error))
		eel_show_error_dialog (_("Could not open the default archive folder"),
		                       error->message, GTK_WINDOW (widget));
}
