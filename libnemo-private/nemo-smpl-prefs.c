/* nemo-smpl-prefs.c
 *
 * Implementation of smplOS pref helpers. See nemo-smpl-prefs.h.
 */

#include <config.h>

#include "nemo-smpl-prefs.h"

#ifdef NEMO_SMPL

NemoInteractiveSearchMode
nemo_smpl_interactive_search_mode (void)
{
	return (NemoInteractiveSearchMode)
		g_settings_get_enum (nemo_preferences,
				     NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE);
}

gboolean
nemo_smpl_safe_cross_fs_copy (void)
{
	return g_settings_get_boolean (nemo_preferences,
				       NEMO_PREFERENCES_SAFE_CROSS_FS_COPY);
}

#endif /* NEMO_SMPL */
