/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <config.h>
#include "nemo-type-jump.h"
#include "nemo-global-preferences.h"

#include <string.h>

char *
nemo_type_jump_normalize (const char *text)
{
	if (text == NULL)
		return NULL;
	g_return_val_if_fail (g_utf8_validate (text, -1, NULL), NULL);
	g_autofree char *normalized = g_utf8_normalize (text, -1, G_NORMALIZE_ALL);
	return g_utf8_casefold (normalized, -1);
}

NemoTypeJumpMatch
nemo_type_jump_match (const char *normalized_query, const char *filename, gboolean prefix_only)
{
	if (normalized_query == NULL || *normalized_query == '\0' || filename == NULL)
		return NEMO_TYPE_JUMP_NO_MATCH;
	g_autofree char *name = nemo_type_jump_normalize (filename);
	if (name == NULL)
		return NEMO_TYPE_JUMP_NO_MATCH;
	const char *match = strstr (name, normalized_query);
	if (match == name)
		return NEMO_TYPE_JUMP_PREFIX;
	if (match != NULL && !prefix_only)
		return NEMO_TYPE_JUMP_SUBSTRING;
	return NEMO_TYPE_JUMP_NO_MATCH;
}

PangoAttrList *
nemo_type_jump_match_attrs (const char *query, const char *filename, gboolean prefix_only)
{
	g_autofree char *needle = nemo_type_jump_normalize (query);
	if (needle == NULL || *needle == '\0' || filename == NULL)
		return NULL;
	g_return_val_if_fail (g_utf8_validate (filename, -1, NULL), NULL);

	GString *folded = g_string_new (NULL);
	GArray *starts = g_array_new (FALSE, FALSE, sizeof (guint));
	GArray *ends = g_array_new (FALSE, FALSE, sizeof (guint));
	for (const char *p = filename; *p != '\0';) {
		const char *next = g_utf8_next_char (p);
		/* Normalize a whole base/mark cluster so reordered combining marks
		 * and casefold expansions still map back to the original glyph. */
		while (*next != '\0' && g_unichar_ismark (g_utf8_get_char (next)))
			next = g_utf8_next_char (next);
		g_autofree char *cluster = g_strndup (p, next - p);
		g_autofree char *normalized = nemo_type_jump_normalize (cluster);
		guint start = p - filename, end = next - filename;
		for (gsize i = 0; normalized[i] != '\0'; i++) {
			g_array_append_val (starts, start);
			g_array_append_val (ends, end);
		}
		g_string_append (folded, normalized);
		p = next;
	}

	PangoAttrList *attrs = NULL;
	gsize length = strlen (needle);
	const char *match = strstr (folded->str, needle);
	if (match != NULL && (!prefix_only || match == folded->str)) {
		attrs = pango_attr_list_new ();
		while (match != NULL) {
			gsize offset = match - folded->str;
			PangoAttribute *weight = pango_attr_weight_new (PANGO_WEIGHT_BOLD);
			weight->start_index = g_array_index (starts, guint, offset);
			weight->end_index = g_array_index (ends, guint, offset + length - 1);
			pango_attr_list_change (attrs, weight);
			if (prefix_only)
				break;
			match = strstr (match + length, needle);
		}
	}
	g_string_free (folded, TRUE);
	g_array_unref (starts);
	g_array_unref (ends);
	return attrs;
}

static guint
navigation_keyval (guint key)
{
	switch (key) {
	case GDK_KEY_KP_Up: return GDK_KEY_Up;
	case GDK_KEY_KP_Down: return GDK_KEY_Down;
	case GDK_KEY_KP_Left: return GDK_KEY_Left;
	case GDK_KEY_KP_Right: return GDK_KEY_Right;
	default: return gdk_keyval_to_lower (key);
	}
}

int
nemo_type_jump_key_direction (const GdkEventKey *event)
{
	static const struct { const char *key; int direction; } bindings[] = {
		{ "type-jump-previous", -1 },
		{ "type-jump-next", 1 }
	};
	if (event == NULL || nemo_keybinding_settings == NULL)
		return 0;
	for (guint i = 0; i < G_N_ELEMENTS (bindings); i++) {
		g_autofree char *value = g_settings_get_string (nemo_keybinding_settings, bindings[i].key);
		guint key = 0;
		GdkModifierType modifiers = 0;
		gtk_accelerator_parse (value, &key, &modifiers);
		if (key != 0 && navigation_keyval (event->keyval) == navigation_keyval (key) &&
		    (event->state & gtk_accelerator_get_default_mod_mask ()) == modifiers)
			return bindings[i].direction;
	}
	return 0;
}
