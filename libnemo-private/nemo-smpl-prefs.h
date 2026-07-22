/* nemo-smpl-prefs.h
 *
 * Thin helpers around smplOS-specific GSettings keys.
 *
 * Rationale: keeping the pref-lookup boilerplate in one place makes each
 * NEMO_SMPL call site a single line, which minimises the diff footprint
 * against upstream and makes future prefs cheap to add. All new smplOS
 * runtime prefs should get a helper here rather than being inlined at
 * the call site.
 */

#ifndef NEMO_SMPL_PREFS_H
#define NEMO_SMPL_PREFS_H

#include <config.h>

#ifdef NEMO_SMPL

#include <glib.h>

#include "nemo-global-preferences.h"

G_BEGIN_DECLS

/* Interactive type-ahead behavior inside a folder view.
 * Backed by org.nemo.preferences interactive-search-mode. */
NemoInteractiveSearchMode nemo_smpl_interactive_search_mode (void);

/* Whether to protect cross-filesystem copies (removable / slow media) with
 * staging (visible '<name>.nemo-partial-XXXXXXXX') + blocking fsync before
 * signalling completion. Backed by org.nemo.preferences safe-cross-fs-copy. */
gboolean nemo_smpl_safe_cross_fs_copy (void);

G_END_DECLS

#endif /* NEMO_SMPL */

#endif /* NEMO_SMPL_PREFS_H */
