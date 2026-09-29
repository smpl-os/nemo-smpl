/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEMO_PACKAGE_VERSION_H
#define NEMO_PACKAGE_VERSION_H

#include <gio/gio.h>

/* Queries the package owning the running executable. Blocking: GUI callers
 * must use a worker. Unowned source builds are explicitly labelled as such. */
char *nemo_get_package_version (GCancellable *cancellable, GError **error);

#endif
