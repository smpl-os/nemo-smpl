/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NEMO_ARCHIVE_DIALOG_H
#define NEMO_ARCHIVE_DIALOG_H

#include <gtk/gtk.h>

void nemo_archive_dialog_show (GtkWindow *parent,
                              GList *sources,
                              GFile *default_directory,
                              gboolean move);

#endif
