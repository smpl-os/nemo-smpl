/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEMO_TYPE_JUMP_H
#define NEMO_TYPE_JUMP_H

#include <gtk/gtk.h>

typedef enum {
	NEMO_TYPE_JUMP_NO_MATCH = -1,
	NEMO_TYPE_JUMP_PREFIX = 0,
	NEMO_TYPE_JUMP_SUBSTRING = 1
} NemoTypeJumpMatch;

char *nemo_type_jump_normalize (const char *text);
NemoTypeJumpMatch nemo_type_jump_match (const char *normalized_query,
				       const char *filename,
				       gboolean prefix_only);
/* Bold ranges use byte offsets in the original, unmodified UTF-8 filename. */
PangoAttrList *nemo_type_jump_match_attrs (const char *query,
					  const char *filename,
					  gboolean prefix_only);
/* -1 = previous match, 1 = next match, 0 = not a configured match-navigation key. */
int nemo_type_jump_key_direction (const GdkEventKey *event);

#endif
