/* -*- Mode: C; indent-tabs-mode: f; c-basic-offset: 4; tab-width: 4 -*- */

/* nemo-file-operations.c - Nemo file operations.

   Copyright (C) 1999, 2000 Free Software Foundation
   Copyright (C) 2000, 2001 Eazel, Inc.
   Copyright (C) 2007 Red Hat, Inc.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public
   License along with this program; if not, write to the
   Free Software Foundation, Inc., 51 Franklin Street - Suite 500,
   Boston, MA 02110-1335, USA.

   Authors: Alexander Larsson <alexl@redhat.com>
            Ettore Perazzoli <ettore@gnu.org>
            Pavel Cisler <pavel@eazel.com>
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  /* for sync_file_range() */
#endif

#include <config.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <locale.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#ifdef NEMO_SMPL
#include <sys/stat.h>
#include <gio/gfiledescriptorbased.h>
#include <gio/gunixinputstream.h>
#endif

#include "nemo-file-operations.h"

#include "nemo-file-changes-queue.h"
#include "nemo-lib-self-check-functions.h"

#include "nemo-progress-info.h"

#include <eel/eel-glib-extensions.h>
#include <eel/eel-gtk-extensions.h>
#include <eel/eel-stock-dialogs.h>
#include <eel/eel-vfs-extensions.h>

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib.h>
#include <libxapp/xapp-favorites.h>

#include "nemo-file-changes-queue.h"
#include "nemo-file-private.h"
#include "nemo-desktop-icon-file.h"
#include "nemo-desktop-link-monitor.h"
#include "nemo-global-preferences.h"
#ifdef NEMO_SMPL
#include "nemo-smpl-prefs.h"
#include "nemo-transfer-safety.h"
#endif
#include "nemo-link.h"
#include "nemo-desktop-utils.h"
#include "nemo-trash-monitor.h"
#include "nemo-file-utilities.h"
#include "nemo-file-conflict-dialog.h"
#include "nemo-file-undo-operations.h"
#include "nemo-file-undo-manager.h"
#include "nemo-job-queue.h"

/* TODO: TESTING!!! */

typedef enum {
    OP_KIND_COPY,
    OP_KIND_MOVE,
    OP_KIND_DELETE,
    OP_KIND_TRASH,
    OP_KIND_EMPTY_TRASH,
    OP_KIND_DUPE,
    OP_KIND_PERMISSIONS,
    OP_KIND_LINK,
    OP_KIND_CREATE,
    OP_KIND_TRUST
} OpKind;

typedef struct {
	GIOSchedulerJob *io_job;
	GtkWindow *parent_window;
    int monitor_num;
	int inhibit_cookie;
	NemoProgressInfo *progress;
	GCancellable *cancellable;
	GHashTable *skip_files;
	GHashTable *skip_readdir_error;
	NemoFileUndoInfo *undo_info;
	gboolean skip_all_error;
	gboolean skip_all_conflict;
	gboolean merge_all;
	gboolean auto_rename_all;
	gboolean replace_all;
	gboolean delete_all;
} CommonJob;

typedef struct {
	CommonJob common;
	gboolean is_move;
	GList *files;
	GFile *destination;
	GFile *desktop_location;
	GFile *fake_display_source;
	GdkPoint *icon_positions;
	int n_icon_positions;
	GHashTable *debuting_files;
	gchar *target_name;
	NemoCopyCallback  done_callback;
	gpointer done_callback_data;
	gboolean verify_after_copy;
#ifndef NEMO_SMPL
	/* Legacy verification-dialog preference, never used to bypass safety
	 * checks in a transactional copy. */
	gboolean verify_skip_all;
#endif
#ifdef NEMO_SMPL
	gboolean had_errors;
	NemoProgressResult result;
	GHashTable *incomplete_paths;
	gboolean undo_has_changes;
	NemoTransferGuard *transfer_guard;
	NemoTransferUndo *transfer_undo;
	guint metadata_limitations;
#endif
} CopyMoveJob;

/* Static flag: when TRUE the next copy/move job will verify checksums.
 * Set from the UI thread before starting the operation, consumed once. */
static int _nemo_next_copy_verify = -1;

static gboolean
consume_copy_verification (void)
{
#ifdef NEMO_SMPL
	gboolean verify = _nemo_next_copy_verify < 0 ?
	                  nemo_smpl_verify_file_copies () : _nemo_next_copy_verify;
#else
	gboolean verify = _nemo_next_copy_verify > 0;
#endif
	_nemo_next_copy_verify = -1;
	return verify;
}

typedef struct {
	CommonJob common;
	GList *files;
	gboolean try_trash;
	gboolean user_cancel;
	NemoDeleteCallback done_callback;
	gpointer done_callback_data;
} DeleteJob;

typedef struct {
	CommonJob common;
	GFile *dest_dir;
	char *filename;
	gboolean make_dir;
	GFile *src;
	char *src_data;
	int length;
	GdkPoint position;
	gboolean has_position;
	GFile *created_file;
	NemoCreateCallback done_callback;
	gpointer done_callback_data;
} CreateJob;


typedef struct {
	CommonJob common;
	GList *trash_dirs;
	gboolean should_confirm;
	NemoOpCallback done_callback;
	gpointer done_callback_data;
} EmptyTrashJob;

typedef struct {
	CommonJob common;
	GFile *file;
	gboolean interactive;
	NemoOpCallback done_callback;
	gpointer done_callback_data;
} MarkTrustedJob;

typedef struct {
	CommonJob common;
	GFile *file;
	NemoOpCallback done_callback;
	gpointer done_callback_data;
	guint32 file_permissions;
	guint32 file_mask;
	guint32 dir_permissions;
	guint32 dir_mask;
} SetPermissionsJob;

typedef struct {
	int num_files;
	goffset num_bytes;
	int num_files_since_progress;
	OpKind op;
} SourceInfo;

typedef struct {
	int num_files;
	goffset num_bytes;
	OpKind op;
	guint64 last_report_time;
	int last_reported_files_left;
} TransferInfo;

#define SECONDS_NEEDED_FOR_RELIABLE_TRANSFER_RATE 8
#define US_PER_MS 1000
#define PROGRESS_UPDATE_THRESHOLD 250

#define MAXIMUM_DISPLAYED_FILE_NAME_LENGTH 50

#define IS_IO_ERROR(__error, KIND) (((__error)->domain == G_IO_ERROR && (__error)->code == G_IO_ERROR_ ## KIND))

#define SKIP _("_Skip")
#define SKIP_ALL _("S_kip All")
#define RETRY _("_Retry")
#define DELETE_ALL _("Delete _All")
#define REPLACE _("_Replace")
#define REPLACE_ALL _("Replace _All")
#define MERGE _("_Merge")
#define MERGE_ALL _("Merge _All")
#define COPY_FORCE _("Copy _Anyway")
;
static void add_job_to_job_queue (GIOSchedulerJobFunc job_func,
                                             gpointer user_data,
                                        GCancellable *cancellable,
                                    NemoProgressInfo *info,
                                              OpKind  kind);

static void
mark_desktop_file_trusted (CommonJob *common,
			   GCancellable *cancellable,
			   GFile *file,
			   gboolean interactive);

static gboolean
is_all_button_text (const char *button_text)
{
	g_assert (button_text != NULL);

	return !strcmp (button_text, SKIP_ALL) ||
	       !strcmp (button_text, REPLACE_ALL) ||
	       !strcmp (button_text, DELETE_ALL) ||
	       !strcmp (button_text, MERGE_ALL);
}

static void scan_sources (GList *files,
			  SourceInfo *source_info,
			  CommonJob *job,
			  OpKind kind);


static gboolean empty_trash_job (GIOSchedulerJob *io_job,
				 GCancellable *cancellable,
				 gpointer user_data);

static char * query_fs_type (GFile *file,
			     GCancellable *cancellable);

/* keep in time with format_time()
 *
 * This counts and outputs the number of “time units”
 * formatted and displayed by format_time().
 * For instance, if format_time outputs “3 hours, 4 minutes”
 * it yields 7.
 */
static int
seconds_count_format_time_units (int seconds)
{
	int minutes;
	int hours;

	if (seconds < 0) {
		/* Just to make sure... */
		seconds = 0;
	}

	if (seconds < 60) {
		/* seconds */
		return seconds;
	}

	if (seconds < 60*60) {
		/* minutes */
		minutes = seconds / 60;
		return minutes;
	}

	hours = seconds / (60*60);

	if (seconds < 60*60*4) {
		/* minutes + hours */
		minutes = (seconds - hours * 60 * 60) / 60;
		return minutes + hours;
	}

	return hours;
}

static char *
format_time (int seconds)
{
	int minutes;
	int hours;
	char *res;

	if (seconds < 0) {
		/* Just to make sure... */
		seconds = 0;
	}

	if (seconds < 60) {
		return g_strdup_printf (ngettext ("%'d second","%'d seconds", (int) seconds), (int) seconds);
	}

	if (seconds < 60*60) {
		minutes = seconds / 60;
		return g_strdup_printf (ngettext ("%'d minute", "%'d minutes", minutes), minutes);
	}

	hours = seconds / (60*60);

	if (seconds < 60*60*4) {
		char *h, *m;

		minutes = (seconds - hours * 60 * 60) / 60;

		h = g_strdup_printf (ngettext ("%'d hour", "%'d hours", hours), hours);
		m = g_strdup_printf (ngettext ("%'d minute", "%'d minutes", minutes), minutes);
		res = g_strconcat (h, ", ", m, NULL);
		g_free (h);
		g_free (m);
		return res;
	}

	return g_strdup_printf (ngettext ("approximately %'d hour",
					  "approximately %'d hours",
					  hours), hours);
}

static char *
shorten_utf8_string (const char *base, int reduce_by_num_bytes)
{
	int len;
	char *ret;
	const char *p;

	len = strlen (base);
	len -= reduce_by_num_bytes;

	if (len <= 0) {
		return NULL;
	}

	ret = g_new (char, len + 1);

	p = base;
	while (len) {
		const char *next;
		next = g_utf8_next_char (p);
		if (next - p > len || *next == '\0') {
			break;
		}

		len -= next - p;
		p = next;
	}

	if (p - base == 0) {
		g_free (ret);
		return NULL;
	} else {
		memcpy (ret, base, p - base);
		ret[p - base] = '\0';
		return ret;
	}
}

/* Note that we have these two separate functions with separate format
 * strings for ease of localization.
 */

static char *
get_link_name (const char *name, int count, int max_length)
{
	const char *format;
	char *result;
	int unshortened_length;
	gboolean use_count;

	g_assert (name != NULL);

	if (count < 0) {
		g_warning ("bad count in get_link_name");
		count = 0;
	}

	if (count <= 2) {
		/* Handle special cases for low numbers.
		 * Perhaps for some locales we will need to add more.
		 */
		switch (count) {
		default:
			g_assert_not_reached ();
			/* fall through */
		case 0:
			/* duplicate original file name */
			format = "%s";
			break;
		case 1:
			/* appended to new link file */
			format = _("Link to %s");
			break;
		case 2:
			/* appended to new link file */
			format = _("Another link to %s");
			break;
		}

		use_count = FALSE;
	} else {
		/* Handle special cases for the first few numbers of each ten.
		 * For locales where getting this exactly right is difficult,
		 * these can just be made all the same as the general case below.
		 */
		switch (count % 10) {
		case 1:
			/* Localizers: Feel free to leave out the "st" suffix                                 codespell:ignore
			 * if there's no way to do that nicely for a
			 * particular language.
			 */
			format = _("%'dst link to %s");
			break;
		case 2:
			/* appended to new link file */
			format = _("%'dnd link to %s");
			break;
		case 3:
			/* appended to new link file */
			format = _("%'drd link to %s");
			break;
		default:
			/* appended to new link file */
			format = _("%'dth link to %s");
			break;
		}

		use_count = TRUE;
	}

	if (use_count)
		result = g_strdup_printf (format, count, name);
	else
		result = g_strdup_printf (format, name);

	if (max_length > 0 && (unshortened_length = strlen (result)) > max_length) {
		char *new_name;

		new_name = shorten_utf8_string (name, unshortened_length - max_length);
		if (new_name) {
			g_free (result);

			if (use_count)
				result = g_strdup_printf (format, count, new_name);
			else
				result = g_strdup_printf (format, new_name);

			g_assert ((int)strlen (result) <= max_length);
			g_free (new_name);
		}
	}

	return result;
}


/* Localizers:
 * Feel free to leave out the st, nd, rd and th suffix or                codespell:ignore
 * make some or all of them match.
 */

/* localizers: tag used to detect the first copy of a file */
static const char untranslated_copy_duplicate_tag[] = N_(" (copy)");
/* localizers: tag used to detect the second copy of a file */
static const char untranslated_another_copy_duplicate_tag[] = N_(" (another copy)");

/* localizers: tag used to detect the x11th copy of a file */
static const char untranslated_x11th_copy_duplicate_tag[] = N_("th copy)");
/* localizers: tag used to detect the x12th copy of a file */
static const char untranslated_x12th_copy_duplicate_tag[] = N_("th copy)");
/* localizers: tag used to detect the x13th copy of a file */
static const char untranslated_x13th_copy_duplicate_tag[] = N_("th copy)");

/* localizers: tag used to detect the x1st copy of a file */
static const char untranslated_st_copy_duplicate_tag[] = N_("st copy)");
/* localizers: tag used to detect the x2nd copy of a file */
static const char untranslated_nd_copy_duplicate_tag[] = N_("nd copy)");
/* localizers: tag used to detect the x3rd copy of a file */
static const char untranslated_rd_copy_duplicate_tag[] = N_("rd copy)");

/* localizers: tag used to detect the xxth copy of a file */
static const char untranslated_th_copy_duplicate_tag[] = N_("th copy)");

#define COPY_DUPLICATE_TAG _(untranslated_copy_duplicate_tag)
#define ANOTHER_COPY_DUPLICATE_TAG _(untranslated_another_copy_duplicate_tag)
#define X11TH_COPY_DUPLICATE_TAG _(untranslated_x11th_copy_duplicate_tag)
#define X12TH_COPY_DUPLICATE_TAG _(untranslated_x12th_copy_duplicate_tag)
#define X13TH_COPY_DUPLICATE_TAG _(untranslated_x13th_copy_duplicate_tag)

#define ST_COPY_DUPLICATE_TAG _(untranslated_st_copy_duplicate_tag)
#define ND_COPY_DUPLICATE_TAG _(untranslated_nd_copy_duplicate_tag)
#define RD_COPY_DUPLICATE_TAG _(untranslated_rd_copy_duplicate_tag)
#define TH_COPY_DUPLICATE_TAG _(untranslated_th_copy_duplicate_tag)

/* localizers: appended to first file copy */
static const char untranslated_first_copy_duplicate_format[] = N_("%s (copy)%s");
/* localizers: appended to second file copy */
static const char untranslated_second_copy_duplicate_format[] = N_("%s (another copy)%s");

/* localizers: appended to x11th file copy */
static const char untranslated_x11th_copy_duplicate_format[] = N_("%s (%'dth copy)%s");
/* localizers: appended to x12th file copy */
static const char untranslated_x12th_copy_duplicate_format[] = N_("%s (%'dth copy)%s");
/* localizers: appended to x13th file copy */
static const char untranslated_x13th_copy_duplicate_format[] = N_("%s (%'dth copy)%s");

/* localizers: if in your language there's no difference between 1st, 2nd, 3rd and nth
 * plurals, you can leave the st, nd, rd suffixes out and just make all the translated
 * strings look like "%s (copy %'d)%s".
 */

/* localizers: appended to x1st file copy */
static const char untranslated_st_copy_duplicate_format[] = N_("%s (%'dst copy)%s");
/* localizers: appended to x2nd file copy */
static const char untranslated_nd_copy_duplicate_format[] = N_("%s (%'dnd copy)%s");
/* localizers: appended to x3rd file copy */
static const char untranslated_rd_copy_duplicate_format[] = N_("%s (%'drd copy)%s");
/* localizers: appended to xxth file copy */
static const char untranslated_th_copy_duplicate_format[] = N_("%s (%'dth copy)%s");

#define FIRST_COPY_DUPLICATE_FORMAT _(untranslated_first_copy_duplicate_format)
#define SECOND_COPY_DUPLICATE_FORMAT _(untranslated_second_copy_duplicate_format)
#define X11TH_COPY_DUPLICATE_FORMAT _(untranslated_x11th_copy_duplicate_format)
#define X12TH_COPY_DUPLICATE_FORMAT _(untranslated_x12th_copy_duplicate_format)
#define X13TH_COPY_DUPLICATE_FORMAT _(untranslated_x13th_copy_duplicate_format)

#define ST_COPY_DUPLICATE_FORMAT _(untranslated_st_copy_duplicate_format)
#define ND_COPY_DUPLICATE_FORMAT _(untranslated_nd_copy_duplicate_format)
#define RD_COPY_DUPLICATE_FORMAT _(untranslated_rd_copy_duplicate_format)
#define TH_COPY_DUPLICATE_FORMAT _(untranslated_th_copy_duplicate_format)

static char *
extract_string_until (const char *original, const char *until_substring)
{
	char *result;

	g_assert ((int) strlen (original) >= until_substring - original);
	g_assert (until_substring - original >= 0);

	result = g_malloc (until_substring - original + 1);
	strncpy (result, original, until_substring - original);
	result[until_substring - original] = '\0';

	return result;
}

/* Dismantle a file name, separating the base name, the file suffix and removing any
 * (xxxcopy), etc. string. Figure out the count that corresponds to the given
 * (xxxcopy) substring.
 */
static void
parse_previous_duplicate_name (const char *name,
			       char **name_base,
			       const char **suffix,
			       int *count)
{
	const char *tag;

	g_assert (name[0] != '\0');

	*suffix = eel_filename_get_extension_offset (name + 1);

	if (*suffix == NULL || (*suffix)[1] == '\0') {
		/* no suffix */
		*suffix = "";
	}

	tag = strstr (name, COPY_DUPLICATE_TAG);
	if (tag != NULL) {
		if (tag > *suffix) {
			/* handle case "foo. (copy)" */
			*suffix = "";
		}
		*name_base = extract_string_until (name, tag);
		*count = 1;
		return;
	}


	tag = strstr (name, ANOTHER_COPY_DUPLICATE_TAG);
	if (tag != NULL) {
		if (tag > *suffix) {
			/* handle case "foo. (another copy)" */
			*suffix = "";
		}
		*name_base = extract_string_until (name, tag);
		*count = 2;
		return;
	}


	/* Check to see if we got one of st, nd, rd, th. */
	tag = strstr (name, X11TH_COPY_DUPLICATE_TAG);

	if (tag == NULL) {
		tag = strstr (name, X12TH_COPY_DUPLICATE_TAG);
	}
	if (tag == NULL) {
		tag = strstr (name, X13TH_COPY_DUPLICATE_TAG);
	}

	if (tag == NULL) {
		tag = strstr (name, ST_COPY_DUPLICATE_TAG);
	}
	if (tag == NULL) {
		tag = strstr (name, ND_COPY_DUPLICATE_TAG);
	}
	if (tag == NULL) {
		tag = strstr (name, RD_COPY_DUPLICATE_TAG);
	}
	if (tag == NULL) {
		tag = strstr (name, TH_COPY_DUPLICATE_TAG);
	}

	/* If we got one of st, nd, rd, th, fish out the duplicate number. */
	if (tag != NULL) {
		/* localizers: opening parentheses to match the "th copy)" string */
		tag = strstr (name, _(" ("));
		if (tag != NULL) {
			if (tag > *suffix) {
				/* handle case "foo. (22nd copy)" */
				*suffix = "";
			}
			*name_base = extract_string_until (name, tag);
			/* localizers: opening parentheses of the "th copy)" string */
			if (sscanf (tag, _(" (%'d"), count) == 1) {
				if (*count < 1 || *count > 1000000) {
					/* keep the count within a reasonable range */
					*count = 0;
				}
				return;
			}
			*count = 0;
			return;
		}
	}


	*count = 0;
	if (**suffix != '\0') {
		*name_base = extract_string_until (name, *suffix);
	} else {
		*name_base = g_strdup (name);
	}
}

static char *
make_next_duplicate_name (const char *base, const char *suffix, int count, int max_length)
{
	const char *format;
	char *result;
	int unshortened_length;
	gboolean use_count;

	if (count < 1) {
		g_warning ("bad count %d in get_duplicate_name", count);
		count = 1;
	}

	if (count <= 2) {

		/* Handle special cases for low numbers.
		 * Perhaps for some locales we will need to add more.
		 */
		switch (count) {
		default:
			g_assert_not_reached ();
			/* fall through */
		case 1:
			format = FIRST_COPY_DUPLICATE_FORMAT;
			break;
		case 2:
			format = SECOND_COPY_DUPLICATE_FORMAT;
			break;

		}

		use_count = FALSE;
	} else {

		/* Handle special cases for the first few numbers of each ten.
		 * For locales where getting this exactly right is difficult,
		 * these can just be made all the same as the general case below.
		 */

		/* Handle special cases for x11th - x20th.
		 */
		switch (count % 100) {
		case 11:
			format = X11TH_COPY_DUPLICATE_FORMAT;
			break;
		case 12:
			format = X12TH_COPY_DUPLICATE_FORMAT;
			break;
		case 13:
			format = X13TH_COPY_DUPLICATE_FORMAT;
			break;
		default:
			format = NULL;
			break;
		}

		if (format == NULL) {
			switch (count % 10) {
			case 1:
				format = ST_COPY_DUPLICATE_FORMAT;
				break;
			case 2:
				format = ND_COPY_DUPLICATE_FORMAT;
				break;
			case 3:
				format = RD_COPY_DUPLICATE_FORMAT;
				break;
			default:
				/* The general case. */
				format = TH_COPY_DUPLICATE_FORMAT;
				break;
			}
		}

		use_count = TRUE;

	}

	if (use_count)
		result = g_strdup_printf (format, base, count, suffix);
	else
		result = g_strdup_printf (format, base, suffix);

	if (max_length > 0 && (unshortened_length = strlen (result)) > max_length) {
		char *new_base;

		new_base = shorten_utf8_string (base, unshortened_length - max_length);
		if (new_base) {
			g_free (result);

			if (use_count)
				result = g_strdup_printf (format, new_base, count, suffix);
			else
				result = g_strdup_printf (format, new_base, suffix);

			g_assert ((int)strlen (result) <= max_length);
			g_free (new_base);
		}
	}

	return result;
}

static char *
get_duplicate_name (const char *name, int count_increment, int max_length)
{
	char *result;
	char *name_base;
	const char *suffix;
	int count;

	parse_previous_duplicate_name (name, &name_base, &suffix, &count);
	result = make_next_duplicate_name (name_base, suffix, count + count_increment, max_length);

	g_free (name_base);

	return result;
}

static gboolean
has_invalid_xml_char (char *str)
{
	gunichar c;

	while (*str != 0) {
		c = g_utf8_get_char (str);
		/* characters XML permits */
		if (!(c == 0x9 ||
		      c == 0xA ||
		      c == 0xD ||
		      (c >= 0x20 && c <= 0xD7FF) ||
		      (c >= 0xE000 && c <= 0xFFFD) ||
		      (c >= 0x10000 && c <= 0x10FFFF))) {
			return TRUE;
		}
		str = g_utf8_next_char (str);
	}
	return FALSE;
}


static char *
custom_full_name_to_string (char *format, va_list va)
{
	GFile *file;

	file = va_arg (va, GFile *);

	return g_file_get_parse_name (file);
}

static void
custom_full_name_skip (va_list *va)
{
	(void) va_arg (*va, GFile *);
}

static char *
custom_basename_to_string (char *format, va_list va)
{
	GFile *file;
#ifndef NEMO_SMPL
	GFileInfo *info;
#endif
	char *name, *basename, *tmp;

	file = va_arg (va, GFile *);

#ifndef NEMO_SMPL
	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME,
				  0,
				  g_cancellable_get_current (),
				  NULL);

	name = NULL;
	if (info) {
		name = g_strdup (g_file_info_get_display_name (info));
		g_object_unref (info);
	}
#else
	/* Formatting an error must not repeat the metadata request that failed. */
	name = NULL;
#endif

    if (name == NULL) {
        basename = g_file_get_basename (file);

        if (basename != NULL) {
            if (g_utf8_validate (basename, -1, NULL)) {
                name = basename;
            } else {
                name = g_uri_escape_string (basename, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, TRUE);
                g_free (basename);
            }
        }
    }

    if (name == NULL) {
        name = g_file_get_parse_name (file);
    }

	/* Some chars can't be put in the markup we use for the dialogs... */
	if (has_invalid_xml_char (name)) {
		tmp = name;
		name = g_uri_escape_string (name, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, TRUE);
		g_free (tmp);
	}

	/* Finally, if the string is too long, truncate it. */
	if (name != NULL) {
		tmp = name;
		name = eel_str_middle_truncate (tmp, MAXIMUM_DISPLAYED_FILE_NAME_LENGTH);
		g_free (tmp);
	}


	return name;
}

static void
custom_basename_skip (va_list *va)
{
	(void) va_arg (*va, GFile *);
}


static char *
custom_size_to_string (char *format, va_list va)
{
	goffset size;

	size = va_arg (va, goffset);

	int prefix;
	prefix = nemo_global_preferences_get_size_prefix_preference ();

	return g_format_size_full (size, prefix);
}

static void
custom_size_skip (va_list *va)
{
	(void) va_arg (*va, goffset);
}

static char *
custom_time_to_string (char *format, va_list va)
{
	int secs;

	secs = va_arg (va, int);
	return format_time (secs);
}

static void
custom_time_skip (va_list *va)
{
	(void) va_arg (*va, int);
}

static char *
custom_mount_to_string (char *format, va_list va)
{
	GMount *mount;

	mount = va_arg (va, GMount *);
	return g_mount_get_name (mount);
}

static void
custom_mount_skip (va_list *va)
{
	(void) va_arg (*va, GMount *);
}


static EelPrintfHandler handlers[] = {
	{ 'F', custom_full_name_to_string, custom_full_name_skip },
	{ 'B', custom_basename_to_string, custom_basename_skip },
	{ 'S', custom_size_to_string, custom_size_skip },
	{ 'T', custom_time_to_string, custom_time_skip },
	{ 'V', custom_mount_to_string, custom_mount_skip },
	{ 0 }
};


static char *
f (const char *format, ...) {
	va_list va;
	char *res;

	va_start (va, format);
	res = eel_strdup_vprintf_with_custom (handlers, format, va);
	va_end (va);

	return res;
}

static void
get_best_name (GFile *file, gchar **name)
{
    gchar *out;

    if (g_file_is_native (file)) {
        gchar *path = g_file_get_path (file);

        if (g_str_has_prefix (path, g_get_home_dir ())) {
            GString *str = g_string_new (path);
            str = g_string_erase (str, 0, strlen (g_get_home_dir ()));
            str = g_string_prepend (str, "~");

            out = g_string_free (str, FALSE);
        } else {
            out = g_strdup (path);
        }

        g_free (path);
    } else {
        out = g_file_get_basename (file);

        if (out == NULL) {
            out = g_file_get_parse_name (file);
        }
    }

    *name = out;
}

static void
get_parent_name (GFile *file, gchar **name)
{
    GFile *parent = g_file_get_parent (file);

    if (!parent)
        return;

    gchar *get = NULL;
    get_best_name (parent, &get);

    g_object_unref (parent);

    *name = get;
}

/* adapted from gio/glocalfile.c */
static gboolean
_g_local_file_delete (GFile         *file,
                     GCancellable  *cancellable,
                     GError       **error)
{
    gchar *path;

#ifdef NEMO_SMPL
    if (g_cancellable_set_error_if_cancelled (cancellable, error)) {
        return FALSE;
    }
#endif
    path = g_file_get_path (file);

    if (g_remove (path) == -1) {
        int errsv = errno;

        /* Posix allows EEXIST too, but the more sane error
           is G_IO_ERROR_NOT_FOUND, and it's what nautilus
           expects */
        if (errsv == EEXIST) {
            errsv = ENOTEMPTY;
        }

        g_set_error (error, G_IO_ERROR,
                     g_io_error_from_errno (errsv),
                     _("Error removing file: %s"),
                     g_strerror (errsv));

        g_free (path);
        return FALSE;
    }

    g_free (path);
    return TRUE;
}

static gboolean
file_delete_wrapper (GFile        *file,
                     GCancellable *cancellable,
                     GError      **error)
{
    gboolean ret;
    gchar *uri;

    ret = FALSE;

    uri = g_file_get_uri (file);

    if (g_file_is_native (file) && !eel_uri_is_favorite (uri)) {
        ret = _g_local_file_delete (file, cancellable, error);
    } else {
        ret = g_file_delete (file, cancellable, error);
    }

    g_free (uri);

    return ret;
}

static void
generate_initial_job_details (NemoProgressInfo *info,
                              OpKind            kind,
                              GList            *files,
                              GFile            *destination)
{
    gchar *s = NULL;
    gchar *dest_name = NULL;
    gchar *src_name = NULL;

    if (destination != NULL)
        get_best_name (destination, &dest_name);

    if (files != NULL)
        get_parent_name (files->data, &src_name);

    switch (kind) {
        case OP_KIND_COPY:
            g_return_if_fail (files != NULL);
            g_return_if_fail (destination != NULL);

            s = f (ngettext("Waiting to copy a file from '%1$s' to '%2$s'",
                            "Waiting to copy files from '%1$s' to '%2$s'",
                            g_list_length (files)),
                            src_name, dest_name);
            break;
        case OP_KIND_MOVE:
            g_return_if_fail (files != NULL);
            g_return_if_fail (destination != NULL);

            s = f (ngettext("Waiting to move a file from '%1$s' to '%2$s'",
                            "Waiting to move files from '%1$s' to '%2$s'",
                            g_list_length (files)),
                            src_name, dest_name);
            break;
        case OP_KIND_DELETE:
            g_return_if_fail (files != NULL);

            s = f (ngettext("Waiting to permanently delete a file from '%s'",
                            "Waiting to permanently delete files from '%s'",
                            g_list_length (files)),
                            src_name);
            break;
        case OP_KIND_TRASH:
            g_return_if_fail (files != NULL);

            s = f (ngettext("Waiting to trash a file in '%s'",
                            "Waiting to trash files in '%s'",
                            g_list_length (files)),
                            src_name);
            break;
        case OP_KIND_EMPTY_TRASH:
            s = f (_("Waiting to empty the trash"));
            break;
        case OP_KIND_DUPE:
            g_return_if_fail (files != NULL);
            g_return_if_fail (destination != NULL);

            s = f (ngettext("Waiting to duplicate a file in '%s'",
                            "Waiting to duplicate files in '%s'",
                            g_list_length (files)),
                            dest_name);
            break;
        case OP_KIND_PERMISSIONS:
            g_return_if_fail (destination != NULL);

            s = f (_("Waiting to change permissions of files in '%s'"), dest_name);
            break;
        case OP_KIND_LINK:
            g_return_if_fail (files != NULL);
            g_return_if_fail (destination != NULL);

            s = f (ngettext("Waiting to link a file from '%1$s' to '%2$s'",
                            "Waiting to link files from '%1$s' to '%2$s'",
                            g_list_length (files)),
                            src_name, dest_name);
            break;
        case OP_KIND_TRUST:
        case OP_KIND_CREATE:
        default:
            break;
    }
#ifdef NEMO_SMPL
    if (kind == OP_KIND_COPY || kind == OP_KIND_MOVE || kind == OP_KIND_DUPE) {
        NemoProgressResult initial_result = {
            .operation = kind == OP_KIND_MOVE ? NEMO_PROGRESS_OPERATION_MOVE :
                                               NEMO_PROGRESS_OPERATION_COPY
        };
        nemo_progress_info_set_result (info, &initial_result);
        if (src_name != NULL && dest_name != NULL) {
            nemo_progress_info_take_completion_context (
                info, f (_("From: %1$s\nTo: %2$s"), src_name, dest_name));
        } else if (dest_name != NULL) {
            nemo_progress_info_take_completion_context (info, f (_("To: %s"), dest_name));
        }
    }
#endif
    g_free (dest_name);
    g_free (src_name);

    nemo_progress_info_take_initial_details (info, s);
}


#define op_job_new(__type, parent_window) ((__type *)(init_common (sizeof(__type), parent_window)))

static gpointer
init_common (gsize job_size,
	     GtkWindow *parent_window)
{
	CommonJob *common;

	common = g_malloc0 (job_size);

	if (parent_window) {
		common->parent_window = parent_window;
		g_object_add_weak_pointer (G_OBJECT (common->parent_window),
					   (gpointer *) &common->parent_window);

	}
	common->progress = nemo_progress_info_new ();
	common->cancellable = nemo_progress_info_get_cancellable (common->progress);
	common->inhibit_cookie = -1;
    common->monitor_num = 0;
	if (parent_window) {
        common->monitor_num = nemo_desktop_utils_get_monitor_for_widget (GTK_WIDGET (parent_window));
	}

	return common;
}

static void
finalize_common (CommonJob *common)
{
	nemo_progress_info_finish (common->progress);

	if (common->inhibit_cookie != -1) {
		nemo_uninhibit_power_manager (common->inhibit_cookie);
	}

	common->inhibit_cookie = -1;

	if (common->parent_window) {
		g_object_remove_weak_pointer (G_OBJECT (common->parent_window),
					      (gpointer *) &common->parent_window);
	}

	if (common->skip_files) {
		g_hash_table_destroy (common->skip_files);
	}
	if (common->skip_readdir_error) {
		g_hash_table_destroy (common->skip_readdir_error);
	}

	if (common->undo_info != NULL) {
		nemo_file_undo_manager_set_action (common->undo_info);
		g_object_unref (common->undo_info);
	}

	g_object_unref (common->progress);
	g_object_unref (common->cancellable);
	g_free (common);
}

static void
skip_file (CommonJob *common,
	   GFile *file)
{
	if (common->skip_files == NULL) {
		common->skip_files =
			g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, NULL);
	}

	g_hash_table_insert (common->skip_files, g_object_ref (file), file);
}

static void
skip_readdir_error (CommonJob *common,
		    GFile *dir)
{
	if (common->skip_readdir_error == NULL) {
		common->skip_readdir_error =
			g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, NULL);
	}

	g_hash_table_insert (common->skip_readdir_error, g_object_ref (dir), dir);
}

static gboolean
should_skip_file (CommonJob *common,
		  GFile *file)
{
	if (common->skip_files != NULL) {
		return g_hash_table_lookup (common->skip_files, file) != NULL;
	}
	return FALSE;
}

static gboolean
should_skip_readdir_error (CommonJob *common,
			   GFile *dir)
{
	if (common->skip_readdir_error != NULL) {
		return g_hash_table_lookup (common->skip_readdir_error, dir) != NULL;
	}
	return FALSE;
}

static gboolean
can_delete_without_confirm (GFile *file)
{
	if (g_file_has_uri_scheme (file, "burn") ||
        g_file_has_uri_scheme (file, "recent") ||
        g_file_has_uri_scheme (file, "favorites") ||
	    g_file_has_uri_scheme (file, "x-nemo-desktop")) {
		return TRUE;
	}

	return FALSE;
}

static gboolean
can_delete_files_without_confirm (GList *files)
{
	g_assert (files != NULL);

	while (files != NULL) {
		if (!can_delete_without_confirm (files->data)) {
			return FALSE;
		}

		files = files->next;
	}

	return TRUE;
}

typedef struct {
	GtkWindow **parent_window;
	gboolean ignore_close_box;
	GtkMessageType message_type;
	const char *primary_text;
	const char *secondary_text;
	const char *details_text;
	const char **button_titles;
	gboolean show_all;
#ifdef NEMO_SMPL
	NemoProgressInfo *progress;
#endif

	int result;
} RunSimpleDialogData;

static gboolean
do_run_simple_dialog (gpointer _data)
{
	RunSimpleDialogData *data = _data;
	const char *button_title;
        GtkWidget *dialog;
	int result;
	int response_id;

	/* Create the dialog. */
	dialog = gtk_message_dialog_new (*data->parent_window,
					 0,
					 data->message_type,
					 GTK_BUTTONS_NONE,
					 NULL);

	g_object_set (dialog,
		      "text", data->primary_text,
		      "secondary-text", data->secondary_text,
		      NULL);

	for (response_id = 0;
	     data->button_titles[response_id] != NULL;
	     response_id++) {
		button_title = data->button_titles[response_id];
		if (!data->show_all && is_all_button_text (button_title)) {
			continue;
		}

		gtk_dialog_add_button (GTK_DIALOG (dialog), button_title, response_id);
	}
	if (response_id > 1) {
		if (button_title == _("Empty _Trash")) {
			gtk_dialog_set_default_response (GTK_DIALOG (dialog), 0);
		} else {
			gtk_dialog_set_default_response (GTK_DIALOG (dialog), response_id - 1);
		}
	}

	if (data->details_text) {
		eel_gtk_message_dialog_set_details_label (GTK_MESSAGE_DIALOG (dialog),
							  data->details_text);
	}

	/* Run it. */
#ifdef NEMO_SMPL
        nemo_progress_info_attach_dialog (data->progress, GTK_WINDOW (dialog));
#endif
        result = gtk_dialog_run (GTK_DIALOG (dialog));

	while ((result == GTK_RESPONSE_NONE || result == GTK_RESPONSE_DELETE_EVENT) && data->ignore_close_box) {
		result = gtk_dialog_run (GTK_DIALOG (dialog));
	}

	gtk_widget_destroy (dialog);

	data->result = result;

	return FALSE;
}

/* NOTE: This frees the primary / secondary strings, in order to
   avoid doing that everywhere. So, make sure they are strduped */

static int
run_simple_dialog_va (CommonJob *job,
		      gboolean ignore_close_box,
		      GtkMessageType message_type,
		      char *primary_text,
		      char *secondary_text,
		      const char *details_text,
		      gboolean show_all,
		      va_list varargs)
{
	RunSimpleDialogData *data;
	int res;
	const char *button_title;
	GPtrArray *ptr_array;

    nemo_progress_info_pause (job->progress);

	data = g_new0 (RunSimpleDialogData, 1);
	data->parent_window = &job->parent_window;
	data->ignore_close_box = ignore_close_box;
	data->message_type = message_type;
	data->primary_text = primary_text;
	data->secondary_text = secondary_text;
	data->details_text = details_text;
	data->show_all = show_all;
#ifdef NEMO_SMPL
	data->progress = job->progress;
#endif

	ptr_array = g_ptr_array_new ();
	while ((button_title = va_arg (varargs, const char *)) != NULL) {
		g_ptr_array_add (ptr_array, (char *)button_title);
	}
	g_ptr_array_add (ptr_array, NULL);
	data->button_titles = (const char **)g_ptr_array_free (ptr_array, FALSE);

	g_io_scheduler_job_send_to_mainloop (job->io_job,
					     do_run_simple_dialog,
					     data,
					     NULL);
	res = data->result;

	g_free (data->button_titles);
	g_free (data);

    nemo_progress_info_resume (job->progress);

	g_free (primary_text);
	g_free (secondary_text);

	return res;
}

#if 0 /* Not used at the moment */
static int
run_simple_dialog (CommonJob *job,
		   gboolean ignore_close_box,
		   GtkMessageType message_type,
		   char *primary_text,
		   char *secondary_text,
		   const char *details_text,
		   ...)
{
	va_list varargs;
	int res;

	va_start (varargs, details_text);
	res = run_simple_dialog_va (job,
				    ignore_close_box,
				    message_type,
				    primary_text,
				    secondary_text,
				    details_text,
				    varargs);
	va_end (varargs);
	return res;
}
#endif

static int
run_error (CommonJob *job,
	   char *primary_text,
	   char *secondary_text,
	   const char *details_text,
	   gboolean show_all,
	   ...)
{
	va_list varargs;
	int res;

	va_start (varargs, show_all);
	res = run_simple_dialog_va (job,
				    FALSE,
				    GTK_MESSAGE_ERROR,
				    primary_text,
				    secondary_text,
				    details_text,
				    show_all,
				    varargs);
	va_end (varargs);
	return res;
}

static int
run_warning (CommonJob *job,
	     char *primary_text,
	     char *secondary_text,
	     const char *details_text,
	     gboolean show_all,
	     ...)
{
	va_list varargs;
	int res;

	va_start (varargs, show_all);
	res = run_simple_dialog_va (job,
				    FALSE,
				    GTK_MESSAGE_WARNING,
				    primary_text,
				    secondary_text,
				    details_text,
				    show_all,
				    varargs);
	va_end (varargs);
	return res;
}

static int
run_question (CommonJob *job,
	      char *primary_text,
	      char *secondary_text,
	      const char *details_text,
	      gboolean show_all,
	      ...)
{
	va_list varargs;
	int res;

	va_start (varargs, show_all);
	res = run_simple_dialog_va (job,
				    FALSE,
				    GTK_MESSAGE_QUESTION,
				    primary_text,
				    secondary_text,
				    details_text,
				    show_all,
				    varargs);
	va_end (varargs);
	return res;
}

static void
inhibit_power_manager (CommonJob *job, const char *message)
{
	job->inhibit_cookie = nemo_inhibit_power_manager (message);
}

static void
abort_job (CommonJob *job)
{
	/* destroy the undo action data too */
	g_clear_object (&job->undo_info);

	g_cancellable_cancel (job->cancellable);
}

static gboolean
job_aborted (CommonJob *job)
{
	return g_cancellable_is_cancelled (job->cancellable);
}

/* Since this happens on a thread we can't use the global prefs object */
static gboolean
should_confirm_move_to_trash (void)
{
	gboolean confirm_move_to_trash;

	confirm_move_to_trash = g_settings_get_boolean (nemo_preferences, NEMO_PREFERENCES_CONFIRM_MOVE_TO_TRASH);

	return confirm_move_to_trash;
}

static gboolean
confirm_move_to_trash (CommonJob *job,
			   GList *files)
{
	char *prompt;
	int file_count;
	int response;

	/* Just Say Yes if the preference says not to confirm. */
	if (!should_confirm_move_to_trash ()) {
		return TRUE;
	}

	file_count = g_list_length (files);
	g_assert (file_count > 0);

    if (file_count == 1) {
        prompt = f (_("Are you sure you want to move \"%B\" "
                      "to the trash?"), files->data);
    } else {
        /* translators: the singular form here can be skipped. */
        prompt = f (ngettext("unused %'d",
                             "Are you sure you want to move "
                             "the %'d selected items to the trash?",
                             file_count),
                    file_count);
    }

	response = run_warning (job,
				prompt,
				f (_("You can restore an item from the trash, if you later change your mind.")),
				NULL,
				FALSE,
				GTK_STOCK_CANCEL, _("Move to _Trash"),
				NULL);

	return (response == 1);
}

static gboolean
should_confirm_trash (void)
{
	gboolean confirm_trash;

	confirm_trash = g_settings_get_boolean (nemo_preferences, NEMO_PREFERENCES_CONFIRM_TRASH);

	return confirm_trash;
}

static gboolean
confirm_delete_from_trash (CommonJob *job,
			   GList *files)
{
	char *prompt;
	int file_count;
	int response;

	/* Just Say Yes if the preference says not to confirm. */
	if (!should_confirm_trash ()) {
		return TRUE;
	}

	file_count = g_list_length (files);
	g_assert (file_count > 0);

	if (file_count == 1) {
		prompt = f (_("Are you sure you want to permanently delete \"%B\" "
					    "from the trash?"), files->data);
	} else {
		prompt = f (ngettext("Are you sure you want to permanently delete "
				     "the %'d selected item from the trash?",
				     "Are you sure you want to permanently delete "
				     "the %'d selected items from the trash?",
				     file_count),
			    file_count);
	}

	response = run_warning (job,
				prompt,
				f (_("If you delete an item, it will be permanently lost.")),
				NULL,
				FALSE,
				GTK_STOCK_CANCEL, GTK_STOCK_DELETE,
				NULL);

	return (response == 1);
}

static gboolean
confirm_empty_trash (CommonJob *job)
{
	char *prompt;
	int response;

	/* Just Say Yes if the preference says not to confirm. */
	if (!should_confirm_trash ()) {
		return TRUE;
	}

	prompt = f (_("Empty all items from Trash?"));

	response = run_warning (job,
				prompt,
				f(_("All items in the Trash will be permanently deleted.")),
				NULL,
				FALSE,
				GTK_STOCK_CANCEL, _("Empty _Trash"),
				NULL);

	return (response == 1);
}

static gboolean
confirm_delete_directly (CommonJob *job,
			 GList *files)
{
	char *prompt;
	int file_count;
	int response;

	/* Just Say Yes if the preference says not to confirm. */
	if (!should_confirm_trash ()) {
		return TRUE;
	}

	file_count = g_list_length (files);
	g_assert (file_count > 0);

	if (can_delete_files_without_confirm (files)) {
		return TRUE;
	}

	if (file_count == 1) {
		prompt = f (_("Are you sure you want to permanently delete \"%B\"?"),
			    files->data);
	} else {
		prompt = f (ngettext("Are you sure you want to permanently delete "
				     "the %'d selected item?",
				     "Are you sure you want to permanently delete "
				     "the %'d selected items?", file_count),
			    file_count);
	}

	response = run_warning (job,
				prompt,
				f (_("If you delete an item, it will be permanently lost.")),
				NULL,
				FALSE,
				GTK_STOCK_CANCEL, GTK_STOCK_DELETE,
				NULL);

	return response == 1;
}

static void
report_delete_progress (CommonJob *job,
			SourceInfo *source_info,
			TransferInfo *transfer_info)
{
	int files_left;
	double elapsed, transfer_rate;
	int remaining_time;
	gint64 now;
	char *files_left_s;

	now = g_get_monotonic_time ();
	if (transfer_info->last_report_time != 0 &&
	    ABS ((gint64)(transfer_info->last_report_time - now)) < PROGRESS_UPDATE_THRESHOLD * US_PER_MS) {
		return;
	}
	transfer_info->last_report_time = now;

	files_left = source_info->num_files - transfer_info->num_files;

	/* Races and whatnot could cause this to be negative... */
	if (files_left < 0) {
		files_left = 1;
	}

	files_left_s = f (ngettext ("%'d file left to delete",
				    "%'d files left to delete",
				    files_left),
			  files_left);

	nemo_progress_info_take_status (job->progress,
					    f (_("Deleting files")));

	elapsed = nemo_progress_info_get_elapsed_time (job->progress);
	if (elapsed < SECONDS_NEEDED_FOR_RELIABLE_TRANSFER_RATE) {
        if (nemo_progress_info_get_is_paused (job->progress)) {
            nemo_progress_info_set_details (job->progress, _("Paused"));
        } else {
            nemo_progress_info_set_details (job->progress, files_left_s);
        }
	} else {
        char *details, *time_left_s;

        if (nemo_progress_info_get_is_paused (job->progress)) {
            time_left_s = g_strdup (_("Paused"));
        } else {
            transfer_rate = transfer_info->num_files / elapsed;
            remaining_time = files_left / transfer_rate;

            /* To translators: %T will expand to a time like "2 minutes".
                 * The singular/plural form will be used depending on the remaining time (i.e. the %T argument).
                 */
            time_left_s = f (ngettext ("%T left",
                           "%T left",
                           seconds_count_format_time_units (remaining_time)),
                     remaining_time);
        }
		details = g_strconcat (files_left_s, "\xE2\x80\x94", time_left_s, NULL);
		nemo_progress_info_take_details (job->progress, details);

		g_free (time_left_s);
	}

	g_free (files_left_s);

	if (source_info->num_files != 0) {
		nemo_progress_info_set_progress (job->progress, transfer_info->num_files, source_info->num_files);
	}
}

static void delete_file (CommonJob *job, GFile *file,
			 gboolean *skipped_file,
			 SourceInfo *source_info,
			 TransferInfo *transfer_info,
			 gboolean toplevel);

static void
delete_dir (CommonJob *job, GFile *dir,
	    gboolean *skipped_file,
	    SourceInfo *source_info,
	    TransferInfo *transfer_info,
	    gboolean toplevel)
{
	GFileInfo *info;
	GError *error;
	GFile *file;
	GFileEnumerator *enumerator;
	char *primary, *secondary, *details;
	int response;
	gboolean skip_error;
	gboolean local_skipped_file;

	local_skipped_file = FALSE;

	skip_error = should_skip_readdir_error (job, dir);
 retry:
	error = NULL;
	enumerator = g_file_enumerate_children (dir,
						G_FILE_ATTRIBUTE_STANDARD_NAME,
						G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
						job->cancellable,
						&error);
	if (enumerator) {
		error = NULL;

		while (!job_aborted (job) &&
		       (info = g_file_enumerator_next_file (enumerator, job->cancellable, skip_error?NULL:&error)) != NULL) {
			file = g_file_get_child (dir,
						 g_file_info_get_name (info));
			delete_file (job, file, &local_skipped_file, source_info, transfer_info, FALSE);
			g_object_unref (file);
			g_object_unref (info);
		}
		g_file_enumerator_close (enumerator, job->cancellable, NULL);
		g_object_unref (enumerator);

		if (error && IS_IO_ERROR (error, CANCELLED)) {
			g_error_free (error);
		} else if (error) {
			primary = f (_("Error while deleting."));
			details = NULL;

			if (IS_IO_ERROR (error, PERMISSION_DENIED)) {
				secondary = f (_("Files in the folder \"%B\" cannot be deleted because you do "
						 "not have permissions to see them."), dir);
			} else {
				secondary = f (_("There was an error getting information about the files in the folder \"%B\"."), dir);
				details = error->message;
			}

			response = run_warning (job,
						primary,
						secondary,
						details,
						FALSE,
						GTK_STOCK_CANCEL, _("_Skip files"),
						NULL);

			g_error_free (error);

			if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
				abort_job (job);
			} else if (response == 1) {
				/* Skip: Do Nothing */
				local_skipped_file = TRUE;
			} else {
				g_assert_not_reached ();
			}
		}

	} else if (IS_IO_ERROR (error, CANCELLED)) {
		g_error_free (error);
	} else {
		primary = f (_("Error while deleting."));
		details = NULL;
		if (IS_IO_ERROR (error, PERMISSION_DENIED)) {
			secondary = f (_("The folder \"%B\" cannot be deleted because you do not have "
					 "permissions to read it."), dir);
		} else {
			secondary = f (_("There was an error reading the folder \"%B\"."), dir);
			details = error->message;
		}

		response = run_warning (job,
					primary,
					secondary,
					details,
					FALSE,
					GTK_STOCK_CANCEL, SKIP, RETRY,
					NULL);

		g_error_free (error);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) {
			/* Skip: Do Nothing  */
			local_skipped_file = TRUE;
		} else if (response == 2) {
			goto retry;
		} else {
			g_assert_not_reached ();
		}
	}

	if (!job_aborted (job) &&
	    /* Don't delete dir if there was a skipped file */
	    !local_skipped_file) {
		if (!file_delete_wrapper (dir, job->cancellable, &error)) {
			if (job->skip_all_error) {
				goto skip;
			}
			primary = f (_("Error while deleting."));
			secondary = f (_("Could not remove the folder %B."), dir);
			details = error->message;

			response = run_warning (job,
						primary,
						secondary,
						details,
						(source_info->num_files - transfer_info->num_files) > 1,
						GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
						NULL);

			if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
				abort_job (job);
			} else if (response == 1) { /* skip all */
				job->skip_all_error = TRUE;
				local_skipped_file = TRUE;
			} else if (response == 2) { /* skip */
				local_skipped_file = TRUE;
			} else {
				g_assert_not_reached ();
			}

		skip:
			g_error_free (error);
		} else {
            gchar *uri = g_file_get_uri (dir);
            if (!eel_uri_is_favorite (uri)) {
                xapp_favorites_remove (xapp_favorites_get_default (), uri);
            }
            g_free (uri);

			nemo_file_changes_queue_file_removed (dir);
			transfer_info->num_files ++;
			report_delete_progress (job, source_info, transfer_info);
			return;
		}
	}

	if (local_skipped_file) {
		*skipped_file = TRUE;
	}
}

static void
delete_file (CommonJob *job, GFile *file,
	     gboolean *skipped_file,
	     SourceInfo *source_info,
	     TransferInfo *transfer_info,
	     gboolean toplevel)
{
	GError *error;
	char *primary, *secondary, *details;
	int response;

	if (should_skip_file (job, file)) {
		*skipped_file = TRUE;
		return;
	}

	error = NULL;
	if (file_delete_wrapper (file, job->cancellable, &error)) {
        gchar *uri = g_file_get_uri (file);
        // We need to remove from favorites explicitly only if we're deleting the file
        // in its native location, otherwise FavoriteVfsFile->file_delete will have
        // already removed it. This is the same with delete_dir and trash_file
        if (!eel_uri_is_favorite (uri)) {
            xapp_favorites_remove (xapp_favorites_get_default (), uri);
        }

        g_free (uri);

		nemo_file_changes_queue_file_removed (file);

		transfer_info->num_files ++;
		report_delete_progress (job, source_info, transfer_info);
		return;
	}

	if (IS_IO_ERROR (error, NOT_EMPTY)) {
		g_error_free (error);
		delete_dir (job, file,
			    skipped_file,
			    source_info, transfer_info,
			    toplevel);
		return;

	} else if (IS_IO_ERROR (error, CANCELLED)) {
		g_error_free (error);

	} else {
		if (job->skip_all_error) {
			goto skip;
		}
		primary = f (_("Error while deleting."));
		secondary = f (_("There was an error deleting %B."), file);
		details = error->message;

		response = run_warning (job,
					primary,
					secondary,
					details,
					(source_info->num_files - transfer_info->num_files) > 1,
					GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
					NULL);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) { /* skip all */
			job->skip_all_error = TRUE;
		} else if (response == 2) { /* skip */
			/* do nothing */
		} else {
			g_assert_not_reached ();
		}
	skip:
		g_error_free (error);
	}

	*skipped_file = TRUE;
}

static void
delete_files (CommonJob *job, GList *files, guint *files_skipped)
{
	GList *l;
	GFile *file;
	SourceInfo source_info;
	TransferInfo transfer_info;
	gboolean skipped_file;

	if (job_aborted (job)) {
		return;
	}

	scan_sources (files,
		      &source_info,
		      job,
		      OP_KIND_DELETE);
	if (job_aborted (job)) {
		return;
	}

	nemo_progress_info_start (job->progress);

	memset (&transfer_info, 0, sizeof (transfer_info));
	report_delete_progress (job, &source_info, &transfer_info);

	for (l = files;
	     l != NULL && !job_aborted (job);
	     l = l->next) {
		file = l->data;

		skipped_file = FALSE;
		delete_file (job, file,
			     &skipped_file,
			     &source_info, &transfer_info,
			     TRUE);
		if (skipped_file) {
			(*files_skipped)++;
		}
	}
}

static void
report_trash_progress (CommonJob *job,
		       int files_trashed,
		       int total_files)
{
	int files_left;
	char *s;

	files_left = total_files - files_trashed;

	nemo_progress_info_take_status (job->progress,
					    f (_("Moving files to trash")));

	s = f (ngettext ("%'d file left to trash",
			 "%'d files left to trash",
			 files_left),
	       files_left);
	nemo_progress_info_take_details (job->progress, s);

	if (total_files != 0) {
		nemo_progress_info_set_progress (job->progress, files_trashed, total_files);
	}
}


static void
trash_files (CommonJob *job, GList *files, guint *files_skipped)
{
	GList *l;
	GFile *file;
	GList *to_delete;
	GError *error;
	int total_files, files_trashed;
	char *primary, *secondary, *details;
	int response;

	if (job_aborted (job)) {
		return;
	}

	total_files = g_list_length (files);
	files_trashed = 0;

	report_trash_progress (job, files_trashed, total_files);

	to_delete = NULL;
	for (l = files;
	     l != NULL && !job_aborted (job);
	     l = l->next) {
		file = l->data;

		error = NULL;

		if (!g_file_trash (file, job->cancellable, &error)) {
			if (job->skip_all_error) {
				(*files_skipped)++;
				goto skip;
			}

			if (job->delete_all) {
				to_delete = g_list_prepend (to_delete, file);
				goto skip;
			}

			primary = f (_("Cannot move file to trash, do you want to delete immediately?"));
			secondary = f (_("The file \"%B\" cannot be moved to the trash."), file);
			details = NULL;
			if (!IS_IO_ERROR (error, NOT_SUPPORTED)) {
				details = error->message;
			}

			response = run_question (job,
						 primary,
						 secondary,
						 details,
						 (total_files - files_trashed) > 1,
						 GTK_STOCK_CANCEL, SKIP_ALL, SKIP, DELETE_ALL, GTK_STOCK_DELETE,
						 NULL);

			if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
				((DeleteJob *) job)->user_cancel = TRUE;
				abort_job (job);
			} else if (response == 1) { /* skip all */
				(*files_skipped)++;
				job->skip_all_error = TRUE;
			} else if (response == 2) { /* skip */
				(*files_skipped)++;
			} else if (response == 3) { /* delete all */
				to_delete = g_list_prepend (to_delete, file);
				job->delete_all = TRUE;
			} else if (response == 4) { /* delete */
				to_delete = g_list_prepend (to_delete, file);
			}

		skip:
			g_error_free (error);
			total_files--;
		} else {
            gchar *uri = g_file_get_uri (file);
            if (!eel_uri_is_favorite (uri)) {
                XAppFavorites *favorites = xapp_favorites_get_default ();
                xapp_favorites_remove (favorites, uri);

                // move-to-trash doesn't recurse, it just trashes the toplevel, and
                // the recent backend (gvfs) takes care of the rest. If we trash a folder
                // that was a favorite, which also had favorites that descended from it,
                // we need to explicitly remove them, or we'll have dangling entries in the
                // favorites list.

                GList *to_remove, *infos, *iter;

                infos = xapp_favorites_get_favorites (favorites, NULL);
                to_remove = NULL;

                for (iter = infos; iter != NULL; iter = iter->next) {
                    XAppFavoriteInfo *info = (XAppFavoriteInfo *) iter->data;

                    if (info->uri && g_str_has_prefix (info->uri, uri)) {
                        to_remove = g_list_prepend (to_remove, g_strdup (info->uri));
                    }
                }

                g_list_free_full (infos, (GDestroyNotify) xapp_favorite_info_free);

                for (iter = to_remove; iter != NULL; iter = iter->next) {
                    xapp_favorites_remove (favorites, (const gchar *) iter->data);
                }

                g_list_free_full (to_remove, g_free);
            }
            g_free (uri);

            nemo_file_changes_queue_file_removed (file);

			if (job->undo_info != NULL) {
				nemo_file_undo_info_trash_add_file (NEMO_FILE_UNDO_INFO_TRASH (job->undo_info), file);
			}

			files_trashed++;
			report_trash_progress (job, files_trashed, total_files);
		}
	}

	if (to_delete) {
		to_delete = g_list_reverse (to_delete);
		delete_files (job, to_delete, files_skipped);
		g_list_free (to_delete);
	}
}

static gboolean
delete_job_done (gpointer user_data)
{
	DeleteJob *job;
	GHashTable *debuting_uris;

	job = user_data;

	g_list_free_full (job->files, g_object_unref);

	if (job->done_callback) {
		debuting_uris = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, NULL);
		job->done_callback (debuting_uris, job->user_cancel, job->done_callback_data);
		g_hash_table_unref (debuting_uris);
	}

	finalize_common ((CommonJob *)job);

	nemo_file_changes_consume_changes (TRUE);

	return FALSE;
}

static gboolean
delete_job (GIOSchedulerJob *io_job,
	    GCancellable *cancellable,
	    gpointer user_data)
{
	DeleteJob *job = user_data;
	GList *to_trash_files;
	GList *to_delete_files;
	GList *l;
	GFile *file;
	gboolean confirmed;
	CommonJob *common;
	gboolean must_confirm_delete_in_trash;
	gboolean must_confirm_delete;
	guint files_skipped;

	common = (CommonJob *)job;
	common->io_job = io_job;

    nemo_progress_info_start (common->progress);

	to_trash_files = NULL;
	to_delete_files = NULL;

	must_confirm_delete_in_trash = FALSE;
	must_confirm_delete = FALSE;
	files_skipped = 0;

	for (l = job->files; l != NULL; l = l->next) {
		file = l->data;

		if (job->try_trash &&
		    g_file_has_uri_scheme (file, "trash")) {
			must_confirm_delete_in_trash = TRUE;
			to_delete_files = g_list_prepend (to_delete_files, file);
		} else if (can_delete_without_confirm (file)) {
			to_delete_files = g_list_prepend (to_delete_files, file);
		} else {
			if (job->try_trash) {
				to_trash_files = g_list_prepend (to_trash_files, file);
			} else {
				must_confirm_delete = TRUE;
				to_delete_files = g_list_prepend (to_delete_files, file);
			}
		}
	}

	if (to_delete_files != NULL && !job_aborted (common)) {
		to_delete_files = g_list_reverse (to_delete_files);
		confirmed = TRUE;
		if (must_confirm_delete_in_trash) {
			confirmed = confirm_delete_from_trash (common, to_delete_files);
		} else if (must_confirm_delete) {
			confirmed = confirm_delete_directly (common, to_delete_files);
		}
		if (confirmed) {
			delete_files (common, to_delete_files, &files_skipped);
		} else {
			job->user_cancel = TRUE;
		}
	}

	if (to_trash_files != NULL && !job_aborted (common)) {
		to_trash_files = g_list_reverse (to_trash_files);
		confirmed = confirm_move_to_trash (common, to_trash_files);
		if (confirmed) {
			trash_files (common, to_trash_files, &files_skipped);
		} else {
			job->user_cancel = TRUE;
			/* destroy the undo action data too */
			g_clear_object (&common->undo_info);
		}
	}

	g_list_free (to_trash_files);
	g_list_free (to_delete_files);

	if (files_skipped == g_list_length (job->files)) {
		/* User has skipped all files, report user cancel */
		job->user_cancel = TRUE;
	}

	g_io_scheduler_job_send_to_mainloop_async (io_job,
						   delete_job_done,
						   job,
						   NULL);



	return FALSE;
}

static void
trash_or_delete_internal (GList                  *files,
			  GtkWindow              *parent_window,
			  gboolean                try_trash,
			  NemoDeleteCallback  done_callback,
			  gpointer                done_callback_data)
{
	DeleteJob *job;

	/* TODO: special case desktop icon link files ... */

	job = op_job_new (DeleteJob, parent_window);
	job->files = eel_g_object_list_copy (files);
	job->try_trash = try_trash;
	job->user_cancel = FALSE;
	job->done_callback = done_callback;
	job->done_callback_data = done_callback_data;

	if (try_trash) {
		inhibit_power_manager ((CommonJob *)job, _("Trashing Files"));
	} else {
		inhibit_power_manager ((CommonJob *)job, _("Deleting Files"));
	}

	if (try_trash && !nemo_file_undo_manager_pop_flag ()) {
		job->common.undo_info = nemo_file_undo_info_trash_new (g_list_length (files));
	}

    generate_initial_job_details (job->common.progress, try_trash ? OP_KIND_TRASH : OP_KIND_DELETE, job->files, NULL);

    add_job_to_job_queue (delete_job, job, job->common.cancellable, job->common.progress,
                                try_trash ? OP_KIND_TRASH : OP_KIND_DELETE);
}

void
nemo_file_operations_trash_or_delete (GList                  *files,
					  GtkWindow              *parent_window,
					  NemoDeleteCallback  done_callback,
					  gpointer                done_callback_data)
{
	g_return_if_fail (files != NULL);

	trash_or_delete_internal (files, parent_window,
				  TRUE,
				  done_callback,  done_callback_data);
}

void
nemo_file_operations_delete (GList                  *files,
				 GtkWindow              *parent_window,
				 NemoDeleteCallback  done_callback,
				 gpointer                done_callback_data)
{
	trash_or_delete_internal (files, parent_window,
				  FALSE,
				  done_callback,  done_callback_data);
}



#include "nemo-mount-operation.h"

typedef struct {
	gboolean eject;
	GMount *mount;
    GMountOperation *mount_operation;
	GtkWindow *parent_window;
	NemoUnmountCallback callback;
	gpointer callback_data;
} UnmountData;

static void
unmount_data_free (UnmountData *data)
{
    if (data->parent_window) {
        g_object_remove_weak_pointer (G_OBJECT (data->parent_window),
                                      (gpointer *) &data->parent_window);
    }

    g_clear_object (&data->mount_operation);
    g_object_unref (data->mount);
    g_free (data);
}

static void
unmount_mount_callback (GObject *source_object,
			GAsyncResult *res,
			gpointer user_data)
{
	UnmountData *data = user_data;
	GError *error;
	char *primary;
	gboolean unmounted;

	error = NULL;
	unmounted = nemo_mount_operation_remove_finish (source_object, res, &error);

	if (! unmounted) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED) &&
		    !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			if (data->eject) {
				primary = f (_("Unable to eject %V"), source_object);
			} else {
				primary = f (_("Unable to unmount %V"), source_object);
			}
			eel_show_error_dialog (primary,
					       error->message,
					       data->parent_window);
			g_free (primary);
		}
	}

	if (data->callback) {
		data->callback (data->callback_data);
	}

	if (error != NULL) {
		g_error_free (error);
	}

	unmount_data_free (data);
}

static void
do_unmount (UnmountData *data)
{
	GMountOperation *mount_op;

	if (data->mount_operation) {
        mount_op = g_object_ref (data->mount_operation);
    } else {
        mount_op = gtk_mount_operation_new (data->parent_window);
    }
	nemo_mount_operation_remove (G_OBJECT (data->mount),
	                             data->eject ? NEMO_MOUNT_REMOVE_EJECT : NEMO_MOUNT_REMOVE_UNMOUNT,
	                             mount_op, NULL, unmount_mount_callback, data);
	g_object_unref (mount_op);
}

static gboolean
dir_has_files (GFile *dir)
{
	GFileEnumerator *enumerator;
	gboolean res;
	GFileInfo *file_info;

	res = FALSE;

	enumerator = g_file_enumerate_children (dir,
						G_FILE_ATTRIBUTE_STANDARD_NAME,
						0,
						NULL, NULL);
	if (enumerator) {
		file_info = g_file_enumerator_next_file (enumerator, NULL, NULL);
		if (file_info != NULL) {
			res = TRUE;
			g_object_unref (file_info);
		}

		g_file_enumerator_close (enumerator, NULL, NULL);
		g_object_unref (enumerator);
	}


	return res;
}

static GList *
get_trash_dirs_for_mount (GMount *mount)
{
	GFile *root;
	GFile *trash;
	char *relpath;
	GList *list;

	root = g_mount_get_root (mount);
	if (root == NULL) {
		return NULL;
	}

	list = NULL;

	if (g_file_is_native (root)) {
		relpath = g_strdup_printf (".Trash/%d", getuid ());
		trash = g_file_resolve_relative_path (root, relpath);
		g_free (relpath);

		list = g_list_prepend (list, g_file_get_child (trash, "files"));
		list = g_list_prepend (list, g_file_get_child (trash, "info"));

		g_object_unref (trash);

		relpath = g_strdup_printf (".Trash-%d", getuid ());
		trash = g_file_get_child (root, relpath);
		g_free (relpath);

		list = g_list_prepend (list, g_file_get_child (trash, "files"));
		list = g_list_prepend (list, g_file_get_child (trash, "info"));

		g_object_unref (trash);
	}

	g_object_unref (root);

	return list;
}

static gboolean
has_trash_files (GMount *mount)
{
	GList *dirs, *l;
	GFile *dir;
	gboolean res;

	dirs = get_trash_dirs_for_mount (mount);

	res = FALSE;

	for (l = dirs; l != NULL; l = l->next) {
		dir = l->data;

		if (dir_has_files (dir)) {
			res = TRUE;
			break;
		}
	}

	g_list_free_full (dirs, g_object_unref);

	return res;
}


static gint
prompt_empty_trash (GtkWindow *parent_window)
{
	gint                    result;
	GtkWidget               *dialog;
	GdkScreen               *screen;

	screen = NULL;
	if (parent_window != NULL) {
		screen = gtk_widget_get_screen (GTK_WIDGET (parent_window));
	}

	/* Do we need to be modal ? */
	dialog = gtk_message_dialog_new (NULL, GTK_DIALOG_MODAL,
					 GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
					 _("Do you want to empty the trash before you unmount?"));
	gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog),
						  _("In order to regain the "
						    "free space on this volume "
						    "the trash must be emptied. "
						    "All trashed items on the volume "
						    "will be permanently lost."));
	gtk_dialog_add_buttons (GTK_DIALOG (dialog),
	                        _("Do _not Empty Trash"), GTK_RESPONSE_REJECT,
	                        GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
	                        _("Empty _Trash"), GTK_RESPONSE_ACCEPT, NULL);
	gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_REJECT);
	gtk_window_set_title (GTK_WINDOW (dialog), ""); /* as per HIG */
	gtk_window_set_skip_taskbar_hint (GTK_WINDOW (dialog), TRUE);
	if (screen) {
		gtk_window_set_screen (GTK_WINDOW (dialog), screen);
	}
	atk_object_set_role (gtk_widget_get_accessible (dialog), ATK_ROLE_ALERT);
	gtk_window_set_wmclass (GTK_WINDOW (dialog), "empty_trash",
				"Nemo");

	/* Make transient for the window group */
	gtk_widget_realize (dialog);
	if (screen != NULL) {
		gdk_window_set_transient_for (gtk_widget_get_window (GTK_WIDGET (dialog)),
				      		gdk_screen_get_root_window (screen));
	}

	result = gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
	return result;
}

static void
empty_trash_for_unmount_done (gboolean success,
			      gpointer user_data)
{
	UnmountData *data = user_data;
	do_unmount (data);
}

void
nemo_file_operations_unmount_mount_full (GtkWindow                      *parent_window,
					     GMount                         *mount,
                         GMountOperation                *mount_operation,
					     gboolean                        eject,
					     gboolean                        check_trash,
					     NemoUnmountCallback         callback,
					     gpointer                        callback_data)
{
	UnmountData *data;
	int response;

	data = g_new0 (UnmountData, 1);
	data->callback = callback;
	data->callback_data = callback_data;
	if (parent_window) {
		data->parent_window = parent_window;
		g_object_add_weak_pointer (G_OBJECT (data->parent_window),
					   (gpointer *) &data->parent_window);

	}

    if (mount_operation) {
        data->mount_operation = g_object_ref (mount_operation);
    }
	data->eject = eject;
	data->mount = g_object_ref (mount);

#ifdef NEMO_SMPL
    /* Removal must not synchronously scan a slow device or start deletion.
     * Trash cleanup remains available as a separate, explicit operation. */
    check_trash = FALSE;
#endif
	if (check_trash && has_trash_files (mount)) {
		response = prompt_empty_trash (parent_window);

		if (response == GTK_RESPONSE_ACCEPT) {
			EmptyTrashJob *job;

			job = op_job_new (EmptyTrashJob, parent_window);
			job->should_confirm = FALSE;
			job->trash_dirs = get_trash_dirs_for_mount (mount);
			job->done_callback = empty_trash_for_unmount_done;
			job->done_callback_data = data;

            generate_initial_job_details (job->common.progress, OP_KIND_EMPTY_TRASH, NULL, NULL);

            add_job_to_job_queue (empty_trash_job, job, job->common.cancellable, job->common.progress,
                                        OP_KIND_EMPTY_TRASH);
			return;
		} else if (response == GTK_RESPONSE_CANCEL) {
			if (callback) {
				callback (callback_data);
			}
			unmount_data_free (data);
			return;
		}
	}

	do_unmount (data);
}

void
nemo_file_operations_unmount_mount (GtkWindow                      *parent_window,
					GMount                         *mount,
					gboolean                        eject,
					gboolean                        check_trash)
{
	nemo_file_operations_unmount_mount_full (parent_window, mount, NULL, eject,
						     check_trash, NULL, NULL);
}

static void
mount_callback_data_notify (gpointer data,
			    GObject *object)
{
	GMountOperation *mount_op;

	mount_op = G_MOUNT_OPERATION (data);
	g_object_set_data (G_OBJECT (mount_op), "mount-callback", NULL);
	g_object_set_data (G_OBJECT (mount_op), "mount-callback-data", NULL);
}

static void
volume_mount_cb (GObject *source_object,
		 GAsyncResult *res,
		 gpointer user_data)
{
	NemoMountCallback mount_callback;
	GObject *mount_callback_data_object;
	GMountOperation *mount_op = user_data;
	GError *error;
	char *primary;
	char *name;
	gboolean success;
	gboolean is_mtp;
	char *id_class;

	success = TRUE;
	error = NULL;
	if (!g_volume_mount_finish (G_VOLUME (source_object), res, &error)) {
		if (error->code != G_IO_ERROR_FAILED_HANDLED &&
                    error->code != G_IO_ERROR_ALREADY_MOUNTED) {
			name = g_volume_get_name (G_VOLUME (source_object));
			primary = g_strdup_printf (_("Unable to mount %s"), name);
			g_free (name);
			success = FALSE;

			id_class = g_volume_get_identifier (G_VOLUME (source_object),
						  G_VOLUME_IDENTIFIER_KIND_CLASS);
			is_mtp = (id_class != NULL && g_strstr_len (id_class, -1, "mtp") != NULL);
			if (!is_mtp) {
				char *vol_name = g_volume_get_name (G_VOLUME (source_object));
				is_mtp = (vol_name != NULL && (
					g_strstr_len (vol_name, -1, "Android") != NULL ||
					g_strstr_len (vol_name, -1, "Phone") != NULL ||
					g_strstr_len (vol_name, -1, "Samsung") != NULL ||
					g_strstr_len (vol_name, -1, "MTP") != NULL));
				g_free (vol_name);
			}
			g_free (id_class);

			if (is_mtp) {
				char *secondary = NULL;
				char *lower = g_ascii_strdown (error->message, -1);
				gboolean needs_unlock = (lower != NULL && (
					g_strstr_len (lower, -1, "locked") != NULL ||
					g_strstr_len (lower, -1, "unauthorized") != NULL ||
					g_strstr_len (lower, -1, "not authorized") != NULL ||
					g_strstr_len (lower, -1, "access denied") != NULL));
				gboolean open_failed = (lower != NULL && (
					g_strstr_len (lower, -1, "unable to open mtp device") != NULL ||
					g_strstr_len (lower, -1, "cannot open mtp device") != NULL ||
					g_strstr_len (lower, -1, "libmtp") != NULL ||
					g_strstr_len (lower, -1, "busy") != NULL));
				gboolean gvfs_mtp_available =
					(g_file_test ("/usr/lib/gvfsd-mtp", G_FILE_TEST_EXISTS) ||
					 g_file_test ("/usr/lib/gvfs/gvfsd-mtp", G_FILE_TEST_EXISTS) ||
					 g_file_test ("/usr/libexec/gvfsd-mtp", G_FILE_TEST_EXISTS) ||
					 g_file_test ("/usr/libexec/gvfs/gvfsd-mtp", G_FILE_TEST_EXISTS));
				gboolean retry_attempted;

				retry_attempted = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (mount_op), "mtp-open-retry-attempted"));
				if (open_failed && !retry_attempted) {
					g_object_set_data (G_OBJECT (mount_op), "mtp-open-retry-attempted", GINT_TO_POINTER (1));
					g_free (lower);
					g_free (primary);
					g_error_free (error);

					/* Keep mount_op alive for the retried async request. */
					g_object_ref (mount_op);
					g_volume_mount (G_VOLUME (source_object), 0, mount_op, NULL, volume_mount_cb, mount_op);
					g_object_unref (mount_op);
					return;
				}
				g_free (lower);

				if (needs_unlock) {
					secondary = g_strdup_printf (
						_ ("%s\n\nUnlock your phone and select \"File Transfer\" (MTP) on the USB options, then try again."),
						error->message);
				} else if (open_failed) {
					secondary = g_strdup_printf (
						_ ("%s\n\nNemo detected your phone, but the MTP session could not be opened.\n\nTry: 1) Unlock the phone and keep it unlocked, 2) Select \"File Transfer\" (MTP), 3) Unplug/replug USB, then retry."),
						error->message);
				} else if (!gvfs_mtp_available) {
					secondary = g_strdup_printf (
						_ ("%s\n\nMTP support requires the GVFS MTP backend. Please install gvfs-mtp (Arch) or gvfs-backends (Debian/Ubuntu) and try again."),
						error->message);
				} else {
					secondary = g_strdup_printf (
						_ ("%s\n\nMTP mount failed. Ensure the phone is unlocked and in \"File Transfer\" mode, then reconnect USB and retry."),
						error->message);
				}

				eel_show_error_dialog (primary, secondary, NULL);
				g_free (secondary);
			} else {
				eel_show_error_dialog (primary,
						       error->message,
						       NULL);
			}
			g_free (primary);
		}
		g_error_free (error);
	}

	mount_callback = (NemoMountCallback)
		g_object_get_data (G_OBJECT (mount_op), "mount-callback");
	mount_callback_data_object =
		g_object_get_data (G_OBJECT (mount_op), "mount-callback-data");

	if (mount_callback != NULL) {
		(* mount_callback) (G_VOLUME (source_object),
				    success,
				    mount_callback_data_object);

	    	if (mount_callback_data_object != NULL) {
			g_object_weak_unref (mount_callback_data_object,
					     mount_callback_data_notify,
					     mount_op);
		}
	}

	g_object_unref (mount_op);
}


void
nemo_file_operations_mount_volume (GtkWindow *parent_window,
				       GVolume *volume)
{
	nemo_file_operations_mount_volume_full (parent_window, volume,
						    NULL, NULL);
}

void
nemo_file_operations_mount_volume_full (GtkWindow *parent_window,
					    GVolume *volume,
					    NemoMountCallback mount_callback,
					    GObject *mount_callback_data_object)
{
	GMountOperation *mount_op;

	mount_op = gtk_mount_operation_new (parent_window);
	g_mount_operation_set_password_save (mount_op, G_PASSWORD_SAVE_FOR_SESSION);
	g_object_set_data (G_OBJECT (mount_op),
			   "mount-callback",
			   mount_callback);

	if (mount_callback != NULL &&
	    mount_callback_data_object != NULL) {
		g_object_weak_ref (mount_callback_data_object,
				   mount_callback_data_notify,
				   mount_op);
	}
	g_object_set_data (G_OBJECT (mount_op),
			   "mount-callback-data",
			   mount_callback_data_object);

	g_volume_mount (volume, 0, mount_op, NULL, volume_mount_cb, mount_op);
}

static void
report_count_progress (CommonJob *job,
		       SourceInfo *source_info)
{
	char *s, *details;

	switch (source_info->op) {

    case OP_KIND_LINK:  /* FIXME */
    case OP_KIND_CREATE:
    case OP_KIND_PERMISSIONS:
    case OP_KIND_TRUST:
	default:
    case OP_KIND_DUPE:
	case OP_KIND_COPY:
		s = f (ngettext("Preparing to copy %'d file (%S)",
		                "Preparing to copy %'d files (%S)",
		                source_info->num_files),
		       source_info->num_files, source_info->num_bytes);
		break;
	case OP_KIND_MOVE:
		s = f (ngettext("Preparing to move %'d file (%S)",
		                "Preparing to move %'d files (%S)",
		                source_info->num_files),
		       source_info->num_files, source_info->num_bytes);
		break;
	case OP_KIND_DELETE:
		s = f (ngettext("Preparing to delete %'d file (%S)",
		                "Preparing to delete %'d files (%S)",
		                source_info->num_files),
		       source_info->num_files, source_info->num_bytes);
		break;
	case OP_KIND_TRASH:
    case OP_KIND_EMPTY_TRASH:
		s = f (ngettext("Preparing to trash %'d file",
		                "Preparing to trash %'d files",
		                source_info->num_files),
		       source_info->num_files);
		break;
	}

    if (nemo_progress_info_get_is_paused (job->progress)) {
        details = g_strconcat (s, "\xE2\x80\x94", _("Paused"), NULL);
        g_free (s);
    } else {
        details = s;
    }

	nemo_progress_info_take_details (job->progress, details);
	nemo_progress_info_pulse_progress (job->progress);
}

static void
count_file (GFileInfo *info,
	    CommonJob *job,
	    SourceInfo *source_info)
{
	source_info->num_files += 1;
	source_info->num_bytes += g_file_info_get_size (info);

	if (source_info->num_files_since_progress++ > 100) {
		report_count_progress (job, source_info);
		source_info->num_files_since_progress = 0;
	}
}

static char *
get_scan_primary (OpKind kind)
{
	switch (kind) {
    case OP_KIND_PERMISSIONS:  /* FIXME */
    case OP_KIND_LINK:
    case OP_KIND_CREATE:
    case OP_KIND_TRUST:
	default:
	case OP_KIND_COPY:
    case OP_KIND_DUPE:
		return f (_("Error while copying."));
	case OP_KIND_MOVE:
		return f (_("Error while moving."));
	case OP_KIND_DELETE:
		return f (_("Error while deleting."));
	case OP_KIND_TRASH:
    case OP_KIND_EMPTY_TRASH:
		return f (_("Error while moving files to trash."));
	}
}

static void
scan_dir (GFile *dir,
	  SourceInfo *source_info,
	  CommonJob *job,
	  GQueue *dirs)
{
	GFileInfo *info;
	GError *error;
	GFile *subdir;
	GFileEnumerator *enumerator;
	char *primary, *secondary, *details;
	int response;
	SourceInfo saved_info;

	saved_info = *source_info;

 retry:
	error = NULL;
	enumerator = g_file_enumerate_children (dir,
						G_FILE_ATTRIBUTE_STANDARD_NAME","
						G_FILE_ATTRIBUTE_STANDARD_TYPE","
						G_FILE_ATTRIBUTE_STANDARD_SIZE,
						G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
						job->cancellable,
						&error);
	if (enumerator) {
		error = NULL;
		while ((info = g_file_enumerator_next_file (enumerator, job->cancellable, &error)) != NULL) {
			count_file (info, job, source_info);

			if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
				subdir = g_file_get_child (dir,
							   g_file_info_get_name (info));

				/* Push to head, since we want depth-first */
				g_queue_push_head (dirs, subdir);
			}

			g_object_unref (info);
		}
		g_file_enumerator_close (enumerator, job->cancellable, NULL);
		g_object_unref (enumerator);

		if (error && IS_IO_ERROR (error, CANCELLED)) {
			g_error_free (error);
		} else if (error) {
			primary = get_scan_primary (source_info->op);
			details = NULL;

			if (IS_IO_ERROR (error, PERMISSION_DENIED)) {
				secondary = f (_("Files in the folder \"%B\" cannot be handled because you do "
						 "not have permissions to see them."), dir);
			} else {
				secondary = f (_("There was an error getting information about the files in the folder \"%B\"."), dir);
				details = error->message;
			}

			response = run_warning (job,
						primary,
						secondary,
						details,
						FALSE,
						GTK_STOCK_CANCEL, RETRY, SKIP,
						NULL);

			g_error_free (error);

			if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
				abort_job (job);
			} else if (response == 1) {
				*source_info = saved_info;
				goto retry;
			} else if (response == 2) {
				skip_readdir_error (job, dir);
			} else {
				g_assert_not_reached ();
			}
		}

	} else if (job->skip_all_error) {
		g_error_free (error);
		skip_file (job, dir);
	} else if (IS_IO_ERROR (error, CANCELLED)) {
		g_error_free (error);
	} else {
		primary = get_scan_primary (source_info->op);
		details = NULL;

		if (IS_IO_ERROR (error, PERMISSION_DENIED)) {
			secondary = f (_("The folder \"%B\" cannot be handled because you do not have "
					 "permissions to read it."), dir);
		} else {
			secondary = f (_("There was an error reading the folder \"%B\"."), dir);
			details = error->message;
		}
		/* set show_all to TRUE here, as we don't know how many
		 * files we'll end up processing yet.
		 */
		response = run_warning (job,
					primary,
					secondary,
					details,
					TRUE,
					GTK_STOCK_CANCEL, SKIP_ALL, SKIP, RETRY,
					NULL);

		g_error_free (error);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1 || response == 2) {
			if (response == 1) {
				job->skip_all_error = TRUE;
			}
			skip_file (job, dir);
		} else if (response == 3) {
			goto retry;
		} else {
			g_assert_not_reached ();
		}
	}
}

static void
scan_file (GFile *file,
	   SourceInfo *source_info,
	   CommonJob *job)
{
	GFileInfo *info;
	GError *error;
	GQueue *dirs;
	GFile *dir;
	char *primary;
	char *secondary;
	char *details;
	int response;

	dirs = g_queue_new ();

 retry:
	error = NULL;
	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_STANDARD_TYPE","
				  G_FILE_ATTRIBUTE_STANDARD_SIZE,
				  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				  job->cancellable,
				  &error);

	if (info) {
		count_file (info, job, source_info);

    		/* trashing operation doesn't recurse */
    		if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY &&
        		source_info->op != OP_KIND_TRASH)
     		{
			g_queue_push_head (dirs, g_object_ref (file));
		}

		g_object_unref (info);
	} else if (job->skip_all_error) {
		g_error_free (error);
		skip_file (job, file);
	} else if (IS_IO_ERROR (error, CANCELLED)) {
		g_error_free (error);
	} else {
		primary = get_scan_primary (source_info->op);
		details = NULL;

		if (IS_IO_ERROR (error, PERMISSION_DENIED)) {
			secondary = f (_("The file \"%B\" cannot be handled because you do not have "
					 "permissions to read it."), file);
		} else {
			secondary = f (_("There was an error getting information about \"%B\"."), file);
			details = error->message;
		}
		/* set show_all to TRUE here, as we don't know how many
		 * files we'll end up processing yet.
		 */
		response = run_warning (job,
					primary,
					secondary,
					details,
					TRUE,
					GTK_STOCK_CANCEL, SKIP_ALL, SKIP, RETRY,
					NULL);

		g_error_free (error);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1 || response == 2) {
			if (response == 1) {
				job->skip_all_error = TRUE;
			}
			skip_file (job, file);
		} else if (response == 3) {
			goto retry;
		} else {
			g_assert_not_reached ();
		}
	}

	while (!job_aborted (job) &&
	       (dir = g_queue_pop_head (dirs)) != NULL) {
		scan_dir (dir, source_info, job, dirs);
		g_object_unref (dir);
	}

	/* Free all from queue if we exited early */
	g_queue_foreach (dirs, (GFunc)g_object_unref, NULL);
	g_queue_free (dirs);
}

static void
scan_sources (GList *files,
	      SourceInfo *source_info,
	      CommonJob *job,
	      OpKind kind)
{
	GList *l;
	GFile *file;

	memset (source_info, 0, sizeof (SourceInfo));
	source_info->op = kind;

	report_count_progress (job, source_info);

	for (l = files; l != NULL && !job_aborted (job); l = l->next) {
		file = l->data;

		scan_file (file,
			   source_info,
			   job);
	}

	/* Make sure we report the final count */
	report_count_progress (job, source_info);
}

static void
verify_destination (CommonJob *job,
		    GFile *dest,
		    char **dest_fs_id,
		    goffset required_size)
{
	GFileInfo *info, *fsinfo;
	GError *error;
	guint64 free_size;
	guint64 size_difference;
	char *primary, *secondary, *details;
	int response;
	GFileType file_type;

	if (dest_fs_id) {
		*dest_fs_id = NULL;
	}

 retry:

	error = NULL;
	info = g_file_query_info (dest,
				  G_FILE_ATTRIBUTE_STANDARD_TYPE","
				  G_FILE_ATTRIBUTE_ID_FILESYSTEM,
				  0,
				  job->cancellable,
				  &error);

	if (info == NULL) {
		if (IS_IO_ERROR (error, CANCELLED)) {
			g_error_free (error);
			return;
		}

		primary = f (_("Error while copying to \"%B\"."), dest);
		details = NULL;

		if (IS_IO_ERROR (error, PERMISSION_DENIED)) {
			secondary = f (_("You do not have permissions to access the destination folder."));
		} else {
			secondary = f (_("There was an error getting information about the destination."));
			details = error->message;
		}

		response = run_error (job,
				      primary,
				      secondary,
				      details,
				      FALSE,
				      GTK_STOCK_CANCEL, RETRY,
				      NULL);

		g_error_free (error);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) {
			goto retry;
		} else {
			g_assert_not_reached ();
		}

		return;
	}

	file_type = g_file_info_get_file_type (info);

	if (dest_fs_id) {
		*dest_fs_id =
			g_strdup (g_file_info_get_attribute_string (info,
								    G_FILE_ATTRIBUTE_ID_FILESYSTEM));
	}

	g_object_unref (info);

	if (file_type != G_FILE_TYPE_DIRECTORY) {
		primary = f (_("Error while copying to \"%B\"."), dest);
		secondary = f (_("The destination is not a folder."));

		response = run_error (job,
				      primary,
				      secondary,
				      NULL,
				      FALSE,
				      GTK_STOCK_CANCEL,
				      NULL);

		abort_job (job);
		return;
	}

	fsinfo = g_file_query_filesystem_info (dest,
					       G_FILE_ATTRIBUTE_FILESYSTEM_FREE","
					       G_FILE_ATTRIBUTE_FILESYSTEM_READONLY,
					       job->cancellable,
					       NULL);
	if (fsinfo == NULL) {
		/* All sorts of things can go wrong getting the fs info (like not supported)
		 * only check these things if the fs returns them
		 */
		return;
	}

	if (required_size > 0 &&
	    g_file_info_has_attribute (fsinfo, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
		free_size = g_file_info_get_attribute_uint64 (fsinfo,
							      G_FILE_ATTRIBUTE_FILESYSTEM_FREE);

		if (free_size < required_size) {
			size_difference = required_size - free_size;
			primary = f (_("Error while copying to \"%B\"."), dest);
			secondary = f (_("There is not enough space on the destination. Try to remove files to make space."));

			details = f (_("%S more space is required to copy to the destination."), size_difference);

			response = run_warning (job,
						primary,
						secondary,
						details,
						FALSE,
						GTK_STOCK_CANCEL,
						COPY_FORCE,
						RETRY,
						NULL);

			if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
				abort_job (job);
			} else if (response == 2) {
				goto retry;
			} else if (response == 1) {
				/* We are forced to copy - just fall through ... */
			} else {
				g_assert_not_reached ();
			}
		}
	}

	if (!job_aborted (job) &&
	    g_file_info_get_attribute_boolean (fsinfo,
					       G_FILE_ATTRIBUTE_FILESYSTEM_READONLY)) {
		primary = f (_("Error while copying to \"%B\"."), dest);
		secondary = f (_("The destination is read-only."));

		response = run_error (job,
				      primary,
				      secondary,
				      NULL,
				      FALSE,
				      GTK_STOCK_CANCEL,
				      NULL);

		g_error_free (error);

		abort_job (job);
	}

	g_object_unref (fsinfo);
}

static void
report_copy_progress (CopyMoveJob *copy_job,
		      SourceInfo *source_info,
		      TransferInfo *transfer_info)
{
	int files_left;
	goffset total_size;
	double elapsed, transfer_rate;
	int remaining_time;
	guint64 now;
	CommonJob *job;
	gboolean is_move;

	job = (CommonJob *)copy_job;

	is_move = copy_job->is_move;

	now = g_get_monotonic_time ();

	if (transfer_info->last_report_time != 0 &&
	    ABS ((gint64)(transfer_info->last_report_time - now)) < PROGRESS_UPDATE_THRESHOLD * US_PER_MS) {
		return;
	}
	transfer_info->last_report_time = now;

	files_left = source_info->num_files - transfer_info->num_files;

	/* Races and whatnot could cause this to be negative... */
	if (files_left < 0) {
		files_left = 1;
	}

	if (files_left != transfer_info->last_reported_files_left ||
	    transfer_info->last_reported_files_left == 0) {
		/* Avoid changing this unless files_left changed since last time */
		transfer_info->last_reported_files_left = files_left;

		if (source_info->num_files == 1) {
			if (copy_job->destination != NULL) {
				nemo_progress_info_take_status (job->progress,
								    f (is_move ?
								       _("Moving \"%B\" to \"%B\""):
								       _("Copying \"%B\" to \"%B\""),
								       copy_job->fake_display_source != NULL ?
								       copy_job->fake_display_source :
								       (GFile *)copy_job->files->data,
								       copy_job->destination));
			} else {
				nemo_progress_info_take_status (job->progress,
								    f (_("Duplicating \"%B\""),
								       (GFile *)copy_job->files->data));
			}
		} else if (copy_job->files != NULL &&
			   copy_job->files->next == NULL) {
			if (copy_job->destination != NULL) {
				nemo_progress_info_take_status (job->progress,
								    f (is_move ?
								       _("Moving file %'d of %'d (in \"%B\") to \"%B\"")
								       :
								       _("Copying file %'d of %'d (in \"%B\") to \"%B\""),
								       transfer_info->num_files + 1,
								       source_info->num_files,
								       (GFile *)copy_job->files->data,
								       copy_job->destination));
			} else {
				nemo_progress_info_take_status (job->progress,
								    f (_("Duplicating file %'d of %'d (in \"%B\")"),
								       transfer_info->num_files + 1,
								       source_info->num_files,
								       (GFile *)copy_job->files->data));
			}
		} else {
			if (copy_job->destination != NULL) {
				nemo_progress_info_take_status (job->progress,
								    f (is_move ?
								       _("Moving file %'d of %'d to \"%B\"")
								       :
								       _ ("Copying file %'d of %'d to \"%B\""),
								       transfer_info->num_files + 1,
								       source_info->num_files,
								       copy_job->destination));
			} else {
				nemo_progress_info_take_status (job->progress,
								    f (_("Duplicating file %'d of %'d"),
								       transfer_info->num_files + 1,
								       source_info->num_files));
			}
		}
	}

	total_size = MAX (source_info->num_bytes, transfer_info->num_bytes);

	elapsed = nemo_progress_info_get_elapsed_time (job->progress);
	transfer_rate = 0;
	if (elapsed > 0) {
		transfer_rate = transfer_info->num_bytes / elapsed;
	}

	if (elapsed < SECONDS_NEEDED_FOR_RELIABLE_TRANSFER_RATE &&
	    transfer_rate > 0) {
		char *s;

        if (nemo_progress_info_get_is_paused (job->progress)) {
            s = g_strdup (_("Paused"));
        } else {
            /* To translators: %S will expand to a size like "2 bytes" or "3 MB", so something like "4 kb of 4 MB" */
            s = f (_("%S of %S"), transfer_info->num_bytes, total_size);
        }

        nemo_progress_info_take_details (job->progress, s);
	} else {
        if (nemo_progress_info_get_is_paused (job->progress)) {
            nemo_progress_info_take_details (job->progress, g_strdup (_("Paused")));
        } else {
            char *s;
            remaining_time = transfer_rate > 0 ?
                (total_size - transfer_info->num_bytes) / transfer_rate : 0;

            /* To translators: %S will expand to a size like "2 bytes" or "3 MB", %T to a time duration like
             * "2 minutes". So the whole thing will be something like "2 kb of 4 MB -- 2 hours left (4kb/sec)"
             *
             * The singular/plural form will be used depending on the remaining time (i.e. the %T argument).
             */
            s = f (ngettext ("%S of %S \xE2\x80\x94 %T left (%S/sec)",
                     "%S of %S \xE2\x80\x94 %T left (%S/sec)",
                     seconds_count_format_time_units (remaining_time)),
                   transfer_info->num_bytes, total_size,
                   remaining_time,
                   (goffset)transfer_rate);
            nemo_progress_info_take_details (job->progress, s);
        }
    }

	nemo_progress_info_set_progress (job->progress, transfer_info->num_bytes, total_size);
}

static int
get_max_name_length (GFile *file_dir)
{
	int max_length;
	char *dir;
	long max_path;
	long max_name;

	max_length = -1;

	if (!g_file_has_uri_scheme (file_dir, "file"))
		return max_length;

	dir = g_file_get_path (file_dir);
	if (!dir)
		return max_length;

	max_path = pathconf (dir, _PC_PATH_MAX);
	max_name = pathconf (dir, _PC_NAME_MAX);

	if (max_name == -1 && max_path == -1) {
		max_length = -1;
	} else if (max_name == -1 && max_path != -1) {
		max_length = max_path - (strlen (dir) + 1);
	} else if (max_name != -1 && max_path == -1) {
		max_length = max_name;
	} else {
		int leftover;

		leftover = max_path - (strlen (dir) + 1);

		max_length = MIN (leftover, max_name);
	}

	g_free (dir);

	return max_length;
}

#define FAT_FORBIDDEN_CHARACTERS "/:*?\"<>\\|"

static gboolean
str_replace (char *str,
	     char replacement)
{
	gboolean success;
	int i;

	success = FALSE;
	for (i = 0; str[i] != '\0'; i++) {
		if (strchr (FAT_FORBIDDEN_CHARACTERS, str[i]) ||
			str[i] < 32) {
			success = TRUE;
			str[i] = replacement;
		}
	}

	return success;
}

static gboolean
make_file_name_valid_for_dest_fs (char *filename,
				 const char *dest_fs_type)
{
	if (dest_fs_type != NULL && filename != NULL) {
		if (!strcmp (dest_fs_type, "fat")  ||
		    !strcmp (dest_fs_type, "vfat") ||
            /* The fuseblk filesystem type could be of any type
             * in theory, but in practice is usually NTFS or exFAT.
             * This assumption is a pragmatic way to solve
             * https://gitlab.gnome.org/GNOME/nautilus/-/issues/1343 */
		    !strcmp (dest_fs_type, "fuse") ||
		    !strcmp (dest_fs_type, "exfat") ||
		    !strcmp (dest_fs_type, "ntfs") ||
		    !strcmp (dest_fs_type, "msdos") ||
		    !strcmp (dest_fs_type, "msdosfs")) {
			gboolean ret;
			guint i, old_len;

			ret = str_replace (filename, '_');

			old_len = strlen (filename);
			for (i = 0; i < old_len; i++) {
				if (filename[i] != ' ') {
					g_strchomp (filename);
					ret |= (old_len != strlen (filename));
					break;
				}
			}

			return ret;
		}
	}

	return FALSE;
}

static GFile *
get_unique_target_file (GFile *src,
			GFile *dest_dir,
			gboolean same_fs,
			const char *dest_fs_type,
			int count,
			GCancellable *cancellable)
{
	const char *editname, *end;
	char *basename, *new_name;
	GFileInfo *info;
	GFile *dest;
	int max_length;

	max_length = get_max_name_length (dest_dir);

	dest = NULL;
	info = g_file_query_info (src,
				  G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME,
				  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
	if (info != NULL) {
		editname = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME);

		if (editname != NULL) {
			new_name = get_duplicate_name (editname, count, max_length);
			make_file_name_valid_for_dest_fs (new_name, dest_fs_type);
			dest = g_file_get_child_for_display_name (dest_dir, new_name, NULL);
			g_free (new_name);
		}

		g_object_unref (info);
	}

	if (dest == NULL) {
		basename = g_file_get_basename (src);

		if (g_utf8_validate (basename, -1, NULL)) {
			new_name = get_duplicate_name (basename, count, max_length);
			make_file_name_valid_for_dest_fs (new_name, dest_fs_type);
			dest = g_file_get_child_for_display_name (dest_dir, new_name, NULL);
			g_free (new_name);
		}

		if (dest == NULL) {
			end = strrchr (basename, '.');
			if (end != NULL) {
				count += atoi (end + 1);
			}
			new_name = g_strdup_printf ("%s.%d", basename, count);
			make_file_name_valid_for_dest_fs (new_name, dest_fs_type);
			dest = g_file_get_child (dest_dir, new_name);
			g_free (new_name);
		}

		g_free (basename);
	}

	return dest;
}

static GFile *
get_target_file_for_link (GFile *src,
			  GFile *dest_dir,
			  const char *dest_fs_type,
			  int count,
			  GCancellable *cancellable)
{
	const char *editname;
	char *basename, *new_name;
	GFileInfo *info;
	GFile *dest;
	int max_length;

	max_length = get_max_name_length (dest_dir);

	dest = NULL;
	info = g_file_query_info (src,
				  G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME,
				  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
	if (info != NULL) {
		editname = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_EDIT_NAME);

		if (editname != NULL) {
			new_name = get_link_name (editname, count, max_length);
			make_file_name_valid_for_dest_fs (new_name, dest_fs_type);
			dest = g_file_get_child_for_display_name (dest_dir, new_name, NULL);
			g_free (new_name);
		}

		g_object_unref (info);
	}

	if (dest == NULL) {
		basename = g_file_get_basename (src);
		make_file_name_valid_for_dest_fs (basename, dest_fs_type);

		if (g_utf8_validate (basename, -1, NULL)) {
			new_name = get_link_name (basename, count, max_length);
			make_file_name_valid_for_dest_fs (new_name, dest_fs_type);
			dest = g_file_get_child_for_display_name (dest_dir, new_name, NULL);
			g_free (new_name);
		}

		if (dest == NULL) {
			if (count == 1) {
				new_name = g_strdup_printf ("%s.lnk", basename);
			} else {
				new_name = g_strdup_printf ("%s.lnk%d", basename, count);
			}
			make_file_name_valid_for_dest_fs (new_name, dest_fs_type);
			dest = g_file_get_child (dest_dir, new_name);
			g_free (new_name);
		}

		g_free (basename);
	}

	return dest;
}

static GFile *
get_target_file_with_custom_name (GFile *src,
				  GFile *dest_dir,
				  const char *dest_fs_type,
				  gboolean same_fs,
				  const gchar *custom_name,
				  GCancellable *cancellable)
{
	char *basename;
	GFile *dest;
	GFileInfo *info;
	char *copyname;

	dest = NULL;

	if (custom_name != NULL) {
		copyname = g_strdup (custom_name);
		make_file_name_valid_for_dest_fs (copyname, dest_fs_type);
		dest = g_file_get_child_for_display_name (dest_dir, copyname, NULL);

		g_free (copyname);
	}

	if (dest == NULL && !same_fs) {
		info = g_file_query_info (src,
					  G_FILE_ATTRIBUTE_STANDARD_COPY_NAME ","
					  G_FILE_ATTRIBUTE_TRASH_ORIG_PATH,
					  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);

		if (info) {
			copyname = NULL;

			/* if file is being restored from trash make sure it uses its original name */
			if (g_file_has_uri_scheme (src, "trash")) {
				copyname = g_path_get_basename (g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_TRASH_ORIG_PATH));
			}

			if (copyname == NULL) {
				copyname = g_strdup (g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_COPY_NAME));
			}

			if (copyname) {
				make_file_name_valid_for_dest_fs (copyname, dest_fs_type);
				dest = g_file_get_child_for_display_name (dest_dir, copyname, NULL);
				g_free (copyname);
			}

			g_object_unref (info);
		}
	}

	if (dest == NULL) {
		basename = g_file_get_basename (src);
		make_file_name_valid_for_dest_fs (basename, dest_fs_type);
		dest = g_file_get_child (dest_dir, basename);
		g_free (basename);
	}

	return dest;
}

static GFile *
get_target_file (GFile *src,
		 GFile *dest_dir,
		 const char *dest_fs_type,
		 gboolean same_fs,
		 GCancellable *cancellable)
{
	return get_target_file_with_custom_name (src, dest_dir, dest_fs_type, same_fs, NULL, cancellable);
}

static gboolean
has_fs_id (GFile *file, const char *fs_id, GCancellable *cancellable)
{
	const char *id;
	GFileInfo *info;
	gboolean res;

	res = FALSE;
	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_ID_FILESYSTEM,
				  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				  cancellable, NULL);

	if (info) {
		id = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_ID_FILESYSTEM);

		if (id && strcmp (id, fs_id) == 0) {
			res = TRUE;
		}

		g_object_unref (info);
	}

	return res;
}

#ifndef NEMO_SMPL
static gboolean
is_dir (GFile *file, GCancellable *cancellable)
{
	GFileInfo *info;
	gboolean res;

	res = FALSE;
	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_STANDARD_TYPE,
				  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				  cancellable, NULL);
	if (info) {
		res = g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY;
		g_object_unref (info);
	}

	return res;
}
#endif

static void copy_move_file (CopyMoveJob *job,
			    GFile *src,
			    GFile *dest_dir,
			    gboolean same_fs,
			    gboolean unique_names,
			    GFile *precomputed_dest,
			    char **dest_fs_type,
			    SourceInfo *source_info,
			    TransferInfo *transfer_info,
			    GHashTable *debuting_files,
			    GdkPoint *point,
			    gboolean overwrite,
			    gboolean *skipped_file,
			    gboolean readonly_source_fs);

#ifdef NEMO_SMPL
static void
remember_incomplete_copy (CopyMoveJob *job, GFile *file, gboolean failed)
{
	gpointer previous;

	job->had_errors = TRUE;
	if (job->incomplete_paths == NULL) {
		job->incomplete_paths = g_hash_table_new_full (
			g_file_hash, (GEqualFunc) g_file_equal, g_object_unref, NULL);
	}
	previous = g_hash_table_lookup (job->incomplete_paths, file);
	/* A later skip must not hide an already recorded error on the same path. */
	g_hash_table_replace (job->incomplete_paths, g_object_ref (file),
	                      GINT_TO_POINTER (failed || GPOINTER_TO_INT (previous) == 2 ? 2 :
	                                       previous != NULL ? GPOINTER_TO_INT (previous) : 1));
}

static void
record_incomplete_copy (CopyMoveJob *job, GFile *file, gboolean failed)
{
	job->had_errors = TRUE;
	if (!job_aborted (&job->common)) {
		remember_incomplete_copy (job, file, failed);
	}
}

static gboolean
transfer_preflight (CopyMoveJob *job)
{
	GError *error = NULL;
	if (job->target_name != NULL &&
	    (job->target_name[0] == '\0' || strchr (job->target_name, '/') != NULL ||
	     g_str_equal (job->target_name, ".") || g_str_equal (job->target_name, ".."))) {
		g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
		                     _("The new name must be a single nonempty filename."));
	} else if (nemo_transfer_guard_check (job->transfer_guard, &error)) {
		return TRUE;
	}
	for (GList *l = job->files; l != NULL; l = l->next) {
		record_incomplete_copy (job, l->data, TRUE);
	}
	g_autofree char *from = g_file_get_parse_name (job->files->data);
	g_autofree char *to = job->destination ? g_file_get_parse_name (job->destination) : NULL;
	nemo_progress_info_take_completion_details (job->common.progress,
		g_strdup_printf (_("From: %s\nTo: %s\nThe transfer was not started. "
		                   "All source files were retained.\n%s"),
		                 from, to ? to : from, error->message));
	run_warning (&job->common, g_strdup (_("The transfer cannot be completed safely.")),
	             g_strdup (_("All source files were retained. No destination was replaced.")),
	             error->message, FALSE, GTK_STOCK_OK, NULL);
	g_error_free (error);
	return FALSE;
}

static gboolean
copy_subtree_only_retained (CopyMoveJob *job, GFile *directory)
{
	GHashTableIter iter;
	gpointer file, state;
	gboolean found = FALSE;

	if (job->incomplete_paths == NULL) {
		return FALSE;
	}
	g_hash_table_iter_init (&iter, job->incomplete_paths);
	while (g_hash_table_iter_next (&iter, &file, &state)) {
		if (g_file_equal (file, directory) || g_file_has_prefix (file, directory)) {
			if (GPOINTER_TO_INT (state) != 3) {
				return FALSE;
			}
			found = TRUE;
		}
	}
	return found;
}

static void
set_copy_move_result (CopyMoveJob *job)
{
	GHashTableIter iter;
	gpointer file, state;
	GHashTable *scan_skips[] = { job->common.skip_files, job->common.skip_readdir_error };
	if (job->transfer_guard != NULL) {
		g_autofree char *recovery = nemo_transfer_guard_take_details (job->transfer_guard);
		if (recovery[0] != '\0' || job->metadata_limitations > 0) {
			g_autofree char *from = g_file_get_parse_name (job->files->data);
			g_autofree char *to = job->destination ? g_file_get_parse_name (job->destination) : NULL;
			g_autofree char *metadata = job->metadata_limitations ?
				g_strdup_printf (ngettext ("\nOptional file metadata could not be fully preserved for %u entry.",
				                           "\nOptional file metadata could not be fully preserved for %u entries.",
				                           job->metadata_limitations), job->metadata_limitations) : g_strdup ("");
			nemo_progress_info_take_completion_details (job->common.progress,
				g_strdup_printf (_("From: %s\nTo: %s\n%s%s"), from, to ? to : from, recovery, metadata));
		}
	}

	/* Some pre-scan failures suppress entire subtrees before the copy worker
	 * visits them. Audit both tables, not just paths reached by that worker. */
	for (guint i = 0; i < G_N_ELEMENTS (scan_skips); i++) {
		if (scan_skips[i] == NULL) {
			continue;
		}
		g_hash_table_iter_init (&iter, scan_skips[i]);
		while (g_hash_table_iter_next (&iter, &file, NULL)) {
			remember_incomplete_copy (job, file, TRUE);
		}
	}
	if (job->incomplete_paths != NULL) {
		g_hash_table_iter_init (&iter, job->incomplete_paths);
		while (g_hash_table_iter_next (&iter, NULL, &state)) {
			if (GPOINTER_TO_INT (state) == 2) {
				job->result.failed_items++;
			} else if (GPOINTER_TO_INT (state) == 3) {
				job->result.unverified_retained_files++;
			} else {
				job->result.skipped_items++;
			}
		}
	}
	job->result.operation = job->is_move ? NEMO_PROGRESS_OPERATION_MOVE : NEMO_PROGRESS_OPERATION_COPY;
	job->result.verification_requested = job->verify_after_copy;
	if (job_aborted (&job->common)) {
		job->result.outcome = NEMO_PROGRESS_OUTCOME_CANCELLED;
	} else if (!job->had_errors) {
		job->result.outcome = NEMO_PROGRESS_OUTCOME_SUCCESS;
	} else if (!job->is_move && !job->verify_after_copy &&
	           job->result.unverified_retained_files > 0 &&
	           job->result.skipped_items == 0 && job->result.failed_items == 0) {
		job->result.outcome = NEMO_PROGRESS_OUTCOME_RETAINED;
	} else if (job->result.completed_items > 0 ||
	           job->result.existing_verified_regular_files > 0 ||
	           job->result.existing_verified_symlinks > 0 ||
	           (job->result.skipped_items > 0 && job->result.failed_items == 0)) {
		job->result.outcome = NEMO_PROGRESS_OUTCOME_PARTIAL;
	} else {
		job->result.outcome = NEMO_PROGRESS_OUTCOME_FAILED;
	}
	nemo_progress_info_set_result (job->common.progress, &job->result);
	g_clear_pointer (&job->incomplete_paths, g_hash_table_unref);
}

static int open_copy_directory (GFile *dir, GCancellable *cancellable, GError **error);
static int open_dest_directory (GFile *file, GCancellable *cancellable, GError **error);
static gboolean sync_copy_fd (int fd, GCancellable *cancellable, GError **error);
static gboolean copy_errno (int err, GError **error);
static gboolean copy_optional_attributes (CopyMoveJob *copy_job, GFile *src, GFile *dest, GFileCopyFlags flags,
                                         GCancellable *cancellable, GError **error);
static GFileInfo *query_copy_source (GFile *src, GCancellable *cancellable, GError **error);
static gboolean copy_source_unchanged (GFile *src, GFileInfo *before,
                                      GCancellable *cancellable, GError **error);
static gboolean copy_directory_identity_unchanged (GFile *dest, GFileInfo *before,
                                                  GCancellable *cancellable, GError **error);

static void
directory_copy_failed (CopyMoveJob *copy_job, GFile *src, GError *error)
{
	CommonJob *job = &copy_job->common;
	int response;

	record_incomplete_copy (copy_job, src, TRUE);
	if (!job_aborted (job) && !job->skip_all_error) {
		response = run_warning (
			job, f (_("The operation on \"%B\" was not completed."), src),
			g_strdup (_("The folder contents could not be fully completed or confirmed. "
			            "Any remaining source files have been retained.")),
			error->message, TRUE, GTK_STOCK_CANCEL, SKIP_ALL, SKIP, NULL);
		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) {
			job->skip_all_error = TRUE;
		}
	}
	g_error_free (error);
}
#endif

typedef enum {
	CREATE_DEST_DIR_RETRY,
	CREATE_DEST_DIR_FAILED,
	CREATE_DEST_DIR_SUCCESS,
	CREATE_DEST_DIR_MERGE
} CreateDestDirResult;

static CreateDestDirResult
create_dest_dir (CommonJob *job,
		 GFile *src,
		 GFile **dest,
		 gboolean same_fs,
		 char **dest_fs_type)
{
	GError *error;
	GFile *new_dest, *dest_dir;
	char *primary, *secondary, *details;
	int response;
	gboolean handled_invalid_filename;
#ifdef NEMO_SMPL
	g_autoptr (GFile) pinned_destination = NULL;
#endif

	handled_invalid_filename = *dest_fs_type != NULL;

 retry:
	/* First create the directory, then copy stuff to it before
	   copying the attributes, because we need to be sure we can write to it */

	error = NULL;
#ifdef NEMO_SMPL
	g_clear_object (&pinned_destination);
	pinned_destination = nemo_transfer_guard_file (((CopyMoveJob *) job)->transfer_guard,
	                                               *dest, TRUE, &error);
	if (pinned_destination == NULL) {
		directory_copy_failed ((CopyMoveJob *) job, src, error);
		return CREATE_DEST_DIR_FAILED;
	}
	if (!g_file_make_directory (pinned_destination, job->cancellable, &error)) {
#else
	if (!g_file_make_directory (*dest, job->cancellable, &error)) {
#endif
		if (IS_IO_ERROR (error, CANCELLED)) {
			g_error_free (error);
			return CREATE_DEST_DIR_FAILED;
#ifdef NEMO_SMPL
		} else if (IS_IO_ERROR (error, EXISTS)) {
			GError *inspect_error = NULL;
			GFileInfo *existing = query_copy_source (pinned_destination,
			                                         job->cancellable, &inspect_error);
			if (existing != NULL &&
			    g_file_info_get_file_type (existing) == G_FILE_TYPE_DIRECTORY) {
				g_object_unref (existing);
				g_error_free (error);
				return CREATE_DEST_DIR_MERGE;
			}
			g_clear_object (&existing);
			if (inspect_error != NULL) {
				g_error_free (error);
				directory_copy_failed ((CopyMoveJob *) job, src, inspect_error);
				return CREATE_DEST_DIR_FAILED;
			}
#endif
		} else if (IS_IO_ERROR (error, INVALID_FILENAME) &&
			   !handled_invalid_filename) {
			handled_invalid_filename = TRUE;

			g_assert (*dest_fs_type == NULL);

			dest_dir = g_file_get_parent (*dest);

			if (dest_dir != NULL) {
				*dest_fs_type = query_fs_type (dest_dir, job->cancellable);

				new_dest = get_target_file (src, dest_dir, *dest_fs_type, same_fs, job->cancellable);
				g_object_unref (dest_dir);

				if (!g_file_equal (*dest, new_dest)) {
					g_object_unref (*dest);
					*dest = new_dest;
					g_error_free (error);
					return CREATE_DEST_DIR_RETRY;
				} else {
					g_object_unref (new_dest);
				}
			}
		}

		primary = f (_("Error while copying."));
		details = NULL;

		if (IS_IO_ERROR (error, PERMISSION_DENIED)) {
			secondary = f (_("The folder \"%B\" cannot be copied because you do not have "
					 "permissions to create it in the destination."), src);
		} else {
			secondary = f (_("There was an error creating the folder \"%B\"."), src);
			details = error->message;
		}

		response = run_warning (job,
					primary,
					secondary,
					details,
					FALSE,
					GTK_STOCK_CANCEL, SKIP, RETRY,
					NULL);

		g_error_free (error);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) {
			/* Skip: Do Nothing  */
		} else if (response == 2) {
			goto retry;
		} else {
			g_assert_not_reached ();
		}
		return CREATE_DEST_DIR_FAILED;
	}
	nemo_file_changes_queue_file_added (*dest);

	if (job->undo_info != NULL) {
		nemo_file_undo_info_ext_add_origin_target_pair (NEMO_FILE_UNDO_INFO_EXT (job->undo_info),
								    src, *dest);
	}

	return CREATE_DEST_DIR_SUCCESS;
}

/* a return value of FALSE means retry, i.e.
 * the destination has changed and the source
 * is expected to re-try the preceeding
 * g_file_move() or g_file_copy() call with
 * the new destination.
 */
static gboolean
copy_move_directory (CopyMoveJob *copy_job,
		     GFile *src,
		     GFile **dest,
		     gboolean same_fs,
		     gboolean create_dest,
		     char **parent_dest_fs_type,
		     SourceInfo *source_info,
		     TransferInfo *transfer_info,
		     GHashTable *debuting_files,
		     gboolean *skipped_file,
		     gboolean readonly_source_fs)
{
	GFileInfo *info;
	GError *error;
	GFile *src_file;
	GFileEnumerator *enumerator;
	char *primary, *secondary, *details;
	char *dest_fs_type;
	int response;
	gboolean skip_error;
	gboolean local_skipped_file;
	CommonJob *job;
	GFileCopyFlags flags;
#ifdef NEMO_SMPL
	int parent_fd = -1, dest_fd = -1;
	g_autoptr (GFileInfo) source_snapshot = NULL;
	g_autoptr (GFileInfo) dest_snapshot = NULL;
	g_autoptr (GFile) pinned_source = NULL;
	g_autoptr (GFile) pinned_destination = NULL;
	g_autoptr (GFile) confirmed_destination = NULL;
#endif

	job = (CommonJob *)copy_job;

#ifdef NEMO_SMPL
	{
		error = NULL;
		source_snapshot = query_copy_source (src, job->cancellable, &error);
		if (source_snapshot != NULL &&
		    g_file_info_get_file_type (source_snapshot) != G_FILE_TYPE_DIRECTORY) {
			g_clear_object (&source_snapshot);
			g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
			                     _("The source is no longer a folder."));
		}
		if (source_snapshot == NULL) {
			directory_copy_failed (copy_job, src, error);
			*skipped_file = TRUE;
			return TRUE;
		}
	}
	error = NULL;
	pinned_source = nemo_transfer_guard_directory (copy_job->transfer_guard, src, FALSE, &error);
	if (pinned_source == NULL) {
		directory_copy_failed (copy_job, src, error);
		*skipped_file = TRUE;
		return TRUE;
	}
#endif
	if (create_dest) {
		CreateDestDirResult creation_result;
#ifdef NEMO_SMPL
		error = NULL;
		pinned_destination = nemo_transfer_guard_file (copy_job->transfer_guard, *dest, TRUE, &error);
		if (pinned_destination != NULL) {
			parent_fd = open_dest_directory (pinned_destination, job->cancellable, &error);
		}
		if (error != NULL) {
			directory_copy_failed (copy_job, src, error);
			*skipped_file = TRUE;
			return TRUE;
		}
#endif
		creation_result = create_dest_dir (job, src, dest, same_fs, parent_dest_fs_type);
		switch (creation_result) {
			case CREATE_DEST_DIR_RETRY:
#ifdef NEMO_SMPL
				if (parent_fd >= 0) {
					close (parent_fd);
				}
#endif
				/* next time copy_move_directory() is called,
				 * create_dest will be FALSE if a directory already
				 * exists under the new name (i.e. WOULD_RECURSE)
				 */
				return FALSE;

			case CREATE_DEST_DIR_FAILED:
#ifdef NEMO_SMPL
				if (parent_fd >= 0) {
					close (parent_fd);
				}
				record_incomplete_copy (copy_job, src, TRUE);
#endif
				*skipped_file = TRUE;
				return TRUE;

			case CREATE_DEST_DIR_MERGE:
				break;
			case CREATE_DEST_DIR_SUCCESS:
			default:
				break;
		}
#ifdef NEMO_SMPL
		copy_job->undo_has_changes |= creation_result == CREATE_DEST_DIR_SUCCESS;
		if (parent_fd >= 0) {
			gboolean synced = sync_copy_fd (parent_fd, job->cancellable, &error);
			if (close (parent_fd) < 0 && synced) {
				synced = copy_errno (errno, &error);
			}
			if (!synced) {
				directory_copy_failed (copy_job, src, error);
				*skipped_file = TRUE;
				return TRUE;
			}
		}
#endif
		/* Recheck conflicts so intentional duplicate names remain unique. */
		if (creation_result == CREATE_DEST_DIR_MERGE)
			return FALSE;

		if (debuting_files) {
			g_hash_table_replace (debuting_files, g_object_ref (*dest), GINT_TO_POINTER (TRUE));
		}

	}

#ifdef NEMO_SMPL
	error = NULL;
	g_clear_object (&pinned_destination);
	pinned_destination = nemo_transfer_guard_directory (copy_job->transfer_guard, *dest, TRUE, &error);
	if (pinned_destination != NULL) {
		dest_fd = open_copy_directory (pinned_destination, job->cancellable, &error);
	}
	if (error == NULL && source_snapshot != NULL) {
		dest_snapshot = query_copy_source (*dest, job->cancellable, &error);
		if (dest_snapshot != NULL &&
		    !copy_directory_identity_unchanged (*dest, dest_snapshot, job->cancellable, &error)) {
			g_clear_object (&dest_snapshot);
		}
	}
	if (error != NULL) {
		if (dest_fd >= 0) {
			close (dest_fd);
		}
		directory_copy_failed (copy_job, src, error);
		*skipped_file = TRUE;
		return TRUE;
	}
#endif
	local_skipped_file = FALSE;
	dest_fs_type = NULL;

	skip_error = should_skip_readdir_error (job, src);
#ifdef NEMO_SMPL
	local_skipped_file = skip_error;
#endif
 retry:
	error = NULL;
	enumerator = g_file_enumerate_children (
#ifdef NEMO_SMPL
						pinned_source,
#else
						src,
#endif
						G_FILE_ATTRIBUTE_STANDARD_NAME,
						G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
						job->cancellable,
						&error);
	if (enumerator) {
		error = NULL;

		while (!job_aborted (job) &&
		       (info = g_file_enumerator_next_file (enumerator, job->cancellable, skip_error?NULL:&error)) != NULL) {
			src_file = g_file_get_child (src,
						     g_file_info_get_name (info));
			copy_move_file (copy_job, src_file, *dest, same_fs, FALSE, NULL, &dest_fs_type,
					source_info, transfer_info, NULL, NULL, FALSE, &local_skipped_file,
					readonly_source_fs);
			g_object_unref (src_file);
			g_object_unref (info);
		}
		g_file_enumerator_close (enumerator, job->cancellable, NULL);
		g_object_unref (enumerator);

		if (error && IS_IO_ERROR (error, CANCELLED)) {
			g_error_free (error);
		} else if (error) {
			if (copy_job->is_move) {
				primary = f (_("Error while moving."));
			} else {
				primary = f (_("Error while copying."));
			}
			details = NULL;

			if (IS_IO_ERROR (error, PERMISSION_DENIED)) {
				secondary = f (_("Files in the folder \"%B\" cannot be copied because you do "
						 "not have permissions to see them."), src);
			} else {
				secondary = f (_("There was an error getting information about the files in the folder \"%B\"."), src);
				details = error->message;
			}

			response = run_warning (job,
						primary,
						secondary,
						details,
						FALSE,
						GTK_STOCK_CANCEL, _("_Skip files"),
						NULL);

			g_error_free (error);

			if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
				abort_job (job);
			} else if (response == 1) {
				/* Skip: Do Nothing */
				local_skipped_file = TRUE;
#ifdef NEMO_SMPL
				record_incomplete_copy (copy_job, src, TRUE);
#endif
			} else {
				g_assert_not_reached ();
			}
		}

		/* Count the copied directory as a file */
		transfer_info->num_files ++;
		report_copy_progress (copy_job, source_info, transfer_info);

		if (debuting_files) {
			g_hash_table_replace (debuting_files, g_object_ref (*dest), GINT_TO_POINTER (create_dest));
		}
	} else if (IS_IO_ERROR (error, CANCELLED)) {
		g_error_free (error);
	} else {
		if (copy_job->is_move) {
			primary = f (_("Error while moving."));
		} else {
			primary = f (_("Error while copying."));
		}
		details = NULL;

		if (IS_IO_ERROR (error, PERMISSION_DENIED)) {
			secondary = f (_("The folder \"%B\" cannot be copied because you do not have "
					 "permissions to read it."), src);
		} else {
			secondary = f (_("There was an error reading the folder \"%B\"."), src);
			details = error->message;
		}

		response = run_warning (job,
					primary,
					secondary,
					details,
					FALSE,
					GTK_STOCK_CANCEL, SKIP, RETRY,
					NULL);

		g_error_free (error);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) {
			/* Skip: Do Nothing  */
			local_skipped_file = TRUE;
#ifdef NEMO_SMPL
			record_incomplete_copy (copy_job, src, TRUE);
#endif
		} else if (response == 2) {
			goto retry;
		} else {
			g_assert_not_reached ();
		}
	}

	if (create_dest) {
		flags = G_FILE_COPY_NOFOLLOW_SYMLINKS;
		if (readonly_source_fs) {
			flags |= G_FILE_COPY_TARGET_DEFAULT_PERMS;
		} else if (copy_job->is_move) {
			flags |= G_FILE_COPY_ALL_METADATA;
		}

#ifdef NEMO_SMPL
		error = NULL;
		/* These directory handles use our own /proc/self/fd links, not
		 * filesystem symlinks selected by the user. Follow the held fds. */
		if (!copy_optional_attributes (copy_job, pinned_source, pinned_destination,
		                               flags & ~G_FILE_COPY_NOFOLLOW_SYMLINKS,
		                               job->cancellable, &error)) {
			directory_copy_failed (copy_job, src, error);
			local_skipped_file = TRUE;
		}
#else
		/* Ignore errors here. Failure to copy metadata is not a hard error */
		g_file_copy_attributes (src, *dest,
					flags,
					job->cancellable, NULL);
#endif
	}

	error = NULL;
#ifdef NEMO_SMPL
	confirmed_destination = nemo_transfer_guard_directory (copy_job->transfer_guard, *dest, TRUE, &error);
	if (confirmed_destination == NULL) {
		directory_copy_failed (copy_job, src, error);
		error = NULL;
		local_skipped_file = TRUE;
	}
	if (source_snapshot != NULL && !job_aborted (job) &&
	    (!(copy_job->is_move ?
	       copy_directory_identity_unchanged (src, source_snapshot, job->cancellable, &error) :
	       copy_source_unchanged (src, source_snapshot, job->cancellable, &error)) ||
	     !copy_directory_identity_unchanged (*dest, dest_snapshot, job->cancellable, &error))) {
		directory_copy_failed (copy_job, src, error);
		error = NULL;
		local_skipped_file = TRUE;
	}
	if (dest_fd >= 0) {
		gboolean synced = TRUE;
		if (!job_aborted (job)) {
			synced = sync_copy_fd (dest_fd, job->cancellable, &error);
		}
		if (close (dest_fd) < 0 && synced) {
			synced = copy_errno (errno, &error);
		}
		if (!synced) {
			directory_copy_failed (copy_job, src, error);
			error = NULL;
			local_skipped_file = TRUE;
		}
	}
#endif
	if (!job_aborted (job) && copy_job->is_move &&
	    /* Don't delete source if there was a skipped file */
	    !local_skipped_file) {
#ifdef NEMO_SMPL
		if (!nemo_transfer_retire_directory (copy_job->transfer_guard, src, job->cancellable, &error)) {
#else
		if (!file_delete_wrapper (src, job->cancellable, &error)) {
#endif
			local_skipped_file = TRUE;
#ifdef NEMO_SMPL
			record_incomplete_copy (copy_job, src, TRUE);
#endif
			if (job->skip_all_error) {
				goto skip;
			}
			primary = f (_("Error while moving \"%B\"."), src);
			secondary = f (_("Could not remove the source folder."));
			details = error->message;

			response = run_warning (job,
						primary,
						secondary,
						details,
						(source_info->num_files - transfer_info->num_files) > 1,
						GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
						NULL);

			if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
				abort_job (job);
			} else if (response == 1) { /* skip all */
				job->skip_all_error = TRUE;
				local_skipped_file = TRUE;
			} else if (response == 2) { /* skip */
				local_skipped_file = TRUE;
			} else {
				g_assert_not_reached ();
			}

		skip:
			g_error_free (error);
		}
	}

	if (local_skipped_file || job_aborted (job)) {
		*skipped_file = TRUE;
#ifdef NEMO_SMPL
		if (!copy_subtree_only_retained (copy_job, src)) {
			record_incomplete_copy (copy_job, src, FALSE);
		}
#endif
	}
#ifdef NEMO_SMPL
	else {
		copy_job->result.completed_items++;
		copy_job->result.completed_directories++;
	}
#endif

	g_free (dest_fs_type);
	return TRUE;
}

#ifndef NEMO_SMPL
static gboolean
remove_target_recursively (CommonJob *job,
			   GFile *src,
			   GFile *toplevel_dest,
			   GFile *file)
{
	GFileEnumerator *enumerator;
	GError *error;
	GFile *child;
	gboolean stop;
	char *primary, *secondary, *details;
	int response;
	GFileInfo *info;

	stop = FALSE;

	error = NULL;
	enumerator = g_file_enumerate_children (file,
						G_FILE_ATTRIBUTE_STANDARD_NAME,
						G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
						job->cancellable,
						&error);
	if (enumerator) {
		error = NULL;

		while (!job_aborted (job) &&
		       (info = g_file_enumerator_next_file (enumerator, job->cancellable, &error)) != NULL) {
			child = g_file_get_child (file,
						  g_file_info_get_name (info));
			if (!remove_target_recursively (job, src, toplevel_dest, child)) {
				stop = TRUE;
				break;
			}
			g_object_unref (child);
			g_object_unref (info);
		}
		g_file_enumerator_close (enumerator, job->cancellable, NULL);
		g_object_unref (enumerator);

	} else if (IS_IO_ERROR (error, NOT_DIRECTORY)) {
		/* Not a dir, continue */
		g_error_free (error);

	} else if (IS_IO_ERROR (error, CANCELLED)) {
		g_error_free (error);
	} else {
		if (job->skip_all_error) {
			goto skip1;
		}

		primary = f (_("Error while copying \"%B\"."), src);
		secondary = f (_("Could not remove files from the already existing folder %F."), file);
		details = error->message;

		/* set show_all to TRUE here, as we don't know how many
		 * files we'll end up processing yet.
		 */
		response = run_warning (job,
					primary,
					secondary,
					details,
					TRUE,
					GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
					NULL);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) { /* skip all */
			job->skip_all_error = TRUE;
		} else if (response == 2) { /* skip */
			/* do nothing */
		} else {
			g_assert_not_reached ();
		}
	skip1:
		g_error_free (error);

		stop = TRUE;
	}

	if (stop) {
		return FALSE;
	}

	error = NULL;

	if (!file_delete_wrapper (file, job->cancellable, &error)) {
		if (job->skip_all_error ||
		    IS_IO_ERROR (error, CANCELLED)) {
			goto skip2;
		}
		primary = f (_("Error while copying \"%B\"."), src);
		secondary = f (_("Could not remove the already existing file %F."), file);
		details = error->message;

		/* set show_all to TRUE here, as we don't know how many
		 * files we'll end up processing yet.
		 */
		response = run_warning (job,
					primary,
					secondary,
					details,
					TRUE,
					GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
					NULL);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) { /* skip all */
			job->skip_all_error = TRUE;
		} else if (response == 2) { /* skip */
			/* do nothing */
		} else {
			g_assert_not_reached ();
		}

	skip2:
		g_error_free (error);

		return FALSE;
	}
	nemo_file_changes_queue_file_removed (file);

	return TRUE;

}
#endif

/* Interval (in bytes) for periodic page cache flushing during
 * cross-filesystem copies.  Every FLUSH_CHUNK_SIZE bytes, the progress
 * callback initiates async writeback and drops clean pages so that dirty
 * pages never accumulate enough to hit the kernel's dirty_ratio limit.
 * See: https://github.com/linuxmint/nemo/issues/3710 */
#define FLUSH_CHUNK_SIZE (32 * 1024 * 1024)  /* 32 MiB */

/* Buffer size for verification reads */
#define VERIFY_BUF_SIZE (1024 * 1024)  /* 1 MiB */

void
nemo_file_operations_set_verify_copies (gboolean verify)
{
	_nemo_next_copy_verify = !!verify;
}

typedef enum {
	VERIFY_RESULT_MATCH,
	VERIFY_RESULT_MISMATCH,
	/* The copy itself is fine as far as we know, but one of the two files
	 * could not be read back, so no comparison was possible. Reporting
	 * this as a mismatch would cry corruption on every file copied from a
	 * phone or a network share. */
	VERIFY_RESULT_UNAVAILABLE
} VerifyResult;

/* Compute the SHA-256 of a file.
 *
 * DONTNEED is only an advisory cache eviction, not a physical-media read
 * guarantee. Files with no local path are read through GIO.
 *
 * Returns NULL if the file could not be read or the job was cancelled. */
static gchar *
checksum_file_bypass_cache (GFile *file, GCancellable *cancellable, guint64 minimum_size)
{
	char *path;
	GChecksum *cksum;
	guchar *buf;
	gchar *result = NULL;
	gboolean failed = FALSE;
	guint64 bytes_hashed = 0;

	cksum = g_checksum_new (G_CHECKSUM_SHA256);
	buf = g_malloc (VERIFY_BUF_SIZE);

	path = g_file_get_path (file);

	if (path != NULL) {
#ifdef NEMO_SMPL
		int fd = -1;
		struct stat st;

		if (!g_cancellable_is_cancelled (cancellable)) {
			fd = open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
		}
#else
		int fd = open (path, O_RDONLY);
#endif

		if (fd < 0) {
			failed = TRUE;
		} else {
#ifdef NEMO_SMPL
			if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode)) {
				close (fd);
				g_checksum_free (cksum);
				g_free (buf);
				g_free (path);
				return NULL;
			}
			minimum_size = MAX (minimum_size, (guint64) st.st_size);
#endif
			/* Request eviction of clean cached pages (advisory only). */
			posix_fadvise (fd, 0, 0, POSIX_FADV_DONTNEED);

			for (;;) {
				if (g_cancellable_is_cancelled (cancellable)) {
					failed = TRUE;
					break;
				}
				ssize_t nread = read (fd, buf, VERIFY_BUF_SIZE);

				if (nread < 0) {
					/* A signal during a multi-GB read is not a
					 * read error; retrying is the difference
					 * between "verified" and a false corruption
					 * warning. */
					if (errno == EINTR) {
						continue;
					}
					failed = TRUE;
					break;
				}

				if (nread == 0) {
					break;
				}

				g_checksum_update (cksum, buf, nread);
				bytes_hashed += nread;

				if (g_cancellable_is_cancelled (cancellable)) {
					failed = TRUE;
					break;
				}
			}

			/* Drop again so we don't pollute the cache with
			 * verification data */
			posix_fadvise (fd, 0, 0, POSIX_FADV_DONTNEED);
#ifdef NEMO_SMPL
			struct stat after, named;
			if (fstat (fd, &after) != 0 || lstat (path, &named) != 0 ||
			    st.st_dev != after.st_dev || st.st_ino != after.st_ino ||
			    st.st_size != after.st_size ||
			    st.st_mtim.tv_sec != after.st_mtim.tv_sec ||
			    st.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
			    st.st_ctim.tv_sec != after.st_ctim.tv_sec ||
			    st.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
			    after.st_dev != named.st_dev || after.st_ino != named.st_ino ||
			    bytes_hashed != (guint64) after.st_size) {
				failed = TRUE;
			}
#endif
			if (close (fd) < 0) {
				failed = TRUE;
			}
		}
		g_free (path);
	} else {
		GFileInputStream *stream;

		stream = g_file_read (file, cancellable, NULL);

		if (stream == NULL) {
			failed = TRUE;
		} else {
			for (;;) {
				gssize nread;

				nread = g_input_stream_read (G_INPUT_STREAM (stream),
				                             buf, VERIFY_BUF_SIZE,
				                             cancellable, NULL);
				if (nread < 0) {
					failed = TRUE;
					break;
				}

				if (nread == 0) {
					break;
				}

				g_checksum_update (cksum, buf, nread);
				bytes_hashed += nread;
			}

#ifdef NEMO_SMPL
			if (!g_input_stream_close (G_INPUT_STREAM (stream), cancellable, NULL)) {
				failed = TRUE;
			}
#endif
			g_object_unref (stream);
		}
	}

	if (!failed && bytes_hashed >= minimum_size &&
	    !g_cancellable_is_cancelled (cancellable)) {
		result = g_strdup (g_checksum_get_string (cksum));
	}

	g_checksum_free (cksum);
	g_free (buf);

	return result;
}

/* The transactional caller flushes its writer before comparing checksums. */
static VerifyResult
verify_copied_file (CommonJob *job, GFile *src, GFile *dest, guint64 minimum_size,
                    const gchar *streamed_checksum)
{
	gchar *read_src_sum = NULL, *dest_sum;
	const gchar *src_sum = streamed_checksum;
	VerifyResult result;

#ifndef NEMO_SMPL
	char *dest_path;
	int fd;
	/* fsync the destination to flush to physical media */
	dest_path = g_file_get_path (dest);
	if (dest_path) {
		fd = open (dest_path, O_RDONLY);
		if (fd >= 0) {
			fsync (fd);
			close (fd);
		}
		g_free (dest_path);
	}
#endif

	if (src_sum == NULL) {
		read_src_sum = checksum_file_bypass_cache (src, job->cancellable, minimum_size);
		src_sum = read_src_sum;
	}
	dest_sum = checksum_file_bypass_cache (dest, job->cancellable, minimum_size);

	if (g_cancellable_is_cancelled (job->cancellable)) {
		result = VERIFY_RESULT_UNAVAILABLE;
	} else if (src_sum == NULL || dest_sum == NULL) {
		char *uri = g_file_get_uri (src_sum == NULL ? src : dest);

		g_warning ("verify-copy: could not read back %s to compare it", uri);
		g_free (uri);

		result = VERIFY_RESULT_UNAVAILABLE;
	} else if (g_strcmp0 (src_sum, dest_sum) == 0) {
		result = VERIFY_RESULT_MATCH;
	} else {
		char *src_uri = g_file_get_uri (src);
		char *dest_uri = g_file_get_uri (dest);

		g_warning ("verify-copy: CHECKSUM MISMATCH  src=%s  dest=%s  (%s vs %s)",
		           src_uri, dest_uri, src_sum, dest_sum);

		g_free (src_uri);
		g_free (dest_uri);

		result = VERIFY_RESULT_MISMATCH;
	}

	g_free (read_src_sum);
	g_free (dest_sum);

	return result;
}

typedef struct {
	CopyMoveJob *job;
	goffset last_size;
	SourceInfo *source_info;
	TransferInfo *transfer_info;
	/* Page cache flush state for cross-filesystem copies */
	GFile *dest;           /* dest file, or NULL to skip flushing */
	int    dest_fd;        /* borrowed writer fd for transactional copies */
	goffset last_flush_offset; /* last offset where we kicked writeback */
	int    flush_errno;    /* first real writeback failure seen, else 0 */
} ProgressData;

/* sync_file_range() reports EINVAL/ENOSYS/EOPNOTSUPP on filesystems that
 * simply do not implement it. None of those
 * mean the user's data is at risk. Anything else -- ENOSPC, EIO, ENODEV from
 * a yanked USB stick -- means writeback genuinely failed and the copy must
 * not be reported as successful. */
static gboolean
flush_error_is_fatal (int err)
{
	switch (err) {
	case EINVAL:
	case ENOSYS:
	case EOPNOTSUPP:
#ifndef NEMO_SMPL
	case EBADF:
#endif
		return FALSE;
	default:
		return TRUE;
	}
}

#ifdef NEMO_SMPL
static int
pace_copy_writeback (int fd, goffset offset, goffset length, GCancellable *cancellable)
{
	int result;

	do {
		if (g_cancellable_is_cancelled (cancellable)) {
			return ECANCELED;
		}
		/* Wait for this range, not the next (as yet unwritten) range.
		 * This bounds dirty data without mistaking writeback for fsync. */
		result = sync_file_range (fd, offset, length,
		                          SYNC_FILE_RANGE_WAIT_BEFORE |
		                          SYNC_FILE_RANGE_WRITE |
		                          SYNC_FILE_RANGE_WAIT_AFTER);
	} while (result < 0 && errno == EINTR);

	return result < 0 && flush_error_is_fatal (errno) ? errno : 0;
}
#endif

static void
copy_file_progress_callback (goffset current_num_bytes,
			     goffset total_num_bytes,
			     gpointer user_data)
{
	ProgressData *pdata;
	goffset new_size;

	pdata = user_data;

	new_size = current_num_bytes - pdata->last_size;

	if (new_size > 0) {
		pdata->transfer_info->num_bytes += new_size;
		pdata->last_size = current_num_bytes;
		report_copy_progress (pdata->job,
				      pdata->source_info,
				      pdata->transfer_info);
	}

#ifdef NEMO_SMPL
	if (pdata->dest_fd >= 0 && pdata->flush_errno == 0 &&
	    current_num_bytes - pdata->last_flush_offset >= FLUSH_CHUNK_SIZE) {
		pdata->flush_errno = pace_copy_writeback (
			pdata->dest_fd, pdata->last_flush_offset,
			current_num_bytes - pdata->last_flush_offset,
			pdata->job->common.cancellable);
		if (pdata->flush_errno == 0) {
			posix_fadvise (pdata->dest_fd, pdata->last_flush_offset,
			               current_num_bytes - pdata->last_flush_offset,
			               POSIX_FADV_DONTNEED);
		}
		pdata->last_flush_offset = current_num_bytes;
	}
#else
	/* Periodically flush dest pages to prevent dirty page accumulation
	 * on slow devices (e.g. USB).  Without this, splice() fills the
	 * page cache at RAM speed and the kernel hits dirty_ratio, blocking
	 * all writers for minutes.
	 *
	 * SYNC_FILE_RANGE_WAIT_BEFORE waits for any previously submitted
	 * writeback to complete (natural back-pressure that paces the copy
	 * to the device speed).  SYNC_FILE_RANGE_WRITE then starts async
	 * writeback for the current chunk.  FADV_DONTNEED drops pages whose
	 * writeback has already completed. */
	if (pdata->dest != NULL &&
	    current_num_bytes - pdata->last_flush_offset >= FLUSH_CHUNK_SIZE) {
		if (pdata->dest_fd < 0) {
			char *path = g_file_get_path (pdata->dest);
			if (path != NULL) {
				pdata->dest_fd = open (path, O_RDONLY);
				g_free (path);
			}
		}

		if (pdata->dest_fd >= 0) {
			goffset flush_end = current_num_bytes;

			if (sync_file_range (pdata->dest_fd,
			                     pdata->last_flush_offset,
			                     flush_end - pdata->last_flush_offset,
			                     SYNC_FILE_RANGE_WAIT_BEFORE |
			                     SYNC_FILE_RANGE_WRITE) != 0 &&
			    pdata->flush_errno == 0 &&
			    flush_error_is_fatal (errno)) {
				/* Remembered rather than acted on here: this
				 * callback cannot fail the operation, so
				 * copy_move_file() checks it once the copy
				 * returns. */
				pdata->flush_errno = errno;
			}

			/* Drop pages from ranges whose writeback is likely done */
			if (pdata->last_flush_offset > 0) {
				posix_fadvise (pdata->dest_fd, 0,
				               pdata->last_flush_offset,
				               POSIX_FADV_DONTNEED);
			}
		}

		pdata->last_flush_offset = current_num_bytes;
	}
#endif
}

static gboolean
test_dir_is_parent (GFile *child, GFile *root)
{
	GFile *f, *tmp;

	f = g_file_dup (child);
	while (f) {
		if (g_file_equal (f, root)) {
			g_object_unref (f);
			return TRUE;
		}
		tmp = f;
		f = g_file_get_parent (f);
		g_object_unref (tmp);
	}
	if (f) {
		g_object_unref (f);
	}
	return FALSE;
}

static char *
query_fs_type (GFile *file,
	       GCancellable *cancellable)
{
	GFileInfo *fsinfo;
	char *ret;

	ret = NULL;

	fsinfo = g_file_query_filesystem_info (file,
					       G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
					       cancellable,
					       NULL);
	if (fsinfo != NULL) {
		ret = g_strdup (g_file_info_get_attribute_string (fsinfo, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE));
		g_object_unref (fsinfo);
	}

	if (ret == NULL) {
		/* ensure that we don't attempt to query
		 * the FS type for each file in a given
		 * directory, if it can't be queried. */
		ret = g_strdup ("");
	}

	return ret;
}

static gboolean
is_trusted_desktop_file (GFile *file,
			 GCancellable *cancellable)
{
	char *basename;
	gboolean res;
	GFileInfo *info;

	/* Don't trust non-local files */
	if (!g_file_is_native (file)) {
		return FALSE;
	}

	basename = g_file_get_basename (file);
	if (basename && !g_str_has_suffix (basename, ".desktop")) {
		g_free (basename);
		return FALSE;
	}
	g_free (basename);

	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_STANDARD_TYPE ","
				  G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE,
				  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				  cancellable,
				  NULL);

	if (info == NULL) {
		return FALSE;
	}

	res = FALSE;

	/* Weird file => not trusted,
	   Already executable => no need to mark trusted */
	if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR &&
	    !g_file_info_get_attribute_boolean (info,
						G_FILE_ATTRIBUTE_ACCESS_CAN_EXECUTE) &&
	    nemo_is_in_system_dir (file)) {
		res = TRUE;
	}
	g_object_unref (info);

	return res;
}

typedef struct {
	int id;
	char *new_name;
	gboolean apply_to_all;
} ConflictResponseData;

typedef struct {
	GFile *src;
	GFile *dest;
	GFile *dest_dir;
	GtkWindow *parent;
	ConflictResponseData *resp_data;
#ifdef NEMO_SMPL
	NemoProgressInfo *progress;
#endif
} ConflictDialogData;

static gboolean
do_run_conflict_dialog (gpointer _data)
{
	ConflictDialogData *data = _data;
	GtkWidget *dialog;
	int response;

	dialog = nemo_file_conflict_dialog_new (data->parent,
						    data->src,
						    data->dest,
						    data->dest_dir);
#ifdef NEMO_SMPL
	nemo_progress_info_attach_dialog (data->progress, GTK_WINDOW (dialog));
#endif
	response = gtk_dialog_run (GTK_DIALOG (dialog));

	if (response == CONFLICT_RESPONSE_RENAME) {
		data->resp_data->new_name =
			nemo_file_conflict_dialog_get_new_name (NEMO_FILE_CONFLICT_DIALOG (dialog));
	} else if (response != GTK_RESPONSE_CANCEL ||
		   response != GTK_RESPONSE_NONE) {
		   data->resp_data->apply_to_all =
			   nemo_file_conflict_dialog_get_apply_to_all
				(NEMO_FILE_CONFLICT_DIALOG (dialog));
	}

	data->resp_data->id = response;

	gtk_widget_destroy (dialog);

	return FALSE;
}

static ConflictResponseData *
run_conflict_dialog (CommonJob *job,
		     GFile *src,
		     GFile *dest,
		     GFile *dest_dir)
{
	ConflictDialogData *data;
	ConflictResponseData *resp_data;

    nemo_progress_info_pause (job->progress);

	data = g_new0 (ConflictDialogData, 1);
	data->parent = job->parent_window;
	data->src = src;
	data->dest = dest;
	data->dest_dir = dest_dir;
#ifdef NEMO_SMPL
	data->progress = job->progress;
#endif

	resp_data = g_new0 (ConflictResponseData, 1);
	resp_data->new_name = NULL;
	data->resp_data = resp_data;

	g_io_scheduler_job_send_to_mainloop (job->io_job,
					     do_run_conflict_dialog,
					     data,
					     NULL);

	g_free (data);

    nemo_progress_info_resume (job->progress);

	return resp_data;
}

static void
conflict_response_data_free (ConflictResponseData *data)
{
	g_free (data->new_name);
	g_free (data);
}

static GFile *
get_target_file_for_display_name (GFile *dir,
				  const gchar *name)
{
	GFile *dest;

	dest = NULL;
	dest = g_file_get_child_for_display_name (dir, name, NULL);

	if (dest == NULL) {
		dest = g_file_get_child (dir, name);
	}

	return dest;
}

#ifdef NEMO_SMPL
static GFile *
make_safe_copy_staging_file (GFile *dest)
{
	GFile *parent, *staging;
	char *uuid, *staging_name;

	parent = g_file_get_parent (dest);
	if (parent == NULL) {
		return NULL;
	}

	/* Independent of the final basename, which may already be NAME_MAX. */
	uuid = g_uuid_string_random ();
	staging_name = g_strconcat ("copy.nemo-partial-", uuid, NULL);
	staging = g_file_get_child (parent, staging_name);

	g_free (uuid);
	g_free (staging_name);
	g_object_unref (parent);
	return staging;
}

static void
discard_staging_file (CopyMoveJob *copy_job, GFile *staging)
{
	CommonJob *job = &copy_job->common;
	GError *error = NULL;

	/* Only called for an exclusively created, unpublished staging entry.
	 * Do not issue an uncancellable remote RPC after cancellation. */
	if (!g_file_delete (staging, g_file_is_native (staging) ? NULL : job->cancellable, &error) &&
	    !IS_IO_ERROR (error, NOT_FOUND)) {
		char *name = g_file_get_parse_name (staging);
		copy_job->had_errors = TRUE;
		g_warning ("Could not remove incomplete copy %s: %s", name, error->message);
		if (!job_aborted (job)) {
			run_warning (job, g_strdup (_("Could not remove the incomplete copy.")),
			             g_strdup_printf (_("The temporary file \"%s\" was left behind. "
			                                "It is not a completed copy."), name),
			             error->message, FALSE, GTK_STOCK_OK, NULL);
		}
		g_free (name);
	}
	g_clear_error (&error);
}

static gboolean
copy_errno (int err, GError **error)
{
	g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
	             _("Error while writing to the destination: %s"), g_strerror (err));
	return FALSE;
}

static gboolean
copy_length_is_complete (guint64 minimum_size, guint64 copied, GError **error)
{
	if (copied < minimum_size) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
		                     _("The copy is shorter than the reported source size. "
		                       "The destination was not replaced and the source was not removed."));
		return FALSE;
	}
	/* Some virtual files report zero while providing a nonempty stream. */
	return TRUE;
}

static gboolean
copy_optional_attributes (CopyMoveJob *copy_job, GFile *src, GFile *dest, GFileCopyFlags flags,
                          GCancellable *cancellable, GError **error)
{
	GError *attribute_error = NULL;

	if (g_file_copy_attributes (src, dest, flags, cancellable, &attribute_error)) {
		return !g_cancellable_set_error_if_cancelled (cancellable, error);
	}
	/* Ownership/extended attributes are not supported by every backend.
	 * Actual I/O and space errors must not disappear with optional metadata. */
	if (IS_IO_ERROR (attribute_error, NOT_SUPPORTED) ||
	    IS_IO_ERROR (attribute_error, PERMISSION_DENIED)) {
		copy_job->metadata_limitations++;
		g_debug ("Could not copy optional file attributes: %s", attribute_error->message);
		g_error_free (attribute_error);
		return !g_cancellable_set_error_if_cancelled (cancellable, error);
	}
	g_propagate_error (error, attribute_error);
	return FALSE;
}

static gboolean
sync_copy_fd (int fd, GCancellable *cancellable, GError **error)
{
	int result;
	do {
		if (g_cancellable_set_error_if_cancelled (cancellable, error)) {
			return FALSE;
		}
		result = fsync (fd);
	} while (result < 0 && errno == EINTR);
	return result == 0 || copy_errno (errno, error);
}

static int
open_copy_directory (GFile *dir, GCancellable *cancellable, GError **error)
{
	char *path;
	int fd;

	if (g_cancellable_set_error_if_cancelled (cancellable, error)) {
		return -1;
	}
	path = g_file_get_path (dir);
	if (path == NULL) {
		/* GIO backends provide close/rename acknowledgements, not a
		 * portable remote directory-fsync API. */
		return -1;
	}

	do {
		fd = open (path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	} while (fd < 0 && errno == EINTR && !g_cancellable_is_cancelled (cancellable));
	if (fd < 0) {
		copy_errno (errno, error);
	}
	g_free (path);
	return fd;
}

static int
open_dest_directory (GFile *file, GCancellable *cancellable, GError **error)
{
	GFile *parent = g_file_get_parent (file);
	int fd;

	if (parent == NULL) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		                     _("The destination has no parent folder."));
		return -1;
	}
	fd = open_copy_directory (parent, cancellable, error);
	g_object_unref (parent);
	return fd;
}

#define COPY_SOURCE_ATTRIBUTES G_FILE_ATTRIBUTE_STANDARD_TYPE "," \
	G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET "," \
	G_FILE_ATTRIBUTE_ID_FILE "," G_FILE_ATTRIBUTE_ETAG_VALUE "," \
	G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC "," \
	G_FILE_ATTRIBUTE_TIME_CHANGED "," G_FILE_ATTRIBUTE_TIME_CHANGED_USEC "," \
	"time::modified-nsec,time::changed-nsec"

static GFileInfo *
query_copy_source (GFile *src, GCancellable *cancellable, GError **error)
{
	GFileInfo *info = g_file_query_info (src, COPY_SOURCE_ATTRIBUTES,
	                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                                    cancellable, error);
	if (info && (!g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_STANDARD_TYPE) ||
	             g_file_info_get_file_type (info) == G_FILE_TYPE_UNKNOWN)) {
		g_clear_object (&info);
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		                     _("Could not determine the source file type."));
	}
	return info;
}

static gboolean
move_without_fallback (CopyMoveJob *copy_job, GFile *src, GFile *dest, GFileCopyFlags flags,
                       GCancellable *cancellable, gboolean *published, GError **error)
{
	return nemo_transfer_native_move (copy_job->transfer_guard, src, dest,
	                                  (flags & G_FILE_COPY_OVERWRITE) != 0, published,
	                                  &copy_job->transfer_undo, cancellable, error);
}

static gboolean
copy_conflict_is_merge (CopyMoveJob *copy_job, GFile *src, GFile *dest,
                        gboolean *is_merge, gboolean *retained_regular)
{
	CommonJob *job = &copy_job->common;
	GError *error = NULL;
	GFileInfo *src_info = query_copy_source (src, job->cancellable, &error);
	GFileInfo *dest_info = NULL;
	gboolean ok;

	if (src_info) {
		dest_info = query_copy_source (dest, job->cancellable, &error);
	}
	ok = src_info != NULL && dest_info != NULL;
	if (ok) {
		*is_merge = g_file_info_get_file_type (src_info) == G_FILE_TYPE_DIRECTORY &&
		            g_file_info_get_file_type (dest_info) == G_FILE_TYPE_DIRECTORY;
		if (retained_regular != NULL) {
			*retained_regular = !copy_job->is_move && !copy_job->verify_after_copy &&
			                   g_file_info_get_file_type (src_info) == G_FILE_TYPE_REGULAR &&
			                   g_file_info_get_file_type (dest_info) == G_FILE_TYPE_REGULAR &&
			                   (!g_file_info_has_attribute (src_info, G_FILE_ATTRIBUTE_STANDARD_SIZE) ||
			                    !g_file_info_has_attribute (dest_info, G_FILE_ATTRIBUTE_STANDARD_SIZE) ||
			                    g_file_info_get_size (src_info) == g_file_info_get_size (dest_info));
		}
	} else {
		record_incomplete_copy (copy_job, src, TRUE);
		if (!job_aborted (job) && !job->skip_all_error) {
			int response = run_warning (
				job, g_strdup (_("Could not inspect the conflicting files.")),
				g_strdup (_("The existing destination has not been changed.")),
				error->message, TRUE, GTK_STOCK_CANCEL, SKIP_ALL, SKIP, NULL);
			if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
				abort_job (job);
			} else if (response == 1) {
				job->skip_all_error = TRUE;
			}
		}
		g_clear_error (&error);
	}
	g_clear_object (&src_info);
	g_clear_object (&dest_info);
	return ok;
}

static gboolean
copy_source_has_version (GFileInfo *info)
{
	const gchar *etag = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_ETAG_VALUE);
	return (etag != NULL && etag[0] != '\0') ||
	       g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED) > 0;
}

static gboolean
copy_info_unchanged (GFileInfo *before, GFileInfo *after)
{
	gboolean same;
	same = g_file_info_get_file_type (before) == g_file_info_get_file_type (after) &&
	       g_file_info_get_attribute_uint64 (before, G_FILE_ATTRIBUTE_STANDARD_SIZE) ==
	       g_file_info_get_attribute_uint64 (after, G_FILE_ATTRIBUTE_STANDARD_SIZE) &&
	       g_strcmp0 (g_file_info_get_attribute_string (before, G_FILE_ATTRIBUTE_ETAG_VALUE),
	                  g_file_info_get_attribute_string (after, G_FILE_ATTRIBUTE_ETAG_VALUE)) == 0 &&
	       g_strcmp0 (g_file_info_get_attribute_string (before, G_FILE_ATTRIBUTE_ID_FILE),
	                  g_file_info_get_attribute_string (after, G_FILE_ATTRIBUTE_ID_FILE)) == 0 &&
	       g_file_info_get_attribute_uint64 (before, G_FILE_ATTRIBUTE_TIME_MODIFIED) ==
	       g_file_info_get_attribute_uint64 (after, G_FILE_ATTRIBUTE_TIME_MODIFIED) &&
	       g_file_info_get_attribute_uint32 (before, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC) ==
	       g_file_info_get_attribute_uint32 (after, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC) &&
	       g_file_info_get_attribute_uint64 (before, G_FILE_ATTRIBUTE_TIME_CHANGED) ==
	       g_file_info_get_attribute_uint64 (after, G_FILE_ATTRIBUTE_TIME_CHANGED) &&
	       g_file_info_get_attribute_uint32 (before, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC) ==
	       g_file_info_get_attribute_uint32 (after, G_FILE_ATTRIBUTE_TIME_CHANGED_USEC) &&
	       g_file_info_get_attribute_uint32 (before, "time::modified-nsec") ==
	       g_file_info_get_attribute_uint32 (after, "time::modified-nsec") &&
	       g_file_info_get_attribute_uint32 (before, "time::changed-nsec") ==
	       g_file_info_get_attribute_uint32 (after, "time::changed-nsec");
	if (same && g_file_info_get_file_type (before) == G_FILE_TYPE_SYMBOLIC_LINK) {
		same = g_strcmp0 (g_file_info_get_attribute_byte_string (before, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET),
		                  g_file_info_get_attribute_byte_string (after, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET)) == 0;
	}
	return same;
}

static gboolean
copy_source_unchanged (GFile *src, GFileInfo *before, GCancellable *cancellable, GError **error)
{
	g_autoptr (GFileInfo) after = query_copy_source (src, cancellable, error);
	if (after == NULL) {
		return FALSE;
	}
	gboolean same = copy_info_unchanged (before, after);
	if (!same) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                     _("A file changed during the operation. Completion could not be "
		                       "confirmed and the source was not removed."));
	}
	return same;
}

static gboolean
copy_directory_identity_unchanged (GFile *dest, GFileInfo *before,
                                   GCancellable *cancellable, GError **error)
{
	g_autoptr (GFileInfo) after = query_copy_source (dest, cancellable, error);
	if (after == NULL) {
		return FALSE;
	}
	const char *id = g_file_info_get_attribute_string (before, G_FILE_ATTRIBUTE_ID_FILE);
	if (g_file_info_get_file_type (before) != G_FILE_TYPE_DIRECTORY ||
	    g_file_info_get_file_type (after) != G_FILE_TYPE_DIRECTORY ||
	    id == NULL || id[0] == '\0' ||
	    g_strcmp0 (id, g_file_info_get_attribute_string (after, G_FILE_ATTRIBUTE_ID_FILE)) != 0) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                     _("The destination folder identity could not be confirmed."));
		return FALSE;
	}
	return TRUE;
}

/* An unreadable or unstable existing file is not a different file, and never
 * grants permission to replace it. Do not use the copy-corruption diagnostic
 * here: different existing contents are an ordinary user conflict. */
static VerifyResult
compare_existing_copy (CommonJob *job, GFile *src, GFile *dest,
                       GFileInfo *src_info, GFileInfo *dest_info, GError **error)
{
	GFileType type = g_file_info_get_file_type (src_info);
	VerifyResult verdict = VERIFY_RESULT_MISMATCH;
	g_autofree char *src_sum = NULL;
	g_autofree char *dest_sum = NULL;

	if ((type == G_FILE_TYPE_REGULAR && !copy_source_has_version (src_info)) ||
	    (g_file_info_get_file_type (dest_info) == G_FILE_TYPE_REGULAR &&
	     !copy_source_has_version (dest_info))) {
		goto unavailable;
	}
	if (type == g_file_info_get_file_type (dest_info)) {
		if (type == G_FILE_TYPE_REGULAR) {
			if (g_file_info_has_attribute (src_info, G_FILE_ATTRIBUTE_STANDARD_SIZE) &&
			    g_file_info_has_attribute (dest_info, G_FILE_ATTRIBUTE_STANDARD_SIZE) &&
			    g_file_info_get_size (src_info) > 0 && g_file_info_get_size (dest_info) > 0 &&
			    g_file_info_get_size (src_info) != g_file_info_get_size (dest_info)) {
				goto check_stability;
			}
			nemo_progress_info_set_details (job->progress, _("Verifying existing file contents (no data written)"));
			nemo_progress_info_pulse_progress (job->progress);
			src_sum = checksum_file_bypass_cache (src, job->cancellable,
				g_file_info_get_attribute_uint64 (src_info, G_FILE_ATTRIBUTE_STANDARD_SIZE));
			if (src_sum == NULL) {
				goto unavailable;
			}
			dest_sum = checksum_file_bypass_cache (dest, job->cancellable,
				g_file_info_get_attribute_uint64 (dest_info, G_FILE_ATTRIBUTE_STANDARD_SIZE));
			if (dest_sum == NULL) {
				goto unavailable;
			}
			verdict = g_str_equal (src_sum, dest_sum) ? VERIFY_RESULT_MATCH : VERIFY_RESULT_MISMATCH;
		} else if (type == G_FILE_TYPE_SYMBOLIC_LINK) {
			const char *source_target = g_file_info_get_symlink_target (src_info);
			const char *dest_target = g_file_info_get_symlink_target (dest_info);
			if (source_target == NULL || dest_target == NULL) {
				goto unavailable;
			}
			verdict = g_str_equal (source_target, dest_target) ? VERIFY_RESULT_MATCH : VERIFY_RESULT_MISMATCH;
		}
	}
check_stability:
	if (!copy_source_unchanged (src, src_info, job->cancellable, error) ||
	    !copy_source_unchanged (dest, dest_info, job->cancellable, error)) {
		return VERIFY_RESULT_UNAVAILABLE;
	}
	return verdict;

unavailable:
	if (!g_cancellable_set_error_if_cancelled (job->cancellable, error)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                     _("The existing contents could not be verified. "
		                       "The source and destination have been retained."));
	}
	return VERIFY_RESULT_UNAVAILABLE;
}

static GInputStream *
open_copy_source (GFile *src, GCancellable *cancellable, guint64 *minimum_size, GError **error)
{
	char *path = g_file_get_path (src);
	int fd;
	struct stat st;

	if (path == NULL) {
		return G_INPUT_STREAM (g_file_read (src, cancellable, error));
	}
	if (g_cancellable_set_error_if_cancelled (cancellable, error)) {
		g_free (path);
		return NULL;
	}
	do {
		fd = open (path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	} while (fd < 0 && errno == EINTR && !g_cancellable_is_cancelled (cancellable));
	g_free (path);
	if (fd < 0) {
		g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
		             _("Could not open the source: %s"), g_strerror (errno));
		return NULL;
	}
	if (fstat (fd, &st) < 0 || !S_ISREG (st.st_mode)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                     _("The source is no longer a regular file."));
		close (fd);
		return NULL;
	}
	*minimum_size = MAX (*minimum_size, (guint64) st.st_size);
	return g_unix_input_stream_new (fd, TRUE);
}

/* Keep the actual writer open from exclusive creation through fsync. A new
 * read-only fd opened after copying can miss an already-consumed writeback
 * error. Stream close errors are part of the transaction too. */
static gboolean
write_copy_contents (GFile *src, GFile *staging, GFileInfo *info,
                     GFileOutputStream *output, GFileCopyFlags flags,
                     ProgressData *pdata, GChecksum *source_checksum,
                     guint64 *copied_bytes, GError **error)
{
	GCancellable *cancellable = pdata->job->common.cancellable;
	GInputStream *input;
	guchar *buffer = NULL;
	goffset copied = 0;
	gboolean ok = FALSE;
	struct stat st;
	GError *close_error = NULL;
	guint64 minimum_size = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE);

	input = open_copy_source (src, cancellable, &minimum_size, error);
	if (input == NULL) {
		return FALSE;
	}
	if (G_IS_FILE_DESCRIPTOR_BASED (output)) {
		pdata->dest_fd = g_file_descriptor_based_get_fd (G_FILE_DESCRIPTOR_BASED (output));
		if (fstat (pdata->dest_fd, &st) < 0 || !S_ISREG (st.st_mode)) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			                     _("The incomplete copy is not a regular file."));
			goto out;
		}
	} else if (g_file_is_native (staging)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		                     _("The local copy cannot be flushed through its writer."));
		goto out;
	}

	buffer = g_malloc (VERIFY_BUF_SIZE);
	for (;;) {
		gssize count = g_input_stream_read (input, buffer, VERIFY_BUF_SIZE, cancellable, error);
		if (count < 0) {
			break;
		}
		if (count == 0) {
			ok = TRUE;
			break;
		}
		if (source_checksum != NULL) {
			g_checksum_update (source_checksum, buffer, count);
		}
		if (!g_output_stream_write_all (G_OUTPUT_STREAM (output), buffer, count,
		                                NULL, cancellable, error)) {
			break;
		}
		copied += count;
		copy_file_progress_callback (copied,
		                             g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE),
		                             pdata);
		if (pdata->flush_errno != 0) {
			copy_errno (pdata->flush_errno, error);
			break;
		}
	}
	if (ok) {
		ok = copy_length_is_complete (minimum_size, copied, error) &&
		     copy_optional_attributes (pdata->job, src, staging, flags, cancellable, error) &&
		     g_output_stream_flush (G_OUTPUT_STREAM (output), cancellable, error);
	}
	if (ok && pdata->dest_fd >= 0) {
		nemo_progress_info_set_details (pdata->job->common.progress, _("Flushing the copy to storage"));
		int err = pace_copy_writeback (pdata->dest_fd, 0, 0, cancellable);
		ok = err == 0 || copy_errno (err, error);
		if (ok) {
			ok = sync_copy_fd (pdata->dest_fd, cancellable, error);
		}
	}
out:
	g_free (buffer);
	/* Explicitly close even on cancellation. Stream disposal otherwise
	 * performs close(NULL), which can hang on an unresponsive backend. */
	if (!g_input_stream_close (input, cancellable, &close_error)) {
		if (ok) {
			g_propagate_error (error, close_error);
			ok = FALSE;
		} else {
			if (!IS_IO_ERROR (close_error, CANCELLED)) {
				g_warning ("Could not close copy source: %s", close_error->message);
			}
			g_error_free (close_error);
		}
	}
	g_object_unref (input);
	if (ok) {
		*copied_bytes = copied;
	}
	return ok;
}

static void
native_copy_progress_callback (goffset current, goffset total, gpointer data)
{
	ProgressData *pdata = data;

	if (pdata->dest_fd < 0 && pdata->flush_errno == 0 &&
	    current - pdata->last_flush_offset >= FLUSH_CHUNK_SIZE &&
	    !job_aborted (&pdata->job->common)) {
		char *path = g_file_get_path (pdata->dest);
		struct stat st;
		int fd = open (path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		int err = errno;
		g_free (path);

		if (fd >= 0) {
			if (fstat (fd, &st) == 0 && S_ISREG (st.st_mode)) {
				pdata->dest_fd = fd;
				pdata->last_flush_offset = 0;
			} else {
				close (fd);
				pdata->flush_errno = EIO;
			}
		} else if (err != ENOENT && err != EINTR) {
			pdata->flush_errno = err;
		} else {
			/* A backend may itself stage under another name. Do not
			 * repeatedly probe it between writeback intervals. */
			pdata->last_flush_offset = current;
		}
	}
	copy_file_progress_callback (current, total, pdata);
}

/* GVfs forwards local paths to another process: /proc/self would name its
 * descriptors, not ours. The transaction keeps these directory fds open.
 * Check that our PID also names us in this procfs mount; a different PID
 * namespace in the backend must fail, never fall back to a public pathname. */
static GFile *
native_backend_file (GFile *file, GError **error)
{
	g_autofree char *path = g_file_get_path (file);
	g_autofree char *anchor = NULL;
	g_autofree char *external = NULL;
	const char *prefix = "/proc/self/fd/";
	char *end;
	guint64 fd;
	struct stat held, named;

	if (path == NULL || !g_str_has_prefix (path, prefix)) {
		return g_object_ref (file);
	}
	fd = g_ascii_strtoull (path + strlen (prefix), &end, 10);
	if (end == path + strlen (prefix) || fd > G_MAXINT || *end != '/') {
		goto unavailable;
	}
	anchor = g_strdup_printf ("/proc/%ld/fd/%d", (long) getpid (), (int) fd);
	if (fstat ((int) fd, &held) < 0 || stat (anchor, &named) < 0 ||
	    !S_ISDIR (held.st_mode) || !S_ISDIR (named.st_mode) ||
	    held.st_dev != named.st_dev || held.st_ino != named.st_ino) {
		goto unavailable;
	}
	external = g_strconcat (anchor, end, NULL);
	return g_file_new_for_path (external);

unavailable:
	g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
	                     _("The backend cannot access the pinned copy location."));
	return NULL;
}

/* Some GVfs sources implement pull but not stream reads. Their writer is
 * opaque to us: an early filesystem error-tracking fd plus checked syncfs
 * is required, not a newly opened read-only fd alone. Keep the backend's
 * output inside an exclusively created private directory until it is ready. */
static gboolean
copy_with_native_backend (GFile *src, GFile *staging, GFileInfo *source_info, GFileCopyFlags flags,
                          ProgressData *pdata, int filesystem_fd, GError **error)
{
	CommonJob *job = &pdata->job->common;
	GFile *container = NULL, *payload = NULL;
	g_autoptr (GFile) backend_source = NULL;
	g_autoptr (GFile) backend_payload = NULL;
	gboolean owned = FALSE, ok = FALSE;
	int fd = -1, result;
	struct stat st;
	ProgressData backend_progress = *pdata;

	for (int attempt = 0; attempt < 16; attempt++) {
		char *path;
		int err;

		if (g_cancellable_set_error_if_cancelled (job->cancellable, error)) {
			goto out;
		}
		g_clear_object (&container);
		container = make_safe_copy_staging_file (staging);
		path = g_file_get_path (container);
		result = g_mkdir (path, 0700);
		err = errno;
		g_free (path);
		if (result == 0) {
			owned = TRUE;
			break;
		}
		if (err != EEXIST || attempt == 15) {
			copy_errno (err, error);
			goto out;
		}
	}
	payload = g_file_get_child (container, "payload");
	backend_source = native_backend_file (src, error);
	if (backend_source == NULL) {
		goto out;
	}
	backend_payload = native_backend_file (payload, error);
	if (backend_payload == NULL) {
		goto out;
	}
	backend_progress.dest = payload;
	backend_progress.dest_fd = -1;
	backend_progress.last_flush_offset = 0;
	/* Keep the random, exclusively owned container component in the external
	 * alias, not just its fd. A wrong process/namespace must not overwrite an
	 * unrelated entry, and only our pinned payload can be adopted below. */
	if (!g_file_copy (backend_source, backend_payload, flags & ~G_FILE_COPY_OVERWRITE,
	                  job->cancellable, native_copy_progress_callback, &backend_progress, error)) {
		goto out;
	}
	if (backend_progress.flush_errno != 0) {
		copy_errno (backend_progress.flush_errno, error);
		goto out;
	}
	nemo_progress_info_set_details (job->progress, _("Flushing the copy to storage"));
	do {
		if (g_cancellable_set_error_if_cancelled (job->cancellable, error)) {
			goto out;
		}
		result = syncfs (filesystem_fd);
	} while (result < 0 && errno == EINTR);
	if (result < 0) {
		copy_errno (errno, error);
		goto out;
	}

	char *path = g_file_get_path (payload);
	do {
		fd = open (path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	} while (fd < 0 && errno == EINTR && !g_cancellable_is_cancelled (job->cancellable));
	g_free (path);
	if (fd < 0) {
		copy_errno (errno, error);
		goto out;
	}
	if (fstat (fd, &st) != 0 || !S_ISREG (st.st_mode)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
		                     _("The backend did not produce a regular file."));
		goto out;
	}
	if (!copy_length_is_complete (
	        g_file_info_get_attribute_uint64 (source_info, G_FILE_ATTRIBUTE_STANDARD_SIZE),
	        st.st_size, error)) {
		goto out;
	}
	if (!sync_copy_fd (fd, job->cancellable, error)) {
		goto out;
	}
	result = close (fd);
	fd = -1;
	if (result < 0) {
		copy_errno (errno, error);
		goto out;
	}
	ok = g_file_move (payload, staging,
	                  G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS |
	                  G_FILE_COPY_NO_FALLBACK_FOR_MOVE,
	                  job->cancellable, NULL, NULL, error);
out:
	pdata->last_size = backend_progress.last_size;
	if (backend_progress.dest_fd >= 0) {
		if (close (backend_progress.dest_fd) < 0 && ok) {
			ok = copy_errno (errno, error);
		}
	}
	if (fd >= 0) {
		if (close (fd) < 0 && ok) {
			ok = copy_errno (errno, error);
		}
	}
	if (owned) {
		if (payload && !ok) {
			discard_staging_file (pdata->job, payload);
		}
		discard_staging_file (pdata->job, container);
	}
	g_clear_object (&payload);
	g_clear_object (&container);
	return ok;
}

/* FALSE always means not completed: before publication both names are
 * untouched, and after publication the source remains until directory sync,
 * verification and cancellation checks have all succeeded. */
static gboolean
copy_move_transaction (CopyMoveJob *copy_job, GFile *src, GFile *dest,
                       gboolean same_fs, GFileCopyFlags flags, ProgressData *pdata,
                       gboolean recognize_existing, GFileInfo **compared_src,
                       GFileInfo **compared_dest, gboolean *already_present,
                       gboolean *published, GError **error)
{
	CommonJob *job = &copy_job->common;
	GFileInfo *info = NULL, *dest_info = NULL;
	GFile *staging = NULL;
	GFileOutputStream *output = NULL;
	GFileType type;
	gboolean ok = FALSE, owned = FALSE;
	gboolean atomic_move = FALSE, checksum_verified = FALSE, link_verified = FALSE;
	gboolean must_verify = copy_job->verify_after_copy || copy_job->is_move;
	NemoTransferTransaction *transaction = NULL;
	int parent_fd = -1;
	gchar *streamed_checksum = NULL;
	guint64 copied_bytes = 0;

	if (g_cancellable_set_error_if_cancelled (job->cancellable, error)) {
		return FALSE;
	}
	g_clear_pointer (&copy_job->transfer_undo, nemo_transfer_undo_unref);
	if (!nemo_transfer_guard_check (copy_job->transfer_guard, error)) {
		return FALSE;
	}
	info = query_copy_source (src, job->cancellable, error);
	if (info == NULL) {
		goto out;
	}
	type = g_file_info_get_file_type (info);
	if (copy_job->is_move) {
		if (move_without_fallback (copy_job, src, dest, flags, job->cancellable, published, error)) {
			ok = TRUE;
			atomic_move = TRUE;
			goto out;
		}
		if (*published) {
			goto out;
		}
		if (!nemo_transfer_guard_can_copy_fallback (copy_job->transfer_guard)) {
			goto out;
		}
		if (!IS_IO_ERROR (*error, NOT_SUPPORTED) &&
		    !IS_IO_ERROR (*error, WOULD_RECURSE) &&
		    !IS_IO_ERROR (*error, WOULD_MERGE) &&
		    !IS_IO_ERROR (*error, IS_DIRECTORY)) {
			goto out;
		}
		g_clear_error (error);
		flags |= G_FILE_COPY_ALL_METADATA;
	}
	dest_info = query_copy_source (dest, job->cancellable, error);
	if (dest_info == NULL && *error) {
		if (!IS_IO_ERROR (*error, NOT_FOUND)) {
			goto out;
		}
		g_clear_error (error);
	}
	if (dest_info != NULL &&
	    !nemo_transfer_guard_expect (copy_job->transfer_guard, dest, error)) {
		goto out;
	}
	if (recognize_existing && dest_info != NULL) {
		if (*compared_src != NULL) {
			/* Reuse hashes only while both snapshots still describe the
			 * files the user saw before authorizing replacement. */
			if (!copy_info_unchanged (*compared_src, info) ||
			    !copy_info_unchanged (*compared_dest, dest_info)) {
				g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
				                     _("The conflicting files changed while awaiting your decision. "
				                       "The destination has not been replaced."));
				goto out;
			}
		} else if (type != G_FILE_TYPE_DIRECTORY) {
			VerifyResult verdict = compare_existing_copy (job, src, dest, info, dest_info, error);
			if (verdict == VERIFY_RESULT_UNAVAILABLE) {
				goto out;
			}
			if (verdict == VERIFY_RESULT_MATCH) {
				if (!nemo_transfer_sync_existing (copy_job->transfer_guard, dest,
				                                  job->cancellable, error) ||
				    !copy_source_unchanged (dest, dest_info, job->cancellable, error)) {
					goto out;
				}
				copy_job->result.existing_verified_regular_files += type == G_FILE_TYPE_REGULAR;
				copy_job->result.existing_verified_symlinks += type == G_FILE_TYPE_SYMBOLIC_LINK;
				pdata->source_info->num_bytes = MAX (0, pdata->source_info->num_bytes -
					g_file_info_get_size (info));
				*already_present = TRUE;
				ok = TRUE;
				goto out;
			}
			*compared_src = g_object_ref (info);
			*compared_dest = g_object_ref (dest_info);
		}
	}
	if (dest_info != NULL && !(flags & G_FILE_COPY_OVERWRITE)) {
		g_set_error_literal (error, G_IO_ERROR,
		                     recognize_existing && type == G_FILE_TYPE_DIRECTORY &&
		                     g_file_info_get_file_type (dest_info) == G_FILE_TYPE_DIRECTORY ?
		                     G_IO_ERROR_WOULD_MERGE : G_IO_ERROR_EXISTS, _("File exists"));
		goto out;
	}
	/* Never delete an existing tree (or file) to make room for a different
	 * type before the replacement is ready. Folder-to-folder is a merge. */
	if (dest_info && (type == G_FILE_TYPE_DIRECTORY) !=
	                 (g_file_info_get_file_type (dest_info) == G_FILE_TYPE_DIRECTORY)) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		                     _("A file and a folder cannot safely replace each other. "
		                       "Choose a different name for the copy."));
		goto out;
	}
	if (type == G_FILE_TYPE_DIRECTORY) {
		g_set_error_literal (error, G_IO_ERROR,
		                     dest_info ? G_IO_ERROR_WOULD_MERGE : G_IO_ERROR_WOULD_RECURSE,
		                     _("The folder contents must be copied individually."));
		goto out;
	}
	if (type != G_FILE_TYPE_REGULAR && type != G_FILE_TYPE_SYMBOLIC_LINK) {
		g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
		                     _("This file type cannot be safely copied."));
		goto out;
	}
	/* Open the parent before writing so its error-tracking lifetime also
	 * covers the rename, rather than opening a fresh fd after publication. */
	transaction = nemo_transfer_transaction_new (copy_job->transfer_guard, src, dest,
	                                             job->cancellable, error);
	if (transaction == NULL) {
		goto out;
	}
	parent_fd = open_dest_directory (nemo_transfer_transaction_destination (transaction),
	                                 job->cancellable, error);
	if (*error) {
		goto out;
	}
	for (int attempt = 0; attempt < 16; attempt++) {
		g_clear_object (&staging);
		staging = g_object_ref (nemo_transfer_transaction_stage (transaction));
		if (type == G_FILE_TYPE_SYMBOLIC_LINK) {
			const char *target = g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET);
			if (target == NULL) {
				g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				                     _("Could not read the symbolic link target."));
				goto out;
			}
			owned = g_file_make_symbolic_link (staging, target, job->cancellable, error);
		} else {
			output = g_file_create (staging, G_FILE_CREATE_PRIVATE, job->cancellable, error);
			owned = output != NULL;
		}
		if (owned || !IS_IO_ERROR (*error, EXISTS)) {
			break;
		}
		if (attempt < 15) {
			g_clear_error (error);
			nemo_transfer_transaction_free (transaction);
			transaction = nemo_transfer_transaction_new (copy_job->transfer_guard, src, dest,
			                                             job->cancellable, error);
			if (transaction == NULL) {
				goto out;
			}
		}
	}
	if (!owned) {
		goto out;
	}
	if (!nemo_transfer_transaction_stage_created (transaction, error)) {
		goto out;
	}
	if (output) {
		GError *close_error = NULL;
		/* Copies retain their source. For a move without any change token,
		 * keep the independent source read before removing that source. */
		gboolean hash_stream = must_verify &&
		                       (!copy_job->is_move || copy_source_has_version (info));
		GChecksum *checksum = hash_stream ? g_checksum_new (G_CHECKSUM_SHA256) : NULL;
		ok = write_copy_contents (nemo_transfer_transaction_source (transaction), staging,
		                          info, output, flags, pdata,
		                          checksum, &copied_bytes, error);
		if (ok && checksum != NULL) {
			streamed_checksum = g_strdup (g_checksum_get_string (checksum));
		}
		if (checksum != NULL) {
			g_checksum_free (checksum);
		}
		gboolean native_pull = !ok && IS_IO_ERROR (*error, NOT_SUPPORTED) && parent_fd >= 0;
		if (!g_output_stream_close (G_OUTPUT_STREAM (output), job->cancellable, &close_error)) {
			if (ok || native_pull) {
				g_clear_error (error);
				g_propagate_error (error, close_error);
				ok = FALSE;
			} else {
				if (!IS_IO_ERROR (close_error, CANCELLED)) {
					g_warning ("Could not close incomplete copy: %s", close_error->message);
				}
				g_error_free (close_error);
			}
			native_pull = FALSE;
		}
		pdata->dest_fd = -1;
		g_clear_object (&output);
		if (native_pull && !job_aborted (job)) {
			g_clear_error (error);
			ok = copy_with_native_backend (nemo_transfer_transaction_source (transaction),
			                               staging, info, flags, pdata, parent_fd, error);
			if (ok) {
				ok = nemo_transfer_transaction_stage_created (transaction, error);
			}
		}
		if (!ok) {
			goto out;
		}
		ok = FALSE;
	} else if (!copy_optional_attributes (copy_job, nemo_transfer_transaction_source (transaction),
	                                     staging, flags, job->cancellable, error)) {
		goto out;
	}
	if (!copy_source_unchanged (src, info, job->cancellable, error)) {
		goto out;
	}
	if (must_verify && type == G_FILE_TYPE_REGULAR) {
		nemo_progress_info_set_details (job->progress, _("Verifying the copy"));
		guint64 minimum_size = MAX (copied_bytes,
			g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_STANDARD_SIZE));
		VerifyResult result = verify_copied_file (job, src, staging, minimum_size,
		                                         streamed_checksum);
		if (g_cancellable_set_error_if_cancelled (job->cancellable, error)) {
			goto out;
		}
		if (result != VERIFY_RESULT_MATCH) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			                     result == VERIFY_RESULT_MISMATCH
			                     ? _("The copy's SHA-256 checksum does not match the source. "
			                         "The destination was not replaced and the source was not removed.")
			                     : _("The copy could not be verified. "
			                         "The destination was not replaced and the source was not removed."));
			goto out;
		}
		checksum_verified = TRUE;
	} else if (type == G_FILE_TYPE_SYMBOLIC_LINK) {
		GFileInfo *link = query_copy_source (staging, job->cancellable, error);
		if (link == NULL) {
			goto out;
		}
		gboolean matches = g_file_info_get_file_type (link) == G_FILE_TYPE_SYMBOLIC_LINK &&
		                   g_strcmp0 (g_file_info_get_attribute_byte_string (link, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET),
		                              g_file_info_get_attribute_byte_string (info, G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET)) == 0;
		g_object_unref (link);
		if (!matches) {
			g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
			                     _("The copied symbolic link does not match the source."));
			goto out;
		}
		link_verified = TRUE;
	}
	if (!copy_source_unchanged (src, info, job->cancellable, error) ||
	    g_cancellable_set_error_if_cancelled (job->cancellable, error)) {
		goto out;
	}
	if (recognize_existing && (flags & G_FILE_COPY_OVERWRITE) && *compared_dest != NULL &&
	    !copy_source_unchanged (dest, *compared_dest, job->cancellable, error)) {
		goto out;
	}
	if (!nemo_transfer_transaction_publish (transaction, (flags & G_FILE_COPY_OVERWRITE) != 0,
	                                        published, job->cancellable, error)) {
		goto out;
	}
	owned = FALSE;
	*published = TRUE;
	nemo_file_changes_queue_file_added (dest);
	if (parent_fd >= 0 && !sync_copy_fd (parent_fd, job->cancellable, error)) {
		goto out;
	}
	if (parent_fd >= 0) {
		int result = close (parent_fd);
		parent_fd = -1;
		if (result < 0) {
			copy_errno (errno, error);
			goto out;
		}
	}
	if (g_cancellable_set_error_if_cancelled (job->cancellable, error)) {
		goto out;
	}
	if (!nemo_transfer_transaction_finish (transaction, error)) {
		goto out;
	}
	if (copy_job->is_move &&
	    (!copy_source_unchanged (src, info, job->cancellable, error) ||
	     !nemo_transfer_transaction_retire_source (transaction, job->cancellable, error))) {
		goto out;
	}
	ok = TRUE;
	if (job->undo_info != NULL) {
		copy_job->transfer_undo = nemo_transfer_transaction_undo (transaction, copy_job->is_move);
	}

out:
	if (ok && !*already_present) {
		copy_job->result.completed_items++;
		if (!atomic_move) {
			copy_job->result.completed_regular_files += type == G_FILE_TYPE_REGULAR;
			copy_job->result.completed_symlinks += type == G_FILE_TYPE_SYMBOLIC_LINK;
		}
		copy_job->result.atomic_moves += atomic_move;
		copy_job->result.checksum_verified_files += checksum_verified;
		copy_job->result.verified_symlinks += link_verified;
	}
	pdata->dest_fd = -1;
	g_free (streamed_checksum);
	g_clear_object (&output);
	if (parent_fd >= 0) {
		close (parent_fd);
	}
	nemo_transfer_transaction_free (transaction);
	g_clear_object (&staging);
	g_clear_object (&info);
	g_clear_object (&dest_info);
	return ok;
}
#endif /* NEMO_SMPL */

/* Debuting files is non-NULL only for toplevel items */
static void
copy_move_file (CopyMoveJob *copy_job,
		GFile *src,
		GFile *dest_dir,
		gboolean same_fs,
		gboolean unique_names,
		GFile *precomputed_dest,
		char **dest_fs_type,
		SourceInfo *source_info,
		TransferInfo *transfer_info,
		GHashTable *debuting_files,
		GdkPoint *position,
		gboolean overwrite,
		gboolean *skipped_file,
		gboolean readonly_source_fs)
{
	GFile *dest, *new_dest;
	GError *error;
	GFileCopyFlags flags;
	char *primary, *secondary, *details;
	int response;
	ProgressData pdata;
	gboolean would_recurse, is_merge;
	CommonJob *job;
	gboolean res;
	int unique_name_nr;
	gboolean handled_invalid_filename;
    gboolean target_is_desktop, source_is_desktop;
#ifdef NEMO_SMPL
	gboolean published = FALSE;
	gboolean already_present = FALSE, retained_regular = FALSE;
	g_autoptr (GFileInfo) compared_src = NULL;
	g_autoptr (GFileInfo) compared_dest = NULL;
#endif

	job = (CommonJob *)copy_job;

	if (should_skip_file (job, src)) {
		*skipped_file = TRUE;
#ifdef NEMO_SMPL
		record_incomplete_copy (copy_job, src, TRUE);
#endif
		return;
	}

    target_is_desktop = (copy_job->desktop_location != NULL &&
                         g_file_equal (copy_job->desktop_location, dest_dir));

    source_is_desktop = FALSE;

    if (src != NULL) {
        GFile *parent = g_file_get_parent (src);

        if (parent != NULL) {
            if (g_file_equal (copy_job->desktop_location, parent)) {
                source_is_desktop = TRUE;
            }
            g_object_unref (parent);
        }
    }

	unique_name_nr = 1;

	/* another file in the same directory might have handled the invalid
	 * filename condition for us
	 */
	handled_invalid_filename = *dest_fs_type != NULL;

	if (unique_names) {
		dest = get_unique_target_file (src, dest_dir, same_fs, *dest_fs_type, unique_name_nr++, job->cancellable);
	} else if (precomputed_dest != NULL) {
		/* Set by the cross-filesystem move fallback when the user already
		 * picked a new name during move_file_prepare. Reuse that exact
		 * destination so the conflict dialog is not shown a second time. */
		dest = g_object_ref (precomputed_dest);
	} else if (copy_job->target_name != NULL && debuting_files != NULL) {
		dest = get_target_file_with_custom_name (src, dest_dir, *dest_fs_type, same_fs,
							 copy_job->target_name, job->cancellable);
	} else {
		dest = get_target_file (src, dest_dir, *dest_fs_type, same_fs, job->cancellable);
	}

	/* Don't allow recursive move/copy into itself.
	 * (We would get a file system error if we proceeded but it is nicer to
	 * detect and report it at this level) */
	if (test_dir_is_parent (dest_dir, src)) {
		if (job->skip_all_error) {
			goto out;
		}

		/*  the run_warning() frees all strings passed in automatically  */
		primary = copy_job->is_move ? g_strdup (_("You cannot move a folder into itself."))
					    : g_strdup (_("You cannot copy a folder into itself."));
		secondary = g_strdup (_("The destination folder is inside the source folder."));

		response = run_warning (job,
					primary,
					secondary,
					NULL,
					(source_info->num_files - transfer_info->num_files) > 1,
					GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
					NULL);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) { /* skip all */
			job->skip_all_error = TRUE;
		} else if (response == 2) { /* skip */
			/* do nothing */
		} else {
			g_assert_not_reached ();
		}

		goto out;
	}

	/* Don't allow copying over the source or one of the parents of the source.
	 */
	if (test_dir_is_parent (src, dest)) {
		if (job->skip_all_error) {
			goto out;
		}

		/*  the run_warning() frees all strings passed in automatically  */
		primary = copy_job->is_move ? g_strdup (_("You cannot move a file over itself."))
					    : g_strdup (_("You cannot copy a file over itself."));
		secondary = g_strdup (_("The source file would be overwritten by the destination."));

		response = run_warning (job,
					primary,
					secondary,
					NULL,
					(source_info->num_files - transfer_info->num_files) > 1,
					GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
					NULL);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) { /* skip all */
			job->skip_all_error = TRUE;
		} else if (response == 2) { /* skip */
			/* do nothing */
		} else {
			g_assert_not_reached ();
		}

		goto out;
	}


 retry:

	error = NULL;
	flags = G_FILE_COPY_NOFOLLOW_SYMLINKS;
	if (overwrite) {
		flags |= G_FILE_COPY_OVERWRITE;
	}
	if (readonly_source_fs) {
		flags |= G_FILE_COPY_TARGET_DEFAULT_PERMS;
	}

	pdata.job = copy_job;
	pdata.last_size = 0;
	pdata.source_info = source_info;
	pdata.transfer_info = transfer_info;
	pdata.dest = (!copy_job->is_move && !same_fs) ? dest : NULL;
	pdata.dest_fd = -1;
	pdata.last_flush_offset = 0;
	pdata.flush_errno = 0;

#ifdef NEMO_SMPL
	published = FALSE;
	res = copy_move_transaction (copy_job, src, dest, same_fs, flags, &pdata,
	                             !copy_job->is_move && copy_job->verify_after_copy &&
	                             !unique_names && !job->auto_rename_all,
	                             &compared_src, &compared_dest, &already_present,
	                             &published, &error);
	if (!res && !published) {
		transfer_info->num_bytes -= pdata.last_size;
	}
	/* A post-publication failure must never enter a retry or replacement
	 * path. The destination is real user data now, and the source stays. */
	if (!res && published) {
		nemo_transfer_guard_describe_destination (copy_job->transfer_guard, dest);
		record_incomplete_copy (copy_job, src, TRUE);
		if (!job_aborted (job) && !job->skip_all_error) {
			response = run_warning (
				job, g_strdup (_("The operation was not completed.")),
				g_strdup (_("A transfer entry was moved or published, but completion could not "
				            "be confirmed. Retained files and recovery locations are listed "
				            "in the operation details. Do not repeat the operation blindly.")),
				error->message, TRUE, GTK_STOCK_CANCEL, SKIP_ALL, SKIP, NULL);
			if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
				abort_job (job);
			} else if (response == 1) {
				job->skip_all_error = TRUE;
			}
		}
		g_clear_error (&error);
		goto out;
	}
#else
	if (copy_job->is_move) {
		res = g_file_move (src, dest,
				   flags,
				   job->cancellable,
				   copy_file_progress_callback,
				   &pdata,
				   &error);
	} else {
		res = g_file_copy (src, dest,
				   flags,
				   job->cancellable,
				   copy_file_progress_callback,
				   &pdata,
				   &error);
	}

	/* g_file_copy() can report success while writeback to the device has
	 * already failed underneath it, because the bytes only had to reach
	 * the page cache. Treat that as the copy failure it is. */
	if (res && pdata.flush_errno != 0) {
		g_clear_error (&error);
		error = g_error_new (G_IO_ERROR,
		                     g_io_error_from_errno (pdata.flush_errno),
		                     _("Error while writing to the destination: %s"),
		                     g_strerror (pdata.flush_errno));
		res = FALSE;
	}

	/* On success, start writeback for whatever is still dirty so the
	 * fsync below has little left to do. */
	if (res && pdata.dest_fd >= 0) {
		sync_file_range (pdata.dest_fd, 0, 0, SYNC_FILE_RANGE_WRITE);
	}
	/* Clean up the fd the progress callback may have opened.
	 * On success, drop the page cache we no longer need. */
	if (pdata.dest_fd >= 0) {
		if (res) {
			posix_fadvise (pdata.dest_fd, 0, 0,
			               POSIX_FADV_DONTNEED);
		}
		close (pdata.dest_fd);
		pdata.dest_fd = -1;
	}

#endif /* NEMO_SMPL */

	if (res) {
#ifndef NEMO_SMPL
		if (copy_job->verify_after_copy && !job_aborted (job) &&
		    !copy_job->verify_skip_all) {
			GFileType ft = g_file_query_file_type (src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
			if (ft == G_FILE_TYPE_REGULAR) {
				VerifyResult verdict = verify_copied_file (job, src, dest, 0, NULL);

				if (verdict != VERIFY_RESULT_MATCH && !job_aborted (job)) {
					char *src_name = g_file_get_basename (src);
					char *primary_msg, *secondary_msg;
					int verify_response;

					if (verdict == VERIFY_RESULT_MISMATCH) {
						primary_msg = g_strdup_printf (_("Verification failed for \"%s\""), src_name);
						secondary_msg = g_strdup (
							_("The SHA-256 checksum of the copy does not match the original. "
							  "The file may be corrupted."));
					} else {
						/* Not evidence of corruption -- we simply could
						 * not read one of the two files back. Common for
						 * phones and cameras, where the source stops
						 * being readable as soon as it has been copied. */
						primary_msg = g_strdup_printf (_("Could not verify \"%s\""), src_name);
						secondary_msg = g_strdup (
							_("The file was copied, but it could not be read back to "
							  "compare it against the original."));
					}

					verify_response = run_warning (
						job, primary_msg, secondary_msg, NULL,
						(source_info->num_files - transfer_info->num_files) > 1,
						GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
						NULL);

					g_free (src_name);

					if (verify_response == 0 || verify_response == GTK_RESPONSE_DELETE_EVENT) {
						abort_job (job);
					} else if (verify_response == 1) { /* skip all */
						/* Stop asking: on an unverifiable source this
						 * would otherwise prompt once per file. */
						copy_job->verify_skip_all = TRUE;
					}
				}
			}
		}
#endif
		transfer_info->num_files ++;
		report_copy_progress (copy_job, source_info, transfer_info);

#ifdef NEMO_SMPL
		if (already_present) {
			if (debuting_files) {
				g_hash_table_replace (debuting_files, g_object_ref (dest), GINT_TO_POINTER (FALSE));
			}
			/* No undo entry: undoing this job must not delete an older copy. */
			g_object_unref (dest);
			return;
		}
#endif
        if (debuting_files) {
            if (target_is_desktop && position) {
                nemo_file_changes_queue_schedule_position_set (dest, *position, job->monitor_num);
            } else if (source_is_desktop && copy_job->is_move) {
                nemo_file_changes_queue_schedule_position_remove (dest);
            }

            g_hash_table_replace (debuting_files, g_object_ref (dest), GINT_TO_POINTER (TRUE));
        }

		if (copy_job->is_move) {
            gchar *src_uri = g_file_get_uri (src);
            gchar *dest_uri = g_file_get_uri (dest);

            if (!eel_uri_is_favorite (src_uri)) {
                xapp_favorites_rename (xapp_favorites_get_default (), src_uri, dest_uri);
            }

            g_free (src_uri);
            g_free (dest_uri);
			nemo_file_changes_queue_file_moved (src, dest);
		} else {
			nemo_file_changes_queue_file_added (dest);
		}

		/* If copying a trusted desktop file to the desktop,
		   mark it as trusted. */
		if (copy_job->desktop_location != NULL &&
		    g_file_equal (copy_job->desktop_location, dest_dir) &&
		    is_trusted_desktop_file (src, job->cancellable)) {
			mark_desktop_file_trusted (job,
						   job->cancellable,
						   dest,
						   FALSE);
		}

		if (job->undo_info != NULL) {
#ifdef NEMO_SMPL
			nemo_file_undo_info_ext_add_transfer (NEMO_FILE_UNDO_INFO_EXT (job->undo_info),
			                                     src, dest, copy_job->transfer_undo);
			g_clear_pointer (&copy_job->transfer_undo, nemo_transfer_undo_unref);
#else
			nemo_file_undo_info_ext_add_origin_target_pair (NEMO_FILE_UNDO_INFO_EXT (job->undo_info),
									    src, dest);
#endif
#ifdef NEMO_SMPL
			copy_job->undo_has_changes = TRUE;
#endif
		}

		g_object_unref (dest);
		return;
	}

	if (!handled_invalid_filename &&
	    IS_IO_ERROR (error, INVALID_FILENAME)) {
		handled_invalid_filename = TRUE;

		g_assert (*dest_fs_type == NULL);
		*dest_fs_type = query_fs_type (dest_dir, job->cancellable);

		if (unique_names) {
			new_dest = get_unique_target_file (src, dest_dir, same_fs, *dest_fs_type, unique_name_nr, job->cancellable);
		} else {
#ifdef NEMO_SMPL
			char *name = g_file_get_basename (dest);
			new_dest = get_target_file_with_custom_name (
				src, dest_dir, *dest_fs_type, same_fs, name, job->cancellable);
			g_free (name);
#else
			new_dest = get_target_file (src, dest_dir, *dest_fs_type, same_fs, job->cancellable);
#endif
		}

		if (!g_file_equal (dest, new_dest)) {
			g_object_unref (dest);
			dest = new_dest;

			g_error_free (error);
			goto retry;
		} else {
			g_object_unref (new_dest);
		}
	}

	/* Conflict */
	if (!overwrite &&
	    IS_IO_ERROR (error, EXISTS)) {
		gboolean is_a_merge;
		ConflictResponseData *resp;

		g_error_free (error);

		if (unique_names || job->auto_rename_all) {
			g_object_unref (dest);
			dest = get_unique_target_file (src, dest_dir, same_fs, *dest_fs_type, unique_name_nr++, job->cancellable);
			goto retry;
		}

		is_a_merge = FALSE;

#ifdef NEMO_SMPL
		if (!copy_conflict_is_merge (copy_job, src, dest, &is_a_merge, &retained_regular)) {
			goto out;
		}
		if (is_a_merge) {
			overwrite = TRUE;
			goto retry;
		}
#else
		if (is_dir (dest, job->cancellable) && is_dir (src, job->cancellable)) {
			is_a_merge = TRUE;
		}
#endif

		if ((is_a_merge && job->merge_all) ||
		    (!is_a_merge && job->replace_all)) {
			overwrite = TRUE;
			goto retry;
		}

		if (job->skip_all_conflict) {
			goto out;
		}

		resp = run_conflict_dialog (job, src, dest, dest_dir);

		if (resp->id == GTK_RESPONSE_CANCEL ||
		    resp->id == GTK_RESPONSE_DELETE_EVENT) {
			conflict_response_data_free (resp);
			abort_job (job);
		} else if (resp->id == CONFLICT_RESPONSE_SKIP) {
			if (resp->apply_to_all) {
				job->skip_all_conflict = TRUE;
			}
			conflict_response_data_free (resp);
		} else if (resp->id == CONFLICT_RESPONSE_REPLACE) { /* merge/replace */
			if (resp->apply_to_all) {
				if (is_a_merge) {
					job->merge_all = TRUE;
				} else {
					job->replace_all = TRUE;
				}
			}
			overwrite = TRUE;
			conflict_response_data_free (resp);
			goto retry;
		} else if (resp->id == CONFLICT_RESPONSE_RENAME) {
#ifdef NEMO_SMPL
			g_clear_object (&compared_src);
			g_clear_object (&compared_dest);
			retained_regular = FALSE;
#endif
			g_object_unref (dest);
			dest = get_target_file_for_display_name (dest_dir,
								 resp->new_name);
			conflict_response_data_free (resp);
			goto retry;
		} else if (resp->id == CONFLICT_RESPONSE_AUTO_RENAME) {
#ifdef NEMO_SMPL
			g_clear_object (&compared_src);
			g_clear_object (&compared_dest);
			retained_regular = FALSE;
#endif
			if (resp->apply_to_all) {
				job->auto_rename_all = TRUE;
			}
			unique_names = TRUE;
			conflict_response_data_free (resp);
			goto retry;
		} else {
			g_assert_not_reached ();
		}
	}

#ifndef NEMO_SMPL
	else if (overwrite &&
		 IS_IO_ERROR (error, IS_DIRECTORY)) {

		g_error_free (error);

		if (remove_target_recursively (job, src, dest, dest)) {
			goto retry;
		}
	}
#endif

	/* Needs to recurse */
	else if (IS_IO_ERROR (error, WOULD_RECURSE) ||
		 IS_IO_ERROR (error, WOULD_MERGE)) {
		is_merge = error->code == G_IO_ERROR_WOULD_MERGE;
		would_recurse = error->code == G_IO_ERROR_WOULD_RECURSE;
		g_error_free (error);

#ifndef NEMO_SMPL
		if (overwrite && would_recurse) {
			error = NULL;

			/* Copying a dir onto file, first remove the file */
			if (!file_delete_wrapper (dest, job->cancellable, &error) &&
			    !IS_IO_ERROR (error, NOT_FOUND)) {
				if (job->skip_all_error) {
					g_error_free (error);
					goto out;
				}
				if (copy_job->is_move) {
					primary = f (_("Error while moving \"%B\"."), src);
				} else {
					primary = f (_("Error while copying \"%B\"."), src);
				}
				secondary = f (_("Could not remove the already existing file with the same name in %F."), dest_dir);
				details = error->message;

				/* setting TRUE on show_all here, as we could have
				 * another error on the same file later.
				 */
				response = run_warning (job,
							primary,
							secondary,
							details,
							TRUE,
							GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
							NULL);

				g_error_free (error);

				if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
					abort_job (job);
				} else if (response == 1) { /* skip all */
					job->skip_all_error = TRUE;
				} else if (response == 2) { /* skip */
					/* do nothing */
				} else {
					g_assert_not_reached ();
				}
				goto out;

			}
			if (error) {
				g_error_free (error);
				error = NULL;
			}
			nemo_file_changes_queue_file_removed (dest);
		}
#endif

		if (is_merge) {
			/* On merge we now write in the target directory, which may not
			   be in the same directory as the source, even if the parent is
			   (if the merged directory is a mountpoint). This could cause
			   problems as we then don't transcode filenames.
			   We just set same_fs to FALSE which is safe but a bit slower. */
			same_fs = FALSE;
		}

		if (!copy_move_directory (copy_job, src, &dest, same_fs,
					  would_recurse, dest_fs_type,
					  source_info, transfer_info,
					  debuting_files, skipped_file,
					  readonly_source_fs)) {
			/* Retry a corrected name or a directory created concurrently. */
#ifndef NEMO_SMPL
			g_assert (*dest_fs_type != NULL);
#endif
			handled_invalid_filename = *dest_fs_type != NULL;
			goto retry;
		}

		g_object_unref (dest);
		return;
	}

	else if (IS_IO_ERROR (error, CANCELLED)) {
		g_error_free (error);
	}

	/* Other error */
	else {
#ifdef NEMO_SMPL
		record_incomplete_copy (copy_job, src, TRUE);
#endif
		if (job->skip_all_error) {
			g_error_free (error);
			goto out;
		}
		primary = f (_("Error while copying \"%B\"."), src);
		secondary = f (_("There was an error copying the file into %F."), dest_dir);
		details = error->message;

		response = run_warning (job,
					primary,
					secondary,
					details,
					(source_info->num_files - transfer_info->num_files) > 1,
					GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
					NULL);

		g_error_free (error);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) { /* skip all */
			job->skip_all_error = TRUE;
		} else if (response == 2) { /* skip */
			/* do nothing */
		} else {
			g_assert_not_reached ();
		}
	}
 out:
	*skipped_file = TRUE; /* Or aborted, but same-same */
#ifdef NEMO_SMPL
	record_incomplete_copy (copy_job, src, FALSE);
	if (retained_regular && copy_job->incomplete_paths != NULL &&
	    GPOINTER_TO_INT (g_hash_table_lookup (copy_job->incomplete_paths, src)) == 1) {
		gboolean merge;
		if (copy_conflict_is_merge (copy_job, src, dest, &merge, &retained_regular) &&
		    retained_regular) {
			g_hash_table_replace (copy_job->incomplete_paths, g_object_ref (src), GINT_TO_POINTER (3));
		}
	}
#endif
	g_object_unref (dest);
}

static void
copy_files (CopyMoveJob *job,
	    const char *dest_fs_id,
	    SourceInfo *source_info,
	    TransferInfo *transfer_info)
{
	CommonJob *common;
	GList *l;
	GFile *src;
	gboolean same_fs;
	int i;
	GdkPoint *point;
	gboolean skipped_file;
	gboolean unique_names;
	GFile *dest;
	GFile *source_dir;
	char *dest_fs_type;
	GFileInfo *inf;
	gboolean readonly_source_fs;

	dest_fs_type = NULL;
	readonly_source_fs = FALSE;

	common = &job->common;

	report_copy_progress (job, source_info, transfer_info);

	/* Query the source dir, not the file because if its a symlink we'll follow it */
	source_dir = g_file_get_parent ((GFile *) job->files->data);
	if (source_dir) {
		inf = g_file_query_filesystem_info (source_dir, "filesystem::readonly", common->cancellable, NULL);
		if (inf != NULL) {
			readonly_source_fs = g_file_info_get_attribute_boolean (inf, "filesystem::readonly");
			g_object_unref (inf);
		}
		g_object_unref (source_dir);
	}

	unique_names = (job->destination == NULL);
	i = 0;
	for (l = job->files;
	     l != NULL && !job_aborted (common);
	     l = l->next) {
		src = l->data;

		if (i < job->n_icon_positions) {
			point = &job->icon_positions[i];
		} else {
			point = NULL;
		}


		same_fs = FALSE;
		if (dest_fs_id) {
			same_fs = has_fs_id (src, dest_fs_id, common->cancellable);
		}

		if (job->destination) {
			dest = g_object_ref (job->destination);
		} else {
			dest = g_file_get_parent (src);

		}
		if (dest) {
			skipped_file = FALSE;
			copy_move_file (job, src, dest,
					same_fs, unique_names,
					NULL,
					&dest_fs_type,
					source_info, transfer_info,
					job->debuting_files,
					point, FALSE, &skipped_file,
					readonly_source_fs);
			g_object_unref (dest);
		}
#ifdef NEMO_SMPL
		else {
			record_incomplete_copy (job, src, TRUE);
		}
#endif
		i++;
	}

	g_free (dest_fs_type);
}

static gboolean
copy_job_done (gpointer user_data)
{
	CopyMoveJob *job;

	job = user_data;
#ifdef NEMO_SMPL
	GError *release_error = NULL;
	if (!nemo_transfer_guard_release (job->transfer_guard, &release_error)) {
		record_incomplete_copy (job, job->files->data, TRUE);
		g_clear_error (&release_error);
	}
	set_copy_move_result (job);
	if (!job->undo_has_changes) {
		g_clear_object (&job->common.undo_info);
	}
#endif
	if (job->done_callback) {
		job->done_callback (job->debuting_files,
				    !job_aborted ((CommonJob *) job)
#ifdef NEMO_SMPL
				    && job->result.outcome == NEMO_PROGRESS_OUTCOME_SUCCESS
#endif
				    ,
				    job->done_callback_data);
	}

	g_list_free_full (job->files, g_object_unref);
	if (job->destination) {
		g_object_unref (job->destination);
	}
	if (job->desktop_location) {
		g_object_unref (job->desktop_location);
	}
	g_hash_table_unref (job->debuting_files);
	g_free (job->icon_positions);
	g_free (job->target_name);

	g_clear_object (&job->fake_display_source);
#ifdef NEMO_SMPL
	g_clear_pointer (&job->transfer_undo, nemo_transfer_undo_unref);
	g_clear_pointer (&job->transfer_guard, nemo_transfer_guard_unref);
#endif

	finalize_common ((CommonJob *)job);

	nemo_file_changes_consume_changes (TRUE);
	return FALSE;
}

static gboolean
copy_job (GIOSchedulerJob *io_job,
	  GCancellable *cancellable,
	  gpointer user_data)
{
	CopyMoveJob *job;
	CommonJob *common;
	SourceInfo source_info;
	TransferInfo transfer_info;
	char *dest_fs_id;
	GFile *dest;

	job = user_data;
	common = &job->common;
	common->io_job = io_job;

	dest_fs_id = NULL;

    nemo_progress_info_start (common->progress);

#ifdef NEMO_SMPL
	if (!transfer_preflight (job)) {
		goto aborted;
	}
#endif
	scan_sources (job->files,
		      &source_info,
		      common,
		      OP_KIND_COPY);
	if (job_aborted (common)) {
		goto aborted;
	}

	if (job->destination) {
		dest = g_object_ref (job->destination);
	} else {
		/* Duplication, no dest,
		 * use source for free size, etc
		 */
		dest = g_file_get_parent (job->files->data);
	}

	verify_destination (&job->common,
			    dest,
			    &dest_fs_id,
			    source_info.num_bytes);
	g_object_unref (dest);
	if (job_aborted (common)) {
		goto aborted;
	}

	nemo_progress_info_start (common->progress);

	memset (&transfer_info, 0, sizeof (transfer_info));
	copy_files (job,
		    dest_fs_id,
		    &source_info, &transfer_info);

 aborted:

	g_free (dest_fs_id);

	g_io_scheduler_job_send_to_mainloop_async (io_job,
						   copy_job_done,
						   job,
						   NULL);

	return FALSE;
}

void
nemo_file_operations_copy_file (GFile *source_file,
				    GFile *target_dir,
				    const gchar *source_display_name,
				    const gchar *new_name,
				    GtkWindow *parent_window,
				    NemoCopyCallback done_callback,
				    gpointer done_callback_data)
{
	CopyMoveJob *job;

	job = op_job_new (CopyMoveJob, parent_window);
	job->done_callback = done_callback;
	job->done_callback_data = done_callback_data;
	job->verify_after_copy = consume_copy_verification ();
	job->desktop_location = nemo_get_desktop_location ();
	job->files = g_list_append (NULL, g_object_ref (source_file));
	job->destination = g_object_ref (target_dir);
	job->target_name = g_strdup (new_name);
	job->debuting_files = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, NULL);

	if (source_display_name != NULL) {
		gchar *path;

		path = g_build_filename ("/", source_display_name, NULL);
		job->fake_display_source = g_file_new_for_path (path);

		g_free (path);
	}

	inhibit_power_manager ((CommonJob *)job, _("Copying Files"));

    generate_initial_job_details (job->common.progress, OP_KIND_COPY, job->files, job->destination);

#ifdef NEMO_SMPL
	job->transfer_guard = nemo_transfer_guard_new (job->files, job->destination, FALSE);
#endif
    add_job_to_job_queue (copy_job, job, job->common.cancellable, job->common.progress, OP_KIND_COPY);
}

void
nemo_file_operations_copy (GList *files,
			       GArray *relative_item_points,
			       GFile *target_dir,
			       GtkWindow *parent_window,
			       NemoCopyCallback  done_callback,
			       gpointer done_callback_data)
{
	CopyMoveJob *job;

	job = op_job_new (CopyMoveJob, parent_window);
	job->desktop_location = nemo_get_desktop_location ();
	job->done_callback = done_callback;
	job->done_callback_data = done_callback_data;
	job->verify_after_copy = consume_copy_verification ();
	job->files = eel_g_object_list_copy (files);
	job->destination = g_object_ref (target_dir);
	if (relative_item_points != NULL &&
	    relative_item_points->len > 0) {
		job->icon_positions =
			g_memdup (relative_item_points->data,
				  sizeof (GdkPoint) * relative_item_points->len);
		job->n_icon_positions = relative_item_points->len;
	}
	job->debuting_files = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, NULL);

	inhibit_power_manager ((CommonJob *)job, _("Copying Files"));

	if (!nemo_file_undo_manager_pop_flag ()) {
		GFile* src_dir;

		src_dir = g_file_get_parent (files->data);
		job->common.undo_info = nemo_file_undo_info_ext_new (NEMO_FILE_UNDO_OP_COPY,
									 g_list_length (files),
									 src_dir, target_dir);

		g_object_unref (src_dir);
	}

    generate_initial_job_details (job->common.progress, OP_KIND_COPY, job->files, job->destination);

#ifdef NEMO_SMPL
	job->transfer_guard = nemo_transfer_guard_new (job->files, job->destination, FALSE);
#endif
    add_job_to_job_queue (copy_job, job, job->common.cancellable, job->common.progress, OP_KIND_COPY);
}

static void
report_move_progress (CopyMoveJob *move_job, int total, int left)
{
	CommonJob *job;

	job = (CommonJob *)move_job;

	nemo_progress_info_take_status (job->progress,
					    f (_("Preparing to Move to \"%B\""),
					       move_job->destination));

    if (nemo_progress_info_get_is_paused (job->progress)) {
        nemo_progress_info_set_details (job->progress, _("Paused"));
    } else {
    	nemo_progress_info_take_details (job->progress,
    					     f (ngettext ("Preparing to move %'d file",
    							  "Preparing to move %'d files",
    							  left), left));
    }

	nemo_progress_info_pulse_progress (job->progress);
}

typedef struct {
	GFile *file;
	/* If set, the copy+delete move phase must use this destination
	 * (e.g. after the user picked a new name in move_file_prepare). */
	GFile *precomputed_dest;
	gboolean overwrite;
	gboolean has_position;
	GdkPoint position;
} MoveFileCopyFallback;

static void
move_file_copy_fallback_free (gpointer data)
{
	MoveFileCopyFallback *fallback = data;

	g_clear_object (&fallback->precomputed_dest);
	g_free (fallback);
}

static MoveFileCopyFallback *
move_copy_file_callback_new (GFile *file,
			     GFile *precomputed_dest,
			     gboolean overwrite,
			     GdkPoint *position)
{
	MoveFileCopyFallback *fallback;

	fallback = g_new (MoveFileCopyFallback, 1);
	fallback->file = file;
	fallback->precomputed_dest = precomputed_dest != NULL
		? g_object_ref (precomputed_dest)
		: NULL;
	fallback->overwrite = overwrite;
	if (position) {
		fallback->has_position = TRUE;
		fallback->position = *position;
	} else {
		fallback->has_position = FALSE;
	}

	return fallback;
}

static GList *
get_files_from_fallbacks (GList *fallbacks)
{
	MoveFileCopyFallback *fallback;
	GList *res, *l;

	res = NULL;
	for (l = fallbacks; l != NULL; l = l->next) {
		fallback = l->data;
		res = g_list_prepend (res, fallback->file);
	}
	return g_list_reverse (res);
}

static void
move_file_prepare (CopyMoveJob *move_job,
		   GFile *src,
		   GFile *dest_dir,
		   gboolean same_fs,
		   char **dest_fs_type,
		   GHashTable *debuting_files,
		   GdkPoint *position,
		   GList **fallback_files,
		   int files_left)
{
	GFile *dest, *new_dest;
	GError *error;
	CommonJob *job;
	gboolean overwrite;
	gboolean auto_rename;
	char *primary, *secondary, *details;
	int response;
	GFileCopyFlags flags;
	MoveFileCopyFallback *fallback;
	gboolean handled_invalid_filename;
    gboolean target_is_desktop, source_is_desktop;
	int unique_name_nr = 1;
#ifdef NEMO_SMPL
	gboolean fallback_scheduled = FALSE;
	gboolean native_state_changed = FALSE;
#endif

    target_is_desktop = (move_job->desktop_location != NULL &&
                         g_file_equal (move_job->desktop_location, dest_dir));

    source_is_desktop = FALSE;

    if (src != NULL) {
        GFile *parent = g_file_get_parent (src);

        if (parent != NULL && g_file_equal (move_job->desktop_location, parent)) {
            source_is_desktop = TRUE;
            g_object_unref (parent);
        }
    }

	overwrite = FALSE;
    auto_rename = FALSE;

	handled_invalid_filename = *dest_fs_type != NULL;

	job = (CommonJob *)move_job;

	dest = get_target_file (src, dest_dir, *dest_fs_type, same_fs, job->cancellable);


	/* Don't allow recursive move/copy into itself.
	 * (We would get a file system error if we proceeded but it is nicer to
	 * detect and report it at this level) */
	if (test_dir_is_parent (dest_dir, src)) {
		if (job->skip_all_error) {
			goto out;
		}

		/*  the run_warning() frees all strings passed in automatically  */
		primary = move_job->is_move ? g_strdup (_("You cannot move a folder into itself."))
					    : g_strdup (_("You cannot copy a folder into itself."));
		secondary = g_strdup (_("The destination folder is inside the source folder."));

		response = run_warning (job,
					primary,
					secondary,
					NULL,
					files_left > 1,
					GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
					NULL);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) { /* skip all */
			job->skip_all_error = TRUE;
		} else if (response == 2) { /* skip */
			/* do nothing */
		} else {
			g_assert_not_reached ();
		}

		goto out;
	}

 retry:

	flags = G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_NO_FALLBACK_FOR_MOVE;
	if (overwrite) {
		flags |= G_FILE_COPY_OVERWRITE;
	}

	error = NULL;
#ifdef NEMO_SMPL
	g_clear_pointer (&move_job->transfer_undo, nemo_transfer_undo_unref);
	native_state_changed = FALSE;
	if (move_without_fallback (move_job, src, dest, flags, job->cancellable,
	                           &native_state_changed, &error)) {
		move_job->result.completed_items++;
		move_job->result.atomic_moves++;
#else
	if (g_file_move (src, dest,
			 flags,
			 job->cancellable,
			 NULL,
			 NULL,
			 &error)) {
#endif

		if (debuting_files) {
			g_hash_table_replace (debuting_files, g_object_ref (dest), GINT_TO_POINTER (TRUE));
		}

        gchar *src_uri = g_file_get_uri (src);
        gchar *dest_uri = g_file_get_uri (dest);

        if (!eel_uri_is_favorite (src_uri)) {
            xapp_favorites_rename (xapp_favorites_get_default (), src_uri, dest_uri);
        }

        g_free (src_uri);
        g_free (dest_uri);

		nemo_file_changes_queue_file_moved (src, dest);

        if (target_is_desktop && position) {
            nemo_file_changes_queue_schedule_position_set (dest, *position, job->monitor_num);
        } else if (source_is_desktop) {
            nemo_file_changes_queue_schedule_position_remove (dest);
        }

		if (job->undo_info != NULL) {
#ifdef NEMO_SMPL
			nemo_file_undo_info_ext_add_transfer (NEMO_FILE_UNDO_INFO_EXT (job->undo_info),
			                                     src, dest, move_job->transfer_undo);
			g_clear_pointer (&move_job->transfer_undo, nemo_transfer_undo_unref);
			move_job->undo_has_changes = TRUE;
#else
			nemo_file_undo_info_ext_add_origin_target_pair (NEMO_FILE_UNDO_INFO_EXT (job->undo_info),
									    src, dest);
#endif
		}

		g_object_unref (dest);
		return;
	}

#ifdef NEMO_SMPL
	if (native_state_changed) {
		goto move_error;
	}
#endif
	if (IS_IO_ERROR (error, INVALID_FILENAME) &&
	    !handled_invalid_filename) {
		handled_invalid_filename = TRUE;

		g_assert (*dest_fs_type == NULL);
		*dest_fs_type = query_fs_type (dest_dir, job->cancellable);

#ifdef NEMO_SMPL
		char *name = g_file_get_basename (dest);
		new_dest = get_target_file_with_custom_name (
			src, dest_dir, *dest_fs_type, same_fs, name, job->cancellable);
		g_free (name);
#else
		new_dest = get_target_file (src, dest_dir, *dest_fs_type, same_fs, job->cancellable);
#endif
		if (!g_file_equal (dest, new_dest)) {
			g_object_unref (dest);
			dest = new_dest;
			g_clear_error (&error);
			goto retry;
		} else {
			g_object_unref (new_dest);
			goto move_error;
		}
	}

	/* Conflict */
	else if (!overwrite &&
		 IS_IO_ERROR (error, EXISTS)) {
		gboolean is_merge;
		ConflictResponseData *resp;

		g_error_free (error);

		is_merge = FALSE;
#ifdef NEMO_SMPL
		if (!copy_conflict_is_merge (move_job, src, dest, &is_merge, NULL)) {
			goto out;
		}
		if (is_merge && !job->auto_rename_all && !auto_rename) {
			overwrite = TRUE;
			goto retry;
		}
#else
		if (is_dir (dest, job->cancellable) && is_dir (src, job->cancellable)) {
			is_merge = TRUE;
		}
#endif

		if ((is_merge && job->merge_all) ||
		    (!is_merge && job->replace_all)) {
			overwrite = TRUE;
			goto retry;
		}

		if (job->auto_rename_all || auto_rename) {
			g_object_unref (dest);
			dest = get_unique_target_file (src, dest_dir, same_fs, *dest_fs_type, unique_name_nr++, job->cancellable);
			goto retry;
		}

		if (job->skip_all_conflict) {
			goto out;
		}

		resp = run_conflict_dialog (job, src, dest, dest_dir);

		if (resp->id == GTK_RESPONSE_CANCEL ||
		    resp->id == GTK_RESPONSE_DELETE_EVENT) {
			conflict_response_data_free (resp);
			abort_job (job);
		} else if (resp->id == CONFLICT_RESPONSE_SKIP) {
			if (resp->apply_to_all) {
				job->skip_all_conflict = TRUE;
			}
			conflict_response_data_free (resp);
		} else if (resp->id == CONFLICT_RESPONSE_REPLACE) { /* merge/replace */
			if (resp->apply_to_all) {
				if (is_merge) {
					job->merge_all = TRUE;
				} else {
					job->replace_all = TRUE;
				}
			}
			overwrite = TRUE;
			conflict_response_data_free (resp);
			goto retry;
		} else if (resp->id == CONFLICT_RESPONSE_RENAME) {
			g_object_unref (dest);
			dest = get_target_file_for_display_name (dest_dir,
								 resp->new_name);
			conflict_response_data_free (resp);
			goto retry;
		} else if (resp->id == CONFLICT_RESPONSE_AUTO_RENAME) {
			if (resp->apply_to_all) {
				job->auto_rename_all = TRUE;
			}
			auto_rename = TRUE;
			conflict_response_data_free (resp);
			goto retry;
		} else {
			g_assert_not_reached ();
		}
	}

	else if (IS_IO_ERROR (error, WOULD_RECURSE) ||
		 IS_IO_ERROR (error, WOULD_MERGE) ||
		 (IS_IO_ERROR (error, NOT_SUPPORTED)
#ifdef NEMO_SMPL
		  && nemo_transfer_guard_can_copy_fallback (move_job->transfer_guard)
#endif
		 ) ||
		 (overwrite && IS_IO_ERROR (error, IS_DIRECTORY))) {
		g_error_free (error);

		fallback = move_copy_file_callback_new (src,
							dest,
							overwrite,
							position);
		*fallback_files = g_list_prepend (*fallback_files, fallback);
#ifdef NEMO_SMPL
		fallback_scheduled = TRUE;
#endif
	}

	else if (IS_IO_ERROR (error, CANCELLED)) {
		g_error_free (error);
	}

	/* Other error */
	else {
	move_error:
#ifdef NEMO_SMPL
		record_incomplete_copy (move_job, src, TRUE);
#endif
		if (job->skip_all_error || job_aborted (job)) {
			g_error_free (error);
			goto out;
		}
		primary = f (_("Error while moving \"%B\"."), src);
		secondary = f (_("There was an error moving the file into %F."), dest_dir);
		details = error->message;

		response = run_warning (job,
					primary,
					secondary,
					details,
					files_left > 1,
					GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
					NULL);

		g_error_free (error);

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (job);
		} else if (response == 1) { /* skip all */
			job->skip_all_error = TRUE;
		} else if (response == 2) { /* skip */
			/* do nothing */
		} else {
			g_assert_not_reached ();
		}
	}

 out:
#ifdef NEMO_SMPL
	if (!fallback_scheduled) {
		record_incomplete_copy (move_job, src, FALSE);
	}
#endif
	g_object_unref (dest);
}

static void
move_files_prepare (CopyMoveJob *job,
		    const char *dest_fs_id,
		    char **dest_fs_type,
		    GList **fallbacks)
{
	CommonJob *common;
	GList *l;
	GFile *src;
	gboolean same_fs;
	int i;
	GdkPoint *point;
	int total, left;

	common = &job->common;

	total = left = g_list_length (job->files);

	report_move_progress (job, total, left);

	i = 0;
	for (l = job->files;
	     l != NULL && !job_aborted (common);
	     l = l->next) {
		src = l->data;

		if (i < job->n_icon_positions) {
			point = &job->icon_positions[i];
		} else {
			point = NULL;
		}


		same_fs = FALSE;
		if (dest_fs_id) {
			same_fs = has_fs_id (src, dest_fs_id, common->cancellable);
		}

		move_file_prepare (job, src, job->destination,
				   same_fs, dest_fs_type,
				   job->debuting_files,
				   point,
				   fallbacks,
				   left);
		report_move_progress (job, total, --left);
		i++;
	}

	*fallbacks = g_list_reverse (*fallbacks);


}

static void
move_files (CopyMoveJob *job,
	    GList *fallbacks,
	    const char *dest_fs_id,
	    char **dest_fs_type,
	    SourceInfo *source_info,
	    TransferInfo *transfer_info)
{
	CommonJob *common;
	GList *l;
	GFile *src;
	gboolean same_fs;
	int i;
	GdkPoint *point;
	gboolean skipped_file;
	MoveFileCopyFallback *fallback;
common = &job->common;

	report_copy_progress (job, source_info, transfer_info);

	i = 0;
	for (l = fallbacks;
	     l != NULL && !job_aborted (common);
	     l = l->next) {
		fallback = l->data;
		src = fallback->file;

		if (fallback->has_position) {
			point = &fallback->position;
		} else {
			point = NULL;
		}

		same_fs = FALSE;
		if (dest_fs_id) {
			same_fs = has_fs_id (src, dest_fs_id, common->cancellable);
		}

		/* Set overwrite to true, as the user has
		   selected overwrite on all toplevel items */
		skipped_file = FALSE;
		copy_move_file (job, src, job->destination,
				same_fs, FALSE,
				fallback->precomputed_dest,
				dest_fs_type,
				source_info, transfer_info,
				job->debuting_files,
				point, fallback->overwrite, &skipped_file, FALSE);
		i++;
	}
}


static gboolean
move_job_done (gpointer user_data)
{
	CopyMoveJob *job;

	job = user_data;
#ifdef NEMO_SMPL
	GError *release_error = NULL;
	if (!nemo_transfer_guard_release (job->transfer_guard, &release_error)) {
		record_incomplete_copy (job, job->files->data, TRUE);
		g_clear_error (&release_error);
	}
	set_copy_move_result (job);
	if (!job->undo_has_changes) {
		g_clear_object (&job->common.undo_info);
	}
#endif
	if (job->done_callback) {
		job->done_callback (job->debuting_files,
				    !job_aborted ((CommonJob *) job)
#ifdef NEMO_SMPL
				    && job->result.outcome == NEMO_PROGRESS_OUTCOME_SUCCESS
#endif
				    ,
				    job->done_callback_data);
	}

	g_list_free_full (job->files, g_object_unref);
	g_object_unref (job->destination);
	g_hash_table_unref (job->debuting_files);
	g_free (job->icon_positions);
#ifdef NEMO_SMPL
	g_clear_pointer (&job->transfer_undo, nemo_transfer_undo_unref);
	g_clear_pointer (&job->transfer_guard, nemo_transfer_guard_unref);
#endif

	finalize_common ((CommonJob *)job);

	nemo_file_changes_consume_changes (TRUE);
	return FALSE;
}

static gboolean
move_job (GIOSchedulerJob *io_job,
	  GCancellable *cancellable,
	  gpointer user_data)
{
	CopyMoveJob *job;
	CommonJob *common;
	GList *fallbacks;
	SourceInfo source_info;
	TransferInfo transfer_info;
	char *dest_fs_id;
	char *dest_fs_type;
	GList *fallback_files;

	job = user_data;
	common = &job->common;
	common->io_job = io_job;

	dest_fs_id = NULL;
	dest_fs_type = NULL;

	fallbacks = NULL;

    nemo_progress_info_start (common->progress);

#ifdef NEMO_SMPL
	if (!transfer_preflight (job)) {
		goto aborted;
	}
#endif
	verify_destination (&job->common,
			    job->destination,
			    &dest_fs_id,
			    -1);
	if (job_aborted (common)) {
		goto aborted;
	}

	/* This moves all files that we can do without copy + delete */
	move_files_prepare (job, dest_fs_id, &dest_fs_type, &fallbacks);
	if (job_aborted (common)) {
		goto aborted;
	}

	/* The rest we need to do deep copy + delete behind on,
	   so scan for size */

	fallback_files = get_files_from_fallbacks (fallbacks);
	scan_sources (fallback_files,
		      &source_info,
		      common,
		      OP_KIND_MOVE);

	g_list_free (fallback_files);

	if (job_aborted (common)) {
		goto aborted;
	}

	verify_destination (&job->common,
			    job->destination,
			    NULL,
			    source_info.num_bytes);
	if (job_aborted (common)) {
		goto aborted;
	}

	memset (&transfer_info, 0, sizeof (transfer_info));
	move_files (job,
		    fallbacks,
		    dest_fs_id, &dest_fs_type,
		    &source_info, &transfer_info);

 aborted:
	g_list_free_full (fallbacks, move_file_copy_fallback_free);

	g_free (dest_fs_id);
	g_free (dest_fs_type);

	g_io_scheduler_job_send_to_mainloop (io_job,
					     move_job_done,
					     job,
					     NULL);

	return FALSE;
}

void
nemo_file_operations_move (GList *files,
			       GArray *relative_item_points,
			       GFile *target_dir,
			       GtkWindow *parent_window,
			       NemoCopyCallback  done_callback,
			       gpointer done_callback_data)
{
	CopyMoveJob *job;

	job = op_job_new (CopyMoveJob, parent_window);
	job->is_move = TRUE;
    job->desktop_location = nemo_get_desktop_location ();
	job->done_callback = done_callback;
	job->done_callback_data = done_callback_data;
	job->verify_after_copy = consume_copy_verification ();
	job->files = eel_g_object_list_copy (files);
	job->destination = g_object_ref (target_dir);
	if (relative_item_points != NULL &&
	    relative_item_points->len > 0) {
		job->icon_positions =
			g_memdup (relative_item_points->data,
				  sizeof (GdkPoint) * relative_item_points->len);
		job->n_icon_positions = relative_item_points->len;
	}
	job->debuting_files = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, NULL);

	inhibit_power_manager ((CommonJob *)job, _("Moving Files"));

	if (!nemo_file_undo_manager_pop_flag ()) {
		GFile* src_dir;

		src_dir = g_file_get_parent (files->data);

		if (g_file_has_uri_scheme (g_list_first (files)->data, "trash")) {
			job->common.undo_info = nemo_file_undo_info_ext_new (NEMO_FILE_UNDO_OP_RESTORE_FROM_TRASH,
										 g_list_length (files),
										 src_dir, target_dir);
		} else {
			job->common.undo_info = nemo_file_undo_info_ext_new (NEMO_FILE_UNDO_OP_MOVE,
										 g_list_length (files),
										 src_dir, target_dir);
		}

		g_object_unref (src_dir);
	}

    generate_initial_job_details (job->common.progress, OP_KIND_MOVE, job->files, job->destination);

#ifdef NEMO_SMPL
	job->transfer_guard = nemo_transfer_guard_new (job->files, job->destination, TRUE);
#endif
    add_job_to_job_queue (move_job, job, job->common.cancellable, job->common.progress, OP_KIND_MOVE);
}

static void
report_link_progress (CopyMoveJob *link_job, int total, int left)
{
	CommonJob *job;

	job = (CommonJob *)link_job;

	nemo_progress_info_take_status (job->progress,
					    f (_("Creating links in \"%B\""),
					       link_job->destination));

	nemo_progress_info_take_details (job->progress,
					     f (ngettext ("Making link to %'d file",
							  "Making links to %'d files",
							  left), left));

	nemo_progress_info_set_progress (job->progress, left, total);
}

static char *
get_abs_path_for_symlink (GFile *file)
{
	GFile *root, *parent;
	char *relative, *abs;

	if (g_file_is_native (file)) {
		return g_file_get_path (file);
	}

	root = g_object_ref (file);
	while ((parent = g_file_get_parent (root)) != NULL) {
		g_object_unref (root);
		root = parent;
	}

	relative = g_file_get_relative_path (root, file);
	g_object_unref (root);
	abs = g_strconcat ("/", relative, NULL);
	g_free (relative);
	return abs;
}


static void
link_file (CopyMoveJob *job,
	   GFile *src, GFile *dest_dir,
	   char **dest_fs_type,
	   GHashTable *debuting_files,
	   GdkPoint *position,
	   int files_left)
{
	GFile *src_dir, *dest, *new_dest;
	int count;
	char *path;
	gboolean not_local;
	GError *error;
	CommonJob *common;
	char *primary, *secondary, *details;
	int response;
	gboolean handled_invalid_filename;
    gboolean target_is_desktop;

    target_is_desktop = (job->desktop_location != NULL &&
                         g_file_equal (job->desktop_location, dest_dir));

	common = (CommonJob *)job;

	count = 0;

	src_dir = g_file_get_parent (src);
	if (g_file_equal (src_dir, dest_dir)) {
		count = 1;
	}
	g_object_unref (src_dir);

	handled_invalid_filename = *dest_fs_type != NULL;

	dest = get_target_file_for_link (src, dest_dir, *dest_fs_type, count, common->cancellable);

 retry:
	error = NULL;
	not_local = FALSE;

	path = get_abs_path_for_symlink (src);
	if (path == NULL) {
		not_local = TRUE;
	} else if (g_file_make_symbolic_link (dest,
					      path,
					      common->cancellable,
					      &error)) {

		if (common->undo_info != NULL) {
			nemo_file_undo_info_ext_add_origin_target_pair (NEMO_FILE_UNDO_INFO_EXT (common->undo_info),
									    src, dest);
		}

		g_free (path);
		if (debuting_files) {
			g_hash_table_replace (debuting_files, g_object_ref (dest), GINT_TO_POINTER (TRUE));
		}

		nemo_file_changes_queue_file_added (dest);

        if (target_is_desktop && position) {
            nemo_file_changes_queue_schedule_position_set (dest, *position, common->monitor_num);
        }

		g_object_unref (dest);

		return;
	}
	g_free (path);

	if (error != NULL &&
	    IS_IO_ERROR (error, INVALID_FILENAME) &&
	    !handled_invalid_filename) {
		handled_invalid_filename = TRUE;

		g_assert (*dest_fs_type == NULL);
		*dest_fs_type = query_fs_type (dest_dir, common->cancellable);

		new_dest = get_target_file_for_link (src, dest_dir, *dest_fs_type, count, common->cancellable);

		if (!g_file_equal (dest, new_dest)) {
			g_object_unref (dest);
			dest = new_dest;
			g_error_free (error);

			goto retry;
		} else {
			g_object_unref (new_dest);
		}
	}
	/* Conflict */
	if (error != NULL && IS_IO_ERROR (error, EXISTS)) {
		g_object_unref (dest);
		dest = get_target_file_for_link (src, dest_dir, *dest_fs_type, count++, common->cancellable);
		g_error_free (error);
		goto retry;
	}

	else if (error != NULL && IS_IO_ERROR (error, CANCELLED)) {
		g_error_free (error);
	}

	/* Other error */
	else {
		if (common->skip_all_error) {
			goto out;
		}
		primary = f (_("Error while creating link to %B."), src);
		if (not_local) {
			secondary = f (_("Symbolic links only supported for local files"));
			details = NULL;
		} else if (error != NULL && IS_IO_ERROR (error, NOT_SUPPORTED)) {
			secondary = f (_("The target doesn't support symbolic links."));
			details = NULL;
		} else {
			secondary = f (_("There was an error creating the symlink in %F."), dest_dir);
			details = error->message;
		}

		response = run_warning (common,
					primary,
					secondary,
					details,
					files_left > 1,
					GTK_STOCK_CANCEL, SKIP_ALL, SKIP,
					NULL);

		if (error) {
			g_error_free (error);
		}

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (common);
		} else if (response == 1) { /* skip all */
			common->skip_all_error = TRUE;
		} else if (response == 2) { /* skip */
			/* do nothing */
		} else {
			g_assert_not_reached ();
		}
	}

 out:
	g_object_unref (dest);
}

static gboolean
link_job_done (gpointer user_data)
{
	CopyMoveJob *job;

	job = user_data;
	if (job->done_callback) {
		job->done_callback (job->debuting_files,
				    !job_aborted ((CommonJob *) job),
				    job->done_callback_data);
	}

	g_list_free_full (job->files, g_object_unref);
	g_object_unref (job->destination);
	g_hash_table_unref (job->debuting_files);
	g_free (job->icon_positions);

	finalize_common ((CommonJob *)job);

	nemo_file_changes_consume_changes (TRUE);
	return FALSE;
}

static gboolean
link_job (GIOSchedulerJob *io_job,
	  GCancellable *cancellable,
	  gpointer user_data)
{
	CopyMoveJob *job;
	CommonJob *common;
	GFile *src;
	GdkPoint *point;
	char *dest_fs_type;
	int total, left;
	int i;
	GList *l;

	job = user_data;
	common = &job->common;
	common->io_job = io_job;

	dest_fs_type = NULL;

    nemo_progress_info_start (common->progress);

	verify_destination (&job->common,
			    job->destination,
			    NULL,
			    -1);
	if (job_aborted (common)) {
		goto aborted;
	}

	total = left = g_list_length (job->files);

	report_link_progress (job, total, left);

	i = 0;
	for (l = job->files;
	     l != NULL && !job_aborted (common);
	     l = l->next) {
		src = l->data;

		if (i < job->n_icon_positions) {
			point = &job->icon_positions[i];
		} else {
			point = NULL;
		}


		link_file (job, src, job->destination,
			   &dest_fs_type, job->debuting_files,
			   point, left);
		report_link_progress (job, total, --left);
		i++;

	}

 aborted:
	g_free (dest_fs_type);

	g_io_scheduler_job_send_to_mainloop (io_job,
					     link_job_done,
					     job,
					     NULL);

	return FALSE;
}

void
nemo_file_operations_link (GList *files,
			       GArray *relative_item_points,
			       GFile *target_dir,
			       GtkWindow *parent_window,
			       NemoCopyCallback  done_callback,
			       gpointer done_callback_data)
{
	CopyMoveJob *job;

	job = op_job_new (CopyMoveJob, parent_window);
	job->done_callback = done_callback;
	job->done_callback_data = done_callback_data;
	job->files = eel_g_object_list_copy (files);
	job->destination = g_object_ref (target_dir);
	if (relative_item_points != NULL &&
	    relative_item_points->len > 0) {
		job->icon_positions =
			g_memdup (relative_item_points->data,
				  sizeof (GdkPoint) * relative_item_points->len);
		job->n_icon_positions = relative_item_points->len;
	}
	job->debuting_files = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, NULL);

	if (!nemo_file_undo_manager_pop_flag ()) {
		GFile* src_dir;

		src_dir = g_file_get_parent (files->data);
		job->common.undo_info = nemo_file_undo_info_ext_new (NEMO_FILE_UNDO_OP_CREATE_LINK,
									 g_list_length (files),
									 src_dir, target_dir);
		g_object_unref (src_dir);
	}

    generate_initial_job_details (job->common.progress, OP_KIND_LINK, job->files, job->destination);

    add_job_to_job_queue (link_job, job, job->common.cancellable, job->common.progress, OP_KIND_LINK);
}


void
nemo_file_operations_duplicate (GList *files,
				    GArray *relative_item_points,
				    GtkWindow *parent_window,
				    NemoCopyCallback  done_callback,
				    gpointer done_callback_data)
{
	CopyMoveJob *job;

	job = op_job_new (CopyMoveJob, parent_window);
    job->desktop_location = nemo_get_desktop_location ();
	job->done_callback = done_callback;
	job->done_callback_data = done_callback_data;
	job->verify_after_copy = consume_copy_verification ();
	job->files = eel_g_object_list_copy (files);
	job->destination = NULL;
	if (relative_item_points != NULL &&
	    relative_item_points->len > 0) {
		job->icon_positions =
			g_memdup (relative_item_points->data,
				  sizeof (GdkPoint) * relative_item_points->len);
		job->n_icon_positions = relative_item_points->len;
	}
	job->debuting_files = g_hash_table_new_full (g_file_hash, (GEqualFunc)g_file_equal, g_object_unref, NULL);

	if (!nemo_file_undo_manager_pop_flag ()) {
		GFile* src_dir;

		src_dir = g_file_get_parent (files->data);
		job->common.undo_info =
			nemo_file_undo_info_ext_new (NEMO_FILE_UNDO_OP_DUPLICATE,
							 g_list_length (files),
							 src_dir, src_dir);
		g_object_unref (src_dir);
	}

    GFile *src_dir = g_file_get_parent (files->data);
    generate_initial_job_details (job->common.progress, OP_KIND_DUPE, job->files, src_dir);
    g_object_unref (src_dir);

#ifdef NEMO_SMPL
	job->transfer_guard = nemo_transfer_guard_new (job->files, job->destination, FALSE);
#endif
    add_job_to_job_queue (copy_job, job, job->common.cancellable, job->common.progress, OP_KIND_DUPE);
}

static gboolean
set_permissions_job_done (gpointer user_data)
{
	SetPermissionsJob *job;

	job = user_data;

	g_object_unref (job->file);

	if (job->done_callback) {
		job->done_callback (!job_aborted ((CommonJob *) job),
				    job->done_callback_data);
	}

	finalize_common ((CommonJob *)job);
	return FALSE;
}

static void
set_permissions_file (SetPermissionsJob *job,
		      GFile *file,
		      GFileInfo *info)
{
	CommonJob *common;
	GFileInfo *child_info;
	gboolean free_info;
	guint32 current;
	guint32 value;
	guint32 mask;
	GFileEnumerator *enumerator;
	GFile *child;

	common = (CommonJob *)job;

	nemo_progress_info_pulse_progress (common->progress);

	free_info = FALSE;
	if (info == NULL) {
		free_info = TRUE;
		info = g_file_query_info (file,
					  G_FILE_ATTRIBUTE_STANDARD_TYPE","
					  G_FILE_ATTRIBUTE_UNIX_MODE,
					  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
					  common->cancellable,
					  NULL);
		/* Ignore errors */
		if (info == NULL) {
			return;
		}
	}

	if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
		value = job->dir_permissions;
		mask = job->dir_mask;
	} else {
		value = job->file_permissions;
		mask = job->file_mask;
	}


	if (!job_aborted (common) &&
	    g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
		current = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE);

		if (common->undo_info != NULL) {
			nemo_file_undo_info_rec_permissions_add_file (NEMO_FILE_UNDO_INFO_REC_PERMISSIONS (common->undo_info),
									  file, current);
		}

		current = (current & ~mask) | value;

		g_file_set_attribute_uint32 (file, G_FILE_ATTRIBUTE_UNIX_MODE,
					     current, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
					     common->cancellable, NULL);
	}

	if (!job_aborted (common) &&
	    g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
		enumerator = g_file_enumerate_children (file,
							G_FILE_ATTRIBUTE_STANDARD_NAME","
							G_FILE_ATTRIBUTE_STANDARD_TYPE","
							G_FILE_ATTRIBUTE_UNIX_MODE,
							G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
							common->cancellable,
							NULL);
		if (enumerator) {
			while (!job_aborted (common) &&
			       (child_info = g_file_enumerator_next_file (enumerator, common->cancellable, NULL)) != NULL) {
				child = g_file_get_child (file,
							  g_file_info_get_name (child_info));
				set_permissions_file (job, child, child_info);
				g_object_unref (child);
				g_object_unref (child_info);
			}
			g_file_enumerator_close (enumerator, common->cancellable, NULL);
			g_object_unref (enumerator);
		}
	}
	if (free_info) {
		g_object_unref (info);
	}
}


static gboolean
set_permissions_job (GIOSchedulerJob *io_job,
		     GCancellable *cancellable,
		     gpointer user_data)
{
	SetPermissionsJob *job = user_data;
	CommonJob *common;

	common = (CommonJob *)job;
	common->io_job = io_job;

	nemo_progress_info_set_status (common->progress,
					   _("Setting permissions"));

    nemo_progress_info_start (common->progress);

	set_permissions_file (job, job->file, NULL);

	g_io_scheduler_job_send_to_mainloop_async (io_job,
						   set_permissions_job_done,
						   job,
						   NULL);

	return FALSE;
}



void
nemo_file_set_permissions_recursive (const char *directory,
					 guint32         file_permissions,
					 guint32         file_mask,
					 guint32         dir_permissions,
					 guint32         dir_mask,
					 NemoOpCallback  callback,
					 gpointer  callback_data)
{
	SetPermissionsJob *job;

	job = op_job_new (SetPermissionsJob, NULL);
	job->file = g_file_new_for_uri (directory);
	job->file_permissions = file_permissions;
	job->file_mask = file_mask;
	job->dir_permissions = dir_permissions;
	job->dir_mask = dir_mask;
	job->done_callback = callback;
	job->done_callback_data = callback_data;

	if (!nemo_file_undo_manager_pop_flag ()) {
		job->common.undo_info =
			nemo_file_undo_info_rec_permissions_new (job->file,
								     file_permissions, file_mask,
								     dir_permissions, dir_mask);
	}

    generate_initial_job_details (job->common.progress, OP_KIND_PERMISSIONS, NULL, job->file);

    add_job_to_job_queue (set_permissions_job, job, job->common.cancellable, job->common.progress, OP_KIND_PERMISSIONS);
}

static GList *
location_list_from_uri_list (const GList *uris)
{
	const GList *l;
	GList *files;
	GFile *f;

	files = NULL;
	for (l = uris; l != NULL; l = l->next) {
		f = g_file_new_for_uri (l->data);
		files = g_list_prepend (files, f);
	}

	return g_list_reverse (files);
}

typedef struct {
	NemoCopyCallback real_callback;
	gpointer real_data;
} MoveTrashCBData;

static void
callback_for_move_to_trash (GHashTable *debuting_uris,
			    gboolean user_cancelled,
			    MoveTrashCBData *data)
{
	if (data->real_callback)
		data->real_callback (debuting_uris, !user_cancelled, data->real_data);
	g_free (data);
}

void
nemo_file_operations_copy_move (const GList *item_uris,
				    GArray *relative_item_points,
				    const char *target_dir,
				    GdkDragAction copy_action,
				    GtkWidget *parent_view,
				    NemoCopyCallback  done_callback,
				    gpointer done_callback_data)
{
	GList *locations;
	GList *p;
	GFile *dest, *src_dir;
	GtkWindow *parent_window;
	gboolean target_is_mapping;
	gboolean have_nonmapping_source;

	dest = NULL;
	target_is_mapping = FALSE;
	have_nonmapping_source = FALSE;

	if (target_dir) {
		dest = g_file_new_for_uri (target_dir);
		if (g_file_has_uri_scheme (dest, "burn")) {
			target_is_mapping = TRUE;
                }
	}

	locations = location_list_from_uri_list (item_uris);

	for (p = locations; p != NULL; p = p->next) {
		if (!g_file_has_uri_scheme ((GFile* )p->data, "burn")) {
			have_nonmapping_source = TRUE;
		}
	}

	if (target_is_mapping && have_nonmapping_source && copy_action == GDK_ACTION_MOVE) {
		/* never move to "burn:///", but fall back to copy.
		 * This is a workaround, because otherwise the source files would be removed.
		 */
		copy_action = GDK_ACTION_COPY;
	}

	parent_window = NULL;
	if (parent_view) {
		parent_window = (GtkWindow *)gtk_widget_get_ancestor (parent_view, GTK_TYPE_WINDOW);
	}

	if (copy_action == GDK_ACTION_COPY) {
		src_dir = g_file_get_parent (locations->data);
		if (target_dir == NULL ||
		    (src_dir != NULL &&
		     g_file_equal (src_dir, dest))) {

			nemo_file_operations_duplicate (locations,
							    relative_item_points,
							    parent_window,
							    done_callback, done_callback_data);
		} else {
			nemo_file_operations_copy (locations,
						       relative_item_points,
						       dest,
						       parent_window,
						       done_callback, done_callback_data);
		}
		if (src_dir) {
			g_object_unref (src_dir);
		}

	} else if (copy_action == GDK_ACTION_MOVE) {
		if (g_file_has_uri_scheme (dest, "trash")) {
			MoveTrashCBData *cb_data;

			cb_data = g_new0 (MoveTrashCBData, 1);
			cb_data->real_callback = done_callback;
			cb_data->real_data = done_callback_data;

			nemo_file_operations_trash_or_delete (locations,
								  parent_window,
								  (NemoDeleteCallback) callback_for_move_to_trash,
								  cb_data);
		} else {

			nemo_file_operations_move (locations,
						       relative_item_points,
						       dest,
						       parent_window,
						       done_callback, done_callback_data);
		}
	} else {

		nemo_file_operations_link (locations,
					       relative_item_points,
					       dest,
					       parent_window,
					       done_callback, done_callback_data);
	}

	g_list_free_full (locations, g_object_unref);
	if (dest) {
		g_object_unref (dest);
	}
}

static gboolean
create_job_done (gpointer user_data)
{
	CreateJob *job;

	job = user_data;
	if (job->done_callback) {
		job->done_callback (job->created_file,
				    !job_aborted ((CommonJob *) job),
				    job->done_callback_data);
	}

	g_object_unref (job->dest_dir);
	if (job->src) {
		g_object_unref (job->src);
	}
	g_free (job->src_data);
	g_free (job->filename);
	if (job->created_file) {
		g_object_unref (job->created_file);
	}

	finalize_common ((CommonJob *)job);

	nemo_file_changes_consume_changes (TRUE);
	return FALSE;
}

static gboolean
create_job (GIOSchedulerJob *io_job,
	    GCancellable *cancellable,
	    gpointer user_data)
{
	CreateJob *job;
	CommonJob *common;
	int count;
	GFile *dest;
	char *basename;
	char *filename, *filename2, *new_filename;
	char *filename_base, *suffix;
	char *dest_fs_type;
	GError *error;
	gboolean res;
	gboolean filename_is_utf8;
	char *primary, *secondary, *details;
	int response;
	char *data;
	int length;
	GFileOutputStream *out;
	gboolean handled_invalid_filename;
	int max_length, offset;

	job = user_data;
	common = &job->common;
	common->io_job = io_job;

    nemo_progress_info_start (common->progress);

	handled_invalid_filename = FALSE;

	dest_fs_type = NULL;
	filename = NULL;
	dest = NULL;

	max_length = get_max_name_length (job->dest_dir);

	verify_destination (common,
			    job->dest_dir,
			    NULL, -1);
	if (job_aborted (common)) {
		goto aborted;
	}

	filename = g_strdup (job->filename);
	filename_is_utf8 = FALSE;
	if (filename) {
		filename_is_utf8 = g_utf8_validate (filename, -1, NULL);
	}
	if (filename == NULL) {
		if (job->make_dir) {
			/* localizers: the initial name of a new folder  */
			filename = g_strdup (_("Untitled Folder"));
			filename_is_utf8 = TRUE; /* Pass in utf8 */
		} else {
			if (job->src != NULL) {
				basename = g_file_get_basename (job->src);
				/* localizers: the initial name of a new template document */
				filename = g_strdup_printf ("%s", basename);

				g_free (basename);
			}
			if (filename == NULL) {
				/* localizers: the initial name of a new empty document */
				filename = g_strdup (_("Untitled Document"));
				filename_is_utf8 = TRUE; /* Pass in utf8 */
			}
		}
	}

	make_file_name_valid_for_dest_fs (filename, dest_fs_type);
	if (filename_is_utf8) {
		dest = g_file_get_child_for_display_name (job->dest_dir, filename, NULL);
	}
	if (dest == NULL) {
		dest = g_file_get_child (job->dest_dir, filename);
	}
	count = 1;

 retry:

	error = NULL;
	if (job->make_dir) {
		res = g_file_make_directory (dest,
					     common->cancellable,
					     &error);

		if (res && common->undo_info != NULL) {
			nemo_file_undo_info_create_set_data (NEMO_FILE_UNDO_INFO_CREATE (common->undo_info),
								 dest, NULL, 0);
		}

	} else {
		if (job->src) {
			res = g_file_copy (job->src,
					   dest,
					   G_FILE_COPY_NONE,
					   common->cancellable,
					   NULL, NULL,
					   &error);

			if (res && common->undo_info != NULL) {
				gchar *uri;

				uri = g_file_get_uri (job->src);
				nemo_file_undo_info_create_set_data (NEMO_FILE_UNDO_INFO_CREATE (common->undo_info),
									 dest, uri, 0);

				g_free (uri);
			}

		} else {
			data = "";
			length = 0;
			if (job->src_data) {
				data = job->src_data;
				length = job->length;
			}

			out = g_file_create (dest,
					     G_FILE_CREATE_NONE,
					     common->cancellable,
					     &error);
			if (out) {
				res = g_output_stream_write_all (G_OUTPUT_STREAM (out),
								 data, length,
								 NULL,
								 common->cancellable,
								 &error);
				if (res) {
					res = g_output_stream_close (G_OUTPUT_STREAM (out),
								     common->cancellable,
								     &error);

					if (res && common->undo_info != NULL) {
						nemo_file_undo_info_create_set_data (NEMO_FILE_UNDO_INFO_CREATE (common->undo_info),
											 dest, data, length);
					}
				}

				/* This will close if the write failed and we didn't close */
				g_object_unref (out);
			} else {
				res = FALSE;
			}
		}
	}

	if (res) {
		job->created_file = g_object_ref (dest);
		nemo_file_changes_queue_file_added (dest);
		if (job->has_position) {
			nemo_file_changes_queue_schedule_position_set (dest, job->position, common->monitor_num);
		}
	} else {
		g_assert (error != NULL);

		if (IS_IO_ERROR (error, INVALID_FILENAME) &&
		    !handled_invalid_filename) {
			handled_invalid_filename = TRUE;

			g_assert (dest_fs_type == NULL);
			dest_fs_type = query_fs_type (job->dest_dir, common->cancellable);

			g_clear_object (&dest);

			if (count == 1) {
				new_filename = g_strdup (filename);
			} else {
				filename_base = eel_filename_strip_extension (filename);
				offset = strlen (filename_base);
				suffix = g_strdup (filename + offset);

				filename2 = g_strdup_printf ("%s %d%s", filename_base, count, suffix);

				new_filename = NULL;
				if (max_length > 0 && strlen (filename2) > abs(max_length)) {
					new_filename = shorten_utf8_string (filename2, strlen (filename2) - max_length);
				}

				if (new_filename == NULL) {
					new_filename = g_strdup (filename2);
				}

				g_free (filename2);
				g_free (suffix);
			}

			if (make_file_name_valid_for_dest_fs (new_filename, dest_fs_type)) {
                g_clear_object (&dest);

				if (filename_is_utf8) {
					dest = g_file_get_child_for_display_name (job->dest_dir, new_filename, NULL);
				}
				if (dest == NULL) {
					dest = g_file_get_child (job->dest_dir, new_filename);
				}

				g_free (new_filename);
				g_error_free (error);
				goto retry;
			}
			g_free (new_filename);
		} else if (IS_IO_ERROR (error, EXISTS)) {
            g_clear_object (&dest);
			dest = NULL;
			filename_base = eel_filename_strip_extension (filename);
			offset = strlen (filename_base);
			suffix = g_strdup (filename + offset);

			filename2 = g_strdup_printf ("%s %d%s", filename_base, ++count, suffix);

			if (max_length > 0 && strlen (filename2) > abs(max_length)) {
				new_filename = shorten_utf8_string (filename2, strlen (filename2) - max_length);
				if (new_filename != NULL) {
					g_free (filename2);
					filename2 = new_filename;
				}
			}

			make_file_name_valid_for_dest_fs (filename2, dest_fs_type);
			if (filename_is_utf8) {
				dest = g_file_get_child_for_display_name (job->dest_dir, filename2, NULL);
			}
			if (dest == NULL) {
				dest = g_file_get_child (job->dest_dir, filename2);
			}
			g_free (filename2);
			g_free (suffix);
			g_error_free (error);
			goto retry;
		}

		else if (IS_IO_ERROR (error, CANCELLED)) {
			g_error_free (error);
		}

		/* Other error */
		else {
			if (job->make_dir) {
				primary = f (_("Error while creating directory %B."), dest);
			} else {
				primary = f (_("Error while creating file %B."), dest);
			}
			secondary = f (_("There was an error creating the directory in %F."), job->dest_dir);
			details = error->message;

			response = run_warning (common,
						primary,
						secondary,
						details,
						FALSE,
						GTK_STOCK_CANCEL, SKIP,
						NULL);

			g_error_free (error);

			if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
				abort_job (common);
			} else if (response == 1) { /* skip */
				/* do nothing */
			} else {
				g_assert_not_reached ();
			}
		}
	}

 aborted:
    g_clear_object (&dest);

	g_free (filename);
	g_free (dest_fs_type);
	g_io_scheduler_job_send_to_mainloop_async (io_job,
						   create_job_done,
						   job,
						   NULL);

	return FALSE;
}

void
nemo_file_operations_new_folder (GtkWidget *parent_view,
				     GdkPoint *target_point,
				     const char *parent_dir,
				     NemoCreateCallback done_callback,
				     gpointer done_callback_data)
{
	CreateJob *job;
	GtkWindow *parent_window;

	parent_window = NULL;
	if (parent_view) {
		parent_window = (GtkWindow *)gtk_widget_get_ancestor (parent_view, GTK_TYPE_WINDOW);
	}

	job = op_job_new (CreateJob, parent_window);
	job->done_callback = done_callback;
	job->done_callback_data = done_callback_data;
	job->dest_dir = g_file_new_for_uri (parent_dir);
	job->make_dir = TRUE;
	if (target_point != NULL) {
		job->position = *target_point;
		job->has_position = TRUE;
	}

	if (!nemo_file_undo_manager_pop_flag ()) {
		job->common.undo_info = nemo_file_undo_info_create_new (NEMO_FILE_UNDO_OP_CREATE_FOLDER);
	}

    add_job_to_job_queue (create_job, job, job->common.cancellable, job->common.progress, OP_KIND_CREATE);
}

void
nemo_file_operations_new_file_from_template (GtkWidget *parent_view,
						 GdkPoint *target_point,
						 const char *parent_dir,
						 const char *target_filename,
						 const char *template_uri,
						 NemoCreateCallback done_callback,
						 gpointer done_callback_data)
{
	CreateJob *job;
	GtkWindow *parent_window;

	parent_window = NULL;
	if (parent_view) {
		parent_window = (GtkWindow *)gtk_widget_get_ancestor (parent_view, GTK_TYPE_WINDOW);
	}

	job = op_job_new (CreateJob, parent_window);
	job->done_callback = done_callback;
	job->done_callback_data = done_callback_data;
	job->dest_dir = g_file_new_for_uri (parent_dir);
	if (target_point != NULL) {
		job->position = *target_point;
		job->has_position = TRUE;
	}
	job->filename = g_strdup (target_filename);

	if (template_uri) {
		job->src = g_file_new_for_uri (template_uri);
	}

	if (!nemo_file_undo_manager_pop_flag ()) {
		job->common.undo_info = nemo_file_undo_info_create_new (NEMO_FILE_UNDO_OP_CREATE_FILE_FROM_TEMPLATE);
	}

    add_job_to_job_queue (create_job, job, job->common.cancellable, job->common.progress, OP_KIND_CREATE);
}

void
nemo_file_operations_new_file (GtkWidget *parent_view,
				   GdkPoint *target_point,
				   const char *parent_dir,
				   const char *target_filename,
				   const char *initial_contents,
				   int length,
				   NemoCreateCallback done_callback,
				   gpointer done_callback_data)
{
	CreateJob *job;
	GtkWindow *parent_window;

	parent_window = NULL;
	if (parent_view) {
		parent_window = (GtkWindow *)gtk_widget_get_ancestor (parent_view, GTK_TYPE_WINDOW);
	}

	job = op_job_new (CreateJob, parent_window);
	job->done_callback = done_callback;
	job->done_callback_data = done_callback_data;
	job->dest_dir = g_file_new_for_uri (parent_dir);
	if (target_point != NULL) {
		job->position = *target_point;
		job->has_position = TRUE;
	}
	job->src_data = g_memdup (initial_contents, length);
	job->length = length;
	job->filename = g_strdup (target_filename);

	if (!nemo_file_undo_manager_pop_flag ()) {
		job->common.undo_info = nemo_file_undo_info_create_new (NEMO_FILE_UNDO_OP_CREATE_EMPTY_FILE);
	}

    add_job_to_job_queue (create_job, job, job->common.cancellable, job->common.progress, OP_KIND_CREATE);
}

static void
delete_trash_file (CommonJob *job,
		   GFile *file,
		   int *deletions_since_progress,
		   gboolean del_file,
		   gboolean del_children)
{
	GFileInfo *info;
	GFile *child;
	GFileEnumerator *enumerator;

	if (job_aborted (job)) {
		return;
	}

	if (del_children) {
        gboolean should_recurse;

        /* The g_file_delete operation works differently for locations provided
         * by the trash backend as it prevents modifications of trashed items
         * For that reason, it is enough to call g_file_delete on top-level
         * items only.
         */
        should_recurse = !g_file_has_uri_scheme (file, "trash");

		enumerator = g_file_enumerate_children (file,
							G_FILE_ATTRIBUTE_STANDARD_NAME ","
							G_FILE_ATTRIBUTE_STANDARD_TYPE,
							G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
							job->cancellable,
							NULL);
		if (enumerator) {
			while (!job_aborted (job) &&
			       (info = g_file_enumerator_next_file (enumerator, job->cancellable, NULL)) != NULL) {
                gboolean is_dir;

				child = g_file_get_child (file,
							  g_file_info_get_name (info));
                is_dir = (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY);

                delete_trash_file (job, child, deletions_since_progress, TRUE, should_recurse && is_dir);
				g_object_unref (child);
				g_object_unref (info);
			}
			g_file_enumerator_close (enumerator, job->cancellable, NULL);
			g_object_unref (enumerator);
		}
	}

	if (!job_aborted (job) && del_file) {
		file_delete_wrapper (file, job->cancellable, NULL);

		if ((*deletions_since_progress)++ > 100) {
			nemo_progress_info_pulse_progress (job->progress);
			*deletions_since_progress = 0;
		}
	}
}

static gboolean
empty_trash_job_done (gpointer user_data)
{
	EmptyTrashJob *job;

	job = user_data;

	g_list_free_full (job->trash_dirs, g_object_unref);

	if (job->done_callback) {
		job->done_callback (!job_aborted ((CommonJob *) job),
				    job->done_callback_data);
	}

	finalize_common ((CommonJob *)job);
	return FALSE;
}

static gboolean
empty_trash_job (GIOSchedulerJob *io_job,
		 GCancellable *cancellable,
		 gpointer user_data)
{
	EmptyTrashJob *job = user_data;
	CommonJob *common;
	GList *l;
	gboolean confirmed;
	int deletions_since_progress = 0;

	common = (CommonJob *)job;
	common->io_job = io_job;

    nemo_progress_info_start (common->progress);

	if (job->should_confirm && !job_aborted (common)) {
		confirmed = confirm_empty_trash (common);
	} else {
		confirmed = TRUE;
	}
	if (confirmed) {
		nemo_progress_info_set_status (common->progress, _("Emptying Trash"));
		nemo_progress_info_set_details (common->progress, _("Emptying Trash"));

		for (l = job->trash_dirs;
		     l != NULL && !job_aborted (common);
		     l = l->next) {
			delete_trash_file (common, l->data, &deletions_since_progress, FALSE, TRUE);
		}
	}

	g_io_scheduler_job_send_to_mainloop_async (io_job,
						   empty_trash_job_done,
						   job,
						   NULL);

	return FALSE;
}

void
nemo_file_operations_empty_trash (GtkWidget *parent_view)
{
	EmptyTrashJob *job;
	GtkWindow *parent_window;

	parent_window = NULL;
	if (parent_view) {
		parent_window = (GtkWindow *)gtk_widget_get_ancestor (parent_view, GTK_TYPE_WINDOW);
	}

	job = op_job_new (EmptyTrashJob, parent_window);
	job->trash_dirs = g_list_prepend (job->trash_dirs,
					  g_file_new_for_uri ("trash:"));
	job->should_confirm = TRUE;

	inhibit_power_manager ((CommonJob *)job, _("Emptying Trash"));

    generate_initial_job_details (job->common.progress, OP_KIND_EMPTY_TRASH, NULL, NULL);

    add_job_to_job_queue (empty_trash_job, job, job->common.cancellable, job->common.progress, OP_KIND_EMPTY_TRASH);
}

static gboolean
mark_trusted_job_done (gpointer user_data)
{
	MarkTrustedJob *job = user_data;

	g_object_unref (job->file);

	if (job->done_callback) {
		job->done_callback (!job_aborted ((CommonJob *) job),
				    job->done_callback_data);
	}

	finalize_common ((CommonJob *)job);
	return FALSE;
}

#define TRUSTED_SHEBANG "#!/usr/bin/env xdg-open\n"

static void
mark_desktop_file_trusted (CommonJob *common,
			   GCancellable *cancellable,
			   GFile *file,
			   gboolean interactive)
{
	char *contents, *new_contents;
	gsize length, new_length;
	GError *error;
	guint32 current_perms, new_perms;
	int response;
	GFileInfo *info;

 retry:
	error = NULL;
	if (!g_file_load_contents (file,
				  cancellable,
				  &contents, &length,
				  NULL, &error)) {
		if (interactive) {
			response = run_error (common,
					      g_strdup (_("Unable to mark launcher trusted (executable)")),
					      error->message,
					      NULL,
					      FALSE,
					      GTK_STOCK_CANCEL, RETRY,
					      NULL);
		} else {
			response = 0;
		}


		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (common);
		} else if (response == 1) {
			goto retry;
		} else {
			g_assert_not_reached ();
		}

		goto out;
	}

	if (!g_str_has_prefix (contents, "#!")) {
		new_length = length + strlen (TRUSTED_SHEBANG);
		new_contents = g_malloc (new_length);

		strcpy (new_contents, TRUSTED_SHEBANG);
		memcpy (new_contents + strlen (TRUSTED_SHEBANG),
			contents, length);

		if (!g_file_replace_contents (file,
					      new_contents,
					      new_length,
					      NULL,
					      FALSE, 0,
					      NULL, cancellable, &error)) {
			g_free (contents);
			g_free (new_contents);

			if (interactive) {
				response = run_error (common,
						      g_strdup (_("Unable to mark launcher trusted (executable)")),
						      error->message,
						      NULL,
						      FALSE,
						      GTK_STOCK_CANCEL, RETRY,
						      NULL);
			} else {
				response = 0;
			}

			if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
				abort_job (common);
			} else if (response == 1) {
				goto retry;
			} else {
				g_assert_not_reached ();
			}

			goto out;
		}
		g_free (new_contents);

	}
	g_free (contents);

	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_STANDARD_TYPE","
				  G_FILE_ATTRIBUTE_UNIX_MODE,
				  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
				  common->cancellable,
				  &error);

	if (info == NULL) {
		if (interactive) {
			response = run_error (common,
					      g_strdup (_("Unable to mark launcher trusted (executable)")),
					      error->message,
					      NULL,
					      FALSE,
					      GTK_STOCK_CANCEL, RETRY,
					      NULL);
		} else {
			response = 0;
		}

		if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
			abort_job (common);
		} else if (response == 1) {
			goto retry;
		} else {
			g_assert_not_reached ();
		}

		goto out;
	}


	if (g_file_info_has_attribute (info, G_FILE_ATTRIBUTE_UNIX_MODE)) {
		current_perms = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_MODE);
		new_perms = current_perms | S_IXGRP | S_IXUSR | S_IXOTH;

		if ((current_perms != new_perms) &&
		    !g_file_set_attribute_uint32 (file, G_FILE_ATTRIBUTE_UNIX_MODE,
						  new_perms, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
						  common->cancellable, &error))
			{
				g_object_unref (info);

				if (interactive) {
					response = run_error (common,
							      g_strdup (_("Unable to mark launcher trusted (executable)")),
							      error->message,
							      NULL,
							      FALSE,
							      GTK_STOCK_CANCEL, RETRY,
							      NULL);
				} else {
					response = 0;
				}

				if (response == 0 || response == GTK_RESPONSE_DELETE_EVENT) {
					abort_job (common);
				} else if (response == 1) {
					goto retry;
				} else {
					g_assert_not_reached ();
				}

				goto out;
			}
	}
	g_object_unref (info);
 out:
	;
}

static gboolean
mark_trusted_job (GIOSchedulerJob *io_job,
		  GCancellable *cancellable,
		  gpointer user_data)
{
	MarkTrustedJob *job = user_data;
	CommonJob *common;

	common = (CommonJob *)job;
	common->io_job = io_job;

    nemo_progress_info_start (common->progress);

	mark_desktop_file_trusted (common,
				   cancellable,
				   job->file,
				   job->interactive);

	g_io_scheduler_job_send_to_mainloop_async (io_job,
						   mark_trusted_job_done,
						   job,
						   NULL);

	return FALSE;
}

void
nemo_file_mark_desktop_file_trusted (GFile *file,
					 GtkWindow *parent_window,
					 gboolean interactive,
					 NemoOpCallback done_callback,
					 gpointer done_callback_data)
{
	MarkTrustedJob *job;

	job = op_job_new (MarkTrustedJob, parent_window);
	job->file = g_object_ref (file);
	job->interactive = interactive;
	job->done_callback = done_callback;
	job->done_callback_data = done_callback_data;

    add_job_to_job_queue (mark_trusted_job, job, job->common.cancellable, job->common.progress, OP_KIND_PERMISSIONS);
}

#if 0
#define DEBUG_FILE_OP_QUEUE
#endif

static gboolean
job_is_local (GList *files, GFile *destination)
{
    gboolean ret = FALSE;

    NemoFile *source = nemo_file_get_existing (G_FILE (files->data));

    if (source == NULL)
        return FALSE;

    NemoFile *dest = destination != NULL ? nemo_file_get_existing (destination) : NULL;

    if (dest != NULL) {
        ret = nemo_file_is_local (source) &&
              nemo_file_is_local (dest);
    } else {
        ret = nemo_file_is_local (source);
    }

    nemo_file_unref (source);
    nemo_file_unref (dest);

#ifdef DEBUG_FILE_OP_QUEUE
    g_message ("File op job is local: %s\n", ret ? "TRUE" : "FALSE");
#endif

    return ret;
}

static gboolean
job_is_same_fs (GList *files, GFile *destination)
{
    gboolean ret = FALSE;

    NemoFile *source = nemo_file_get_existing (G_FILE (files->data));

    if (source == NULL)
        return FALSE;

    NemoFile *dest = nemo_file_get_existing (destination);

    if (dest != NULL) {
        gchar *src_fs_id = nemo_file_get_filesystem_id (source);
        gchar *dst_fs_id = nemo_file_get_filesystem_id (dest);

        if (g_strcmp0 (src_fs_id, dst_fs_id) == 0)
            ret = TRUE;

#ifdef DEBUG_FILE_OP_QUEUE
        g_message ("File op job is same filesystem (src: %s, dst: %s): %s\n", src_fs_id, dst_fs_id, ret ? "TRUE" : "FALSE");
#endif

        g_free (src_fs_id);
        g_free (dst_fs_id);
    }

    nemo_file_unref (source);
    nemo_file_unref (dest);

    return ret;
}

static gboolean
job_has_no_folders (GList *files)
{
    GList *l;
    gboolean ret = TRUE;

    for (l = files; l != NULL; l = l->next) {
        GFile *location = G_FILE (l->data);
        NemoFile *file = nemo_file_get_existing (location);

        if (file == NULL) {
            ret = FALSE;
            break;
        }

        if (nemo_file_is_directory (file)) {
            ret = FALSE;
            nemo_file_unref (file);
            break;
        }

        nemo_file_unref (file);
    }

#ifdef DEBUG_FILE_OP_QUEUE
    g_message ("File op job has no folders: %s\n", ret ? "TRUE" : "FALSE");
#endif

    return ret;
}

static gboolean
job_is_small (GList *files)
{
    gboolean ret = FALSE;
    GList *l;
    goffset size = 0;

    for (l = files; l != NULL; l = l->next) {
        GFile *location = G_FILE (l->data);
        NemoFile *file = nemo_file_get_existing (location);

        if (file == NULL) {
            size = G_MAXOFFSET;
            break;
        }

        size = size + nemo_file_get_size (file);

        nemo_file_unref (file);
    }

    ret = size < 104857600; /* 100 mb */

#ifdef DEBUG_FILE_OP_QUEUE
    g_message ("File op job is small: %s\n", ret ? "TRUE" : "FALSE");
#endif

    return ret;
}

static gboolean
should_start_immediately (OpKind kind, gpointer op_data)
{
    gboolean ret = FALSE;

    switch (kind) {
        case OP_KIND_CREATE:
        case OP_KIND_TRUST:
        case OP_KIND_EMPTY_TRASH:
        case OP_KIND_PERMISSIONS:
        case OP_KIND_LINK:
            ret = TRUE;
            break;
        case OP_KIND_MOVE:
            ;
            CopyMoveJob *mjob = (CopyMoveJob *) op_data;
            ret = job_is_same_fs (mjob->files, mjob->destination) &&
                  job_is_local (mjob->files, mjob->destination);
            break;
        case OP_KIND_COPY:
            ;
            CopyMoveJob *cjob = (CopyMoveJob *) op_data;
            ret = job_is_same_fs (cjob->files, cjob->destination) &&
                  job_is_local (cjob->files, cjob->destination) &&
                  job_has_no_folders (cjob->files) &&
                  job_is_small (cjob->files);
            break;
        case OP_KIND_DUPE:
            ;
            CopyMoveJob *dupejob = (CopyMoveJob *) op_data;
            ret = job_is_local (dupejob->files, dupejob->destination) &&
                  job_has_no_folders (dupejob->files) &&
                  job_is_small (dupejob->files);
            break;
        case OP_KIND_DELETE:
        case OP_KIND_TRASH:
            ;
            DeleteJob *deljob = (DeleteJob *) op_data;
            ret = job_is_local (deljob->files, NULL) ||
                      (job_has_no_folders (deljob->files) &&
                       job_is_small (deljob->files));
            break;
        default:
            ret = FALSE;
            break;
    }

#ifdef DEBUG_FILE_OP_QUEUE
    g_message ("File op job STARTING IMMEDIATELY: %s\n", ret ? "TRUE" : "FALSE");
#endif

    return ret;
}

void
add_job_to_job_queue (GIOSchedulerJobFunc job_func,
                                 gpointer user_data,
                            GCancellable *cancellable,
                        NemoProgressInfo *info,
                                  OpKind  kind)
{
    gboolean start_immediately;

    NemoJobQueue *job_queue = nemo_job_queue_get ();

    start_immediately = should_start_immediately (kind, user_data);

    nemo_job_queue_add_new_job (job_queue,
                                job_func,
                                user_data,
                                cancellable,
                                info,
                                start_immediately);
}


#if !defined (NEMO_OMIT_SELF_CHECK)

void
nemo_self_check_file_operations (void)
{
	setlocale (LC_MESSAGES, "C");


	/* test the next duplicate name generator */
	EEL_CHECK_STRING_RESULT (get_duplicate_name (" (copy)", 1, -1), " (another copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo", 1, -1), "foo (copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name (".bashrc", 1, -1), ".bashrc (copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name (".foo.txt", 1, -1), ".foo (copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo foo", 1, -1), "foo foo (copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo.txt", 1, -1), "foo (copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo foo.txt", 1, -1), "foo foo (copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo foo.txt txt", 1, -1), "foo foo (copy).txt txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo...txt", 1, -1), "foo.. (copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo...", 1, -1), "foo... (copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo. (copy)", 1, -1), "foo. (another copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (copy)", 1, -1), "foo (another copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (copy).txt", 1, -1), "foo (another copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (another copy)", 1, -1), "foo (3rd copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (another copy).txt", 1, -1), "foo (3rd copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo foo (another copy).txt", 1, -1), "foo foo (3rd copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (13th copy)", 1, -1), "foo (14th copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (13th copy).txt", 1, -1), "foo (14th copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (21st copy)", 1, -1), "foo (22nd copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (21st copy).txt", 1, -1), "foo (22nd copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (22nd copy)", 1, -1), "foo (23rd copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (22nd copy).txt", 1, -1), "foo (23rd copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (23rd copy)", 1, -1), "foo (24th copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (23rd copy).txt", 1, -1), "foo (24th copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (24th copy)", 1, -1), "foo (25th copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (24th copy).txt", 1, -1), "foo (25th copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo foo (24th copy)", 1, -1), "foo foo (25th copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo foo (24th copy).txt", 1, -1), "foo foo (25th copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo foo (100000000000000th copy).txt", 1, -1), "foo foo (copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (10th copy)", 1, -1), "foo (11th copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (10th copy).txt", 1, -1), "foo (11th copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (11th copy)", 1, -1), "foo (12th copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (11th copy).txt", 1, -1), "foo (12th copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (12th copy)", 1, -1), "foo (13th copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (12th copy).txt", 1, -1), "foo (13th copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (110th copy)", 1, -1), "foo (111th copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (110th copy).txt", 1, -1), "foo (111th copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (122nd copy)", 1, -1), "foo (123rd copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (122nd copy).txt", 1, -1), "foo (123rd copy).txt");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (123rd copy)", 1, -1), "foo (124th copy)");
	EEL_CHECK_STRING_RESULT (get_duplicate_name ("foo (123rd copy).txt", 1, -1), "foo (124th copy).txt");

	setlocale (LC_MESSAGES, "");
}

#endif
