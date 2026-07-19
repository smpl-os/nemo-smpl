/* nemo-smpl-prefs.c
 *
 * Implementation of smplOS pref helpers. See nemo-smpl-prefs.h.
 */

#include <config.h>

#include "nemo-smpl-prefs.h"

NemoInteractiveSearchMode
nemo_smpl_interactive_search_mode (void)
{
	return (NemoInteractiveSearchMode)
		g_settings_get_enum (nemo_preferences,
				     NEMO_PREFERENCES_INTERACTIVE_SEARCH_MODE);
}
