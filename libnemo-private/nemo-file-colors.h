#ifndef NEMO_FILE_COLORS_H
#define NEMO_FILE_COLORS_H

#include "nemo-file.h"
#include <gtk/gtk.h>

typedef enum {
    NEMO_FILE_COLOR_DEFAULT,
    NEMO_FILE_COLOR_FOLDER,
    NEMO_FILE_COLOR_IMAGE,
    NEMO_FILE_COLOR_VIDEO,
    NEMO_FILE_COLOR_AUDIO,
    NEMO_FILE_COLOR_ARCHIVE,
    NEMO_FILE_COLOR_DOCUMENT,
    NEMO_FILE_COLOR_SOURCE,
    NEMO_FILE_COLOR_EXECUTABLE,
    NEMO_FILE_COLOR_COUNT
} NemoFileColorKind;

NemoFileColorKind nemo_file_color_kind_for_type (const char *name, const char *mime_type,
                                                gboolean directory);
NemoFileColorKind nemo_file_get_color_kind (NemoFile *file);
gboolean nemo_file_color_for_kind (NemoFileColorKind kind, GtkStyleContext *context,
                                   GtkStateFlags state, GdkRGBA *color);

#endif
