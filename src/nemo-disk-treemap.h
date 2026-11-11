#ifndef NEMO_DISK_TREEMAP_H
#define NEMO_DISK_TREEMAP_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NEMO_TYPE_DISK_TREEMAP (nemo_disk_treemap_get_type ())
G_DECLARE_FINAL_TYPE (NemoDiskTreemap, nemo_disk_treemap, NEMO, DISK_TREEMAP, GtkBox)

GtkWidget *nemo_disk_treemap_new (void);
void nemo_disk_treemap_scan (NemoDiskTreemap *self, const char *path);
void nemo_disk_treemap_cancel (NemoDiskTreemap *self);
const char *nemo_disk_treemap_get_path (NemoDiskTreemap *self);
gboolean nemo_disk_treemap_is_loading (NemoDiskTreemap *self);
gboolean nemo_disk_treemap_has_result (NemoDiskTreemap *self);

G_END_DECLS

#endif
