/* nemo-icon-fallback.c
 *
 * Implementation of the xapp icon-name fallbacks. See nemo-icon-fallback.h.
 *
 * Copyright (C) 2026 smplOS
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
 */

#include <config.h>

#include "nemo-icon-fallback.h"

#ifdef NEMO_SMPL

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

/* Nemo, following upstream Cinnamon, asks for most of its UI icons by their
 * xapp names: "xsi-*" for symbolic action icons plus a handful of "xapp-*"
 * ones. Those names only resolve when the xapp icon packages are installed
 * *and* the active icon theme reaches hicolor through its inheritance chain.
 *
 * On a system where either is untrue -- a non-Mint distro, a theme that does
 * not inherit hicolor, or (very commonly) an icon-theme setting naming a
 * theme that is not installed at all -- every single one of those lookups
 * fails and GTK draws a blank placeholder. Toolbars, menus, the sidebar and
 * dialog buttons all turn into unlabelled boxes.
 *
 * Instead of rewriting ~130 call sites (many of them static GtkActionEntry
 * tables, which cannot be intercepted), we maintain a small directory of
 * symlinks pointing at the closest standard freedesktop icon and put it on
 * the icon theme search path. GTK treats loose image files in the root of a
 * search path as "unthemed" icons and consults them only when no theme in
 * the chain supplies the name -- so a system that genuinely has the xapp
 * icons keeps using them and is completely unaffected.
 */

#define FALLBACK_LOOKUP_SIZE 16

/* Slots in a candidate list, terminator included. Both the hand-written
 * table below and derive_candidates() produce arrays of exactly this size,
 * so resolve_fallback_file() can bound its walk and stay in range even if a
 * future edit forgets the terminator. */
#define MAX_FALLBACK_CANDIDATES 5

typedef struct {
	const char *alias;
	const char *targets[MAX_FALLBACK_CANDIDATES];   /* NULL-terminated, best match first */
} IconAlias;

/* Aliases that do not reduce to a standard icon just by dropping the
 * "xsi-"/"xapp-" prefix. Everything not listed here is handled mechanically
 * by derive_candidates(). */
static const IconAlias extra_aliases[] = {
	{ "xapp-favorite",                   { "starred", "starred-symbolic", "bookmark-new-symbolic", NULL } },
	{ "xapp-favorite-available",         { "non-starred", "non-starred-symbolic", "bookmark-new-symbolic", NULL } },
	{ "xapp-user-favorites",             { "user-bookmarks", "user-bookmarks-symbolic", "starred-symbolic", NULL } },
	{ "xsi-favorite-symbolic",           { "starred-symbolic", "bookmark-new-symbolic", NULL } },
	{ "xsi-unfavorite-symbolic",         { "non-starred-symbolic", "bookmark-missing-symbolic", "list-remove-symbolic", NULL } },
	{ "xsi-user-favorites-symbolic",     { "user-bookmarks-symbolic", "starred-symbolic", NULL } },
	{ "xsi-folder-recent-symbolic",      { "document-open-recent-symbolic", "folder-open-symbolic", NULL } },
	{ "xsi-folder-warning-symbolic",     { "dialog-warning-symbolic", "folder-visiting-symbolic", NULL } },
	{ "xsi-keyboard-shortcuts-symbolic", { "preferences-desktop-keyboard-shortcuts-symbolic", "input-keyboard-symbolic", "preferences-desktop-keyboard-symbolic", NULL } },
	{ "xsi-media-mount-symbolic",        { "drive-removable-media-symbolic", "media-removable-symbolic", NULL } },
	{ "xsi-pin-symbolic",                { "view-pin-symbolic", "bookmark-new-symbolic", "starred-symbolic", NULL } },
	{ "xsi-unpin-symbolic",              { "view-unpin-symbolic", "bookmark-missing-symbolic", "non-starred-symbolic", "list-remove-symbolic", NULL } },
	{ "xsi-preview-symbolic",            { "view-reveal-symbolic", "document-print-preview-symbolic", "image-x-generic-symbolic", NULL } },
	{ "xsi-search-symbolic",             { "system-search-symbolic", "edit-find-symbolic", NULL } },
	{ "xsi-toolbox-symbolic",            { "open-menu-symbolic", "applications-utilities-symbolic", NULL } },
	{ "xsi-tools-symbolic",              { "applications-utilities-symbolic", "preferences-other-symbolic", "emblem-system-symbolic", NULL } },
	{ "xsi-view-compact-symbolic",       { "view-list-compact-symbolic", "view-continuous-symbolic", "view-list-symbolic", NULL } },
};

/* Every xapp icon name Nemo asks for. Regenerate with:
 *
 *   grep -rhoE '"(xsi|xapp)-[a-z0-9-]+"' --include='*.c' --include='*.h' \
 *        src libnemo-private | tr -d '"' | sort -u
 */
static const char * const aliased_icon_names[] = {
	"xapp-favorite",
	"xapp-favorite-available",
	"xapp-user-favorites",
	"xsi-bookmark-new-symbolic",
	"xsi-computer-symbolic",
	"xsi-dialog-error-symbolic",
	"xsi-dialog-password-symbolic",
	"xsi-dialog-warning-symbolic",
	"xsi-document-new-symbolic",
	"xsi-document-open-symbolic",
	"xsi-document-properties-symbolic",
	"xsi-drive-harddisk-symbolic",
	"xsi-drive-removable-media-symbolic",
	"xsi-edit-clear-symbolic",
	"xsi-edit-copy-symbolic",
	"xsi-edit-cut-symbolic",
	"xsi-edit-delete-symbolic",
	"xsi-edit-find-symbolic",
	"xsi-edit-paste-symbolic",
	"xsi-edit-redo-symbolic",
	"xsi-edit-undo-symbolic",
	"xsi-emblem-important-symbolic",
	"xsi-favorite-symbolic",
	"xsi-folder-documents-symbolic",
	"xsi-folder-download-symbolic",
	"xsi-folder-music-symbolic",
	"xsi-folder-new-symbolic",
	"xsi-folder-open-symbolic",
	"xsi-folder-pictures-symbolic",
	"xsi-folder-publicshare-symbolic",
	"xsi-folder-recent-symbolic",
	"xsi-folder-remote-symbolic",
	"xsi-folder-saved-search-symbolic",
	"xsi-folder-symbolic",
	"xsi-folder-templates-symbolic",
	"xsi-folder-videos-symbolic",
	"xsi-folder-warning-symbolic",
	"xsi-go-down-symbolic",
	"xsi-go-home-symbolic",
	"xsi-go-jump-symbolic",
	"xsi-go-next-symbolic",
	"xsi-go-previous-symbolic",
	"xsi-go-up-symbolic",
	"xsi-help-about-symbolic",
	"xsi-help-contents-symbolic",
	"xsi-keyboard-shortcuts-symbolic",
	"xsi-list-remove-symbolic",
	"xsi-media-eject-symbolic",
	"xsi-media-mount-symbolic",
	"xsi-media-playback-pause-symbolic",
	"xsi-media-playback-start-symbolic",
	"xsi-media-playback-stop-symbolic",
	"xsi-network-server-symbolic",
	"xsi-network-workgroup-symbolic",
	"xsi-pan-end-symbolic",
	"xsi-pan-start-symbolic",
	"xsi-pin-symbolic",
	"xsi-preview-symbolic",
	"xsi-process-stop-symbolic",
	"xsi-search-symbolic",
	"xsi-tab-new-symbolic",
	"xsi-toolbox-symbolic",
	"xsi-tools-symbolic",
	"xsi-unfavorite-symbolic",
	"xsi-unpin-symbolic",
	"xsi-user-desktop-symbolic",
	"xsi-user-favorites-symbolic",
	"xsi-user-home-symbolic",
	"xsi-user-trash-full-symbolic",
	"xsi-user-trash-symbolic",
	"xsi-utilities-terminal-symbolic",
	"xsi-view-compact-symbolic",
	"xsi-view-dual-symbolic",
	"xsi-view-grid-symbolic",
	"xsi-view-list-symbolic",
	"xsi-view-refresh-symbolic",
	"xsi-view-sort-ascending-symbolic",
	"xsi-window-close-symbolic",
	"xsi-zoom-in-symbolic",
	"xsi-zoom-original-symbolic",
	"xsi-zoom-out-symbolic",
};

static char     *fallback_dir = NULL;
static gboolean  rebuild_running = FALSE;

static const char *
strip_xapp_prefix (const char *name)
{
	if (g_str_has_prefix (name, "xsi-")) {
		return name + strlen ("xsi-");
	}
	if (g_str_has_prefix (name, "xapp-")) {
		return name + strlen ("xapp-");
	}
	return name;
}

/* Fill `out` (MAX_FALLBACK_CANDIDATES slots, NULL-terminated) with standard
 * names to try for `alias`, best first. */
static void
derive_candidates (const char *alias, char **out)
{
	const char *base = strip_xapp_prefix (alias);
	int n = 0;

	out[n++] = g_strdup (base);

	if (g_str_has_suffix (base, "-symbolic")) {
		out[n++] = g_strndup (base, strlen (base) - strlen ("-symbolic"));
	} else {
		out[n++] = g_strconcat (base, "-symbolic", NULL);
	}

	out[n] = NULL;
}

/* TRUE when a real icon theme supplies `name`, i.e. the lookup resolved to
 * something that is not one of the symlinks we manage ourselves. */
static gboolean
theme_provides_icon (GtkIconTheme *theme, const char *name)
{
	GtkIconInfo *info;
	const char *filename;
	gboolean provided;

	info = gtk_icon_theme_lookup_icon (theme, name, FALLBACK_LOOKUP_SIZE, 0);
	if (info == NULL) {
		return FALSE;
	}

	filename = gtk_icon_info_get_filename (info);
	/* A NULL filename means a builtin or GResource icon, which is a real
	 * hit we must not shadow. */
	provided = (filename == NULL || !g_str_has_prefix (filename, fallback_dir));

	g_object_unref (info);
	return provided;
}

/* Resolve the on-disk file of the best standard replacement for `alias`,
 * or NULL when the theme has nothing usable. */
static char *
resolve_fallback_file (GtkIconTheme *theme, const char *alias)
{
	const char * const *candidates = NULL;
	char *derived[MAX_FALLBACK_CANDIDATES] = { NULL };
	char *result = NULL;
	guint i;
	int c, f;

	for (i = 0; i < G_N_ELEMENTS (extra_aliases); i++) {
		if (g_strcmp0 (extra_aliases[i].alias, alias) == 0) {
			candidates = extra_aliases[i].targets;
			break;
		}
	}

	if (candidates == NULL) {
		derive_candidates (alias, derived);
		candidates = (const char * const *) derived;
	}

	/* Prefer a scalable source across *all* candidates before settling for
	 * a fixed-size one, so an alias keeps working at every icon size. */
	for (f = 0; f < 2 && result == NULL; f++) {
		GtkIconLookupFlags flags = (f == 0) ? GTK_ICON_LOOKUP_FORCE_SVG : 0;

		for (c = 0;
		     c < MAX_FALLBACK_CANDIDATES && candidates[c] != NULL && result == NULL;
		     c++) {
			GtkIconInfo *info;
			const char *filename;

			info = gtk_icon_theme_lookup_icon (theme, candidates[c],
			                                   FALLBACK_LOOKUP_SIZE, flags);
			if (info == NULL) {
				continue;
			}

			filename = gtk_icon_info_get_filename (info);
			if (filename != NULL && !g_str_has_prefix (filename, fallback_dir)) {
				result = g_strdup (filename);
			}

			g_object_unref (info);
		}
	}

	for (c = 0; c < MAX_FALLBACK_CANDIDATES; c++) {
		g_free (derived[c]);
	}

	return result;
}

static void
rebuild_fallbacks (GtkIconTheme *theme)
{
	GHashTable *wanted;
	GHashTableIter iter;
	gpointer key, value;
	GDir *dir;
	const char *entry;
	gboolean changed = FALSE;
	guint i;

	/* Work out the set of symlinks we should have right now. */
	wanted = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	for (i = 0; i < G_N_ELEMENTS (aliased_icon_names); i++) {
		const char *alias = aliased_icon_names[i];
		char *target, *link_name;
		const char *ext;

		if (theme_provides_icon (theme, alias)) {
			continue;
		}

		target = resolve_fallback_file (theme, alias);
		if (target == NULL) {
			continue;
		}

		ext = strrchr (target, '.');
		link_name = g_strconcat (alias, ext ? ext : ".svg", NULL);
		g_hash_table_insert (wanted, link_name, target);
	}

	/* Drop anything present that is stale or now points at the wrong file.
	 * We own this directory outright, so anything unexpected can go. */
	dir = g_dir_open (fallback_dir, 0, NULL);
	if (dir != NULL) {
		while ((entry = g_dir_read_name (dir)) != NULL) {
			const char *want = g_hash_table_lookup (wanted, entry);
			char *path = g_build_filename (fallback_dir, entry, NULL);
			char *current = g_file_read_link (path, NULL);

			if (want != NULL && g_strcmp0 (current, want) == 0) {
				/* Already correct, nothing to write. */
				g_hash_table_remove (wanted, entry);
			} else {
				g_unlink (path);
				changed = TRUE;
			}

			g_free (current);
			g_free (path);
		}
		g_dir_close (dir);
	}

	/* Create whatever is left. */
	g_hash_table_iter_init (&iter, wanted);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		char *path = g_build_filename (fallback_dir, (const char *) key, NULL);

		if (symlink ((const char *) value, path) == 0) {
			changed = TRUE;
		} else {
			g_warning ("icon-fallback: could not link %s -> %s: %s",
			           path, (const char *) value, g_strerror (errno));
		}

		g_free (path);
	}

	g_hash_table_destroy (wanted);

	if (changed) {
		/* gtk_icon_theme_rescan_if_needed() compares stored directory
		 * mtimes at one-second resolution, so it reliably misses links
		 * we just wrote. Re-setting the search path forces an
		 * unconditional reload instead, which is what actually pulls
		 * the new files into the theme's "unthemed" icon table.
		 *
		 * This re-enters us through "changed"; rebuild_running makes
		 * that a no-op. */
		gchar **path = NULL;
		gint n_paths = 0;

		gtk_icon_theme_get_search_path (theme, &path, &n_paths);
		gtk_icon_theme_set_search_path (theme, (const gchar **) path, n_paths);
		g_strfreev (path);
	}
}

static void
on_icon_theme_changed (GtkIconTheme *theme,
                       gpointer      user_data)
{
	if (rebuild_running) {
		return;
	}

	rebuild_running = TRUE;
	rebuild_fallbacks (theme);
	rebuild_running = FALSE;
}

void
nemo_icon_fallback_init (void)
{
	static gboolean initialized = FALSE;
	GtkIconTheme *theme;

	if (initialized) {
		return;
	}
	initialized = TRUE;

	fallback_dir = g_build_filename (g_get_user_cache_dir (), "nemo",
	                                 "icon-fallbacks", NULL);

	if (g_mkdir_with_parents (fallback_dir, 0700) != 0) {
		g_warning ("icon-fallback: cannot create %s: %s -- xapp icon names "
		           "will not have standard fallbacks",
		           fallback_dir, g_strerror (errno));
		g_clear_pointer (&fallback_dir, g_free);
		return;
	}

	theme = gtk_icon_theme_get_default ();

	g_signal_connect (theme, "changed",
	                  G_CALLBACK (on_icon_theme_changed), NULL);

	rebuild_running = TRUE;
	/* Deliberately populated before the directory is on the search path:
	 * while it is still invisible to GTK, every lookup inside
	 * rebuild_fallbacks() is guaranteed to be a genuine theme hit, so a
	 * leftover symlink from a previous run cannot be mistaken for one.
	 *
	 * append_search_path() then forces an unconditional reload, which is
	 * what actually makes the new links visible -- notably including the
	 * first run, where the links are younger than the one-second
	 * resolution of the mtimes GTK compares. */
	rebuild_fallbacks (theme);
	gtk_icon_theme_append_search_path (theme, fallback_dir);
	rebuild_running = FALSE;
}

#else /* !NEMO_SMPL */

void
nemo_icon_fallback_init (void)
{
}

#endif /* NEMO_SMPL */
