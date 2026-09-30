/*
 * Persistent reading positions for document previews.
 * Copyright (C) 2026 smplOS contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <config.h>

#include "nemo-document-position.h"

#include "nemo-file-utilities.h"
#include "nemo-global-preferences.h"

#include <glib/gstdio.h>

/* Bounded so a long reading history can never grow the keyfile without limit. */
#define POSITION_MAX_ENTRIES 512
#define POSITION_KEEP_ENTRIES 384
#define POSITION_MAX_PAGES 10000
#define POSITION_SAVE_DELAY 1500

#define POSITION_KEY_PAGE "page"
#define POSITION_KEY_OFFSET "offset"
#define POSITION_KEY_PAGES "pages"
#define POSITION_KEY_SIZE "size"
#define POSITION_KEY_MTIME "mtime"
#define POSITION_KEY_STAMP "stamp"

static GKeyFile *position_keyfile = NULL;
static guint position_save_source = 0;

static gboolean
position_settings_lookup (GSettings **settings)
{
	static GSettings *fallback = NULL;

	if (nemo_preferences != NULL) {
		*settings = nemo_preferences;
		return TRUE;
	}
	if (fallback == NULL) {
		GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
		g_autoptr (GSettingsSchema) schema = NULL;

		if (source != NULL)
			schema = g_settings_schema_source_lookup (source, "org.nemo.preferences", TRUE);
		if (schema == NULL)
			return FALSE;
		fallback = g_settings_new ("org.nemo.preferences");
	}
	*settings = fallback;
	return TRUE;
}

gboolean
nemo_document_position_is_enabled (void)
{
	GSettings *settings = NULL;

	/* Without a readable schema the feature stays at its default-on value. */
	if (!position_settings_lookup (&settings))
		return TRUE;
	return g_settings_get_boolean (settings, NEMO_PREFERENCES_REMEMBER_DOCUMENT_POSITION);
}

static char *
position_file_path (void)
{
	g_autofree char *directory = nemo_get_user_directory ();
	return g_build_filename (directory, "document-positions", NULL);
}

static char *
position_group (GFile *file)
{
	g_autofree char *uri = g_file_get_uri (file);

	if (uri == NULL || *uri == '\0')
		return NULL;
	/* Brackets and control characters would break keyfile group syntax. */
	return g_uri_escape_string (uri, "/:@+$,-_.!~*'()", FALSE);
}

static GKeyFile *
position_get_keyfile (void)
{
	if (position_keyfile == NULL) {
		g_autofree char *path = position_file_path ();
		g_autoptr (GError) error = NULL;

		position_keyfile = g_key_file_new ();
		if (!g_key_file_load_from_file (position_keyfile, path, G_KEY_FILE_NONE, &error) &&
		    !g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
			g_debug ("Unable to read saved reading positions: %s", error->message);
	}
	return position_keyfile;
}

static gboolean
position_save_cb (gpointer unused)
{
	g_autofree char *path = position_file_path ();
	g_autofree char *contents = NULL;
	g_autoptr (GError) error = NULL;
	gsize length = 0;

	position_save_source = 0;
	if (position_keyfile == NULL)
		return G_SOURCE_REMOVE;
	contents = g_key_file_to_data (position_keyfile, &length, NULL);
	if (contents == NULL)
		return G_SOURCE_REMOVE;
	if (!g_file_set_contents (path, contents, length, &error))
		g_warning ("Unable to save reading positions: %s", error->message);
	return G_SOURCE_REMOVE;
}

static void
position_queue_save (void)
{
	if (position_save_source != 0)
		g_source_remove (position_save_source);
	position_save_source = g_timeout_add (POSITION_SAVE_DELAY, position_save_cb, NULL);
}

void
nemo_document_position_flush (void)
{
	if (position_save_source == 0)
		return;
	g_source_remove (position_save_source);
	position_save_source = 0;
	position_save_cb (NULL);
}

static gboolean
position_file_identity (GFile *file, gint64 *size, gint64 *mtime)
{
	g_autoptr (GFileInfo) info = NULL;

	*size = *mtime = -1;
	/* Only local files are stat-ed: a preview must never block on the network. */
	if (!g_file_is_native (file))
		return FALSE;
	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_STANDARD_SIZE "," G_FILE_ATTRIBUTE_TIME_MODIFIED,
				  G_FILE_QUERY_INFO_NONE, NULL, NULL);
	if (info == NULL)
		return FALSE;
	*size = g_file_info_get_size (info);
	*mtime = g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
	return TRUE;
}

static gint
position_compare_stamp (gconstpointer a, gconstpointer b, gpointer data)
{
	GKeyFile *keyfile = data;
	gint64 first = g_key_file_get_int64 (keyfile, *(const char * const *) a, POSITION_KEY_STAMP, NULL);
	gint64 second = g_key_file_get_int64 (keyfile, *(const char * const *) b, POSITION_KEY_STAMP, NULL);

	return first < second ? -1 : (first > second ? 1 : 0);
}

static void
position_prune (GKeyFile *keyfile)
{
	gsize length = 0;
	g_auto (GStrv) groups = g_key_file_get_groups (keyfile, &length);

	if (groups == NULL || length <= POSITION_MAX_ENTRIES)
		return;
	g_qsort_with_data (groups, length, sizeof (char *), position_compare_stamp, keyfile);
	for (gsize i = 0; i + POSITION_KEEP_ENTRIES < length; i++)
		g_key_file_remove_group (keyfile, groups[i], NULL);
}

void
nemo_document_position_forget (GFile *file)
{
	g_return_if_fail (G_IS_FILE (file));

	g_autofree char *group = position_group (file);
	GKeyFile *keyfile = position_get_keyfile ();

	if (group == NULL || !g_key_file_has_group (keyfile, group))
		return;
	g_key_file_remove_group (keyfile, group, NULL);
	position_queue_save ();
}

gboolean
nemo_document_position_lookup (GFile *file, guint *page, gdouble *offset)
{
	g_return_val_if_fail (G_IS_FILE (file), FALSE);
	g_return_val_if_fail (page != NULL && offset != NULL, FALSE);

	*page = 0;
	*offset = 0;
	if (!nemo_document_position_is_enabled ())
		return FALSE;

	g_autofree char *group = position_group (file);
	GKeyFile *keyfile = position_get_keyfile ();
	g_autoptr (GError) error = NULL;
	gint64 stored_page, stored_size, stored_mtime, size, mtime;
	gdouble stored_offset;

	if (group == NULL || !g_key_file_has_group (keyfile, group))
		return FALSE;
	stored_page = g_key_file_get_int64 (keyfile, group, POSITION_KEY_PAGE, &error);
	if (error != NULL || stored_page < 0 || stored_page >= POSITION_MAX_PAGES)
		return FALSE;
	g_autoptr (GError) identity_error = NULL;
	stored_size = g_key_file_get_int64 (keyfile, group, POSITION_KEY_SIZE, &identity_error);
	stored_mtime = g_key_file_get_int64 (keyfile, group, POSITION_KEY_MTIME, NULL);
	if (identity_error == NULL && position_file_identity (file, &size, &mtime) &&
	    (stored_size != size || stored_mtime != mtime)) {
		/* The document changed on disk, so the old page is meaningless. */
		g_key_file_remove_group (keyfile, group, NULL);
		position_queue_save ();
		return FALSE;
	}
	stored_offset = g_key_file_get_double (keyfile, group, POSITION_KEY_OFFSET, NULL);
	*page = (guint) stored_page;
	*offset = CLAMP (stored_offset, 0.0, 1.0);
	return *page > 0 || *offset > 0;
}

void
nemo_document_position_store (GFile *file, guint page, gdouble offset, guint page_count)
{
	g_return_if_fail (G_IS_FILE (file));

	if (!nemo_document_position_is_enabled ())
		return;
	if (page_count == 0 || page >= page_count || page_count > POSITION_MAX_PAGES)
		return;

	g_autofree char *group = position_group (file);
	GKeyFile *keyfile = position_get_keyfile ();
	gint64 size, mtime;

	if (group == NULL)
		return;
	offset = CLAMP (offset, 0.0, 1.0);
	/* The start of a document is the default, so remember nothing for it. */
	if (page == 0 && offset <= 0.0) {
		nemo_document_position_forget (file);
		return;
	}
	g_key_file_set_int64 (keyfile, group, POSITION_KEY_PAGE, page);
	g_key_file_set_double (keyfile, group, POSITION_KEY_OFFSET, offset);
	g_key_file_set_int64 (keyfile, group, POSITION_KEY_PAGES, page_count);
	g_key_file_set_int64 (keyfile, group, POSITION_KEY_STAMP, g_get_real_time () / G_USEC_PER_SEC);
	if (position_file_identity (file, &size, &mtime)) {
		g_key_file_set_int64 (keyfile, group, POSITION_KEY_SIZE, size);
		g_key_file_set_int64 (keyfile, group, POSITION_KEY_MTIME, mtime);
	} else {
		g_key_file_remove_key (keyfile, group, POSITION_KEY_SIZE, NULL);
		g_key_file_remove_key (keyfile, group, POSITION_KEY_MTIME, NULL);
	}
	position_prune (keyfile);
	position_queue_save ();
}
