/* Registry/settings tests; no NemoView or archive engine is included. */
#include <config.h>
#include <gtk/gtk.h>
#include <string.h>
#include "../src/nemo-keybindings.h"

typedef struct {
    const char *key;
    const char *action;
    const char *path;
    const char *accelerator;
    const char *replacement;
} Shortcut;

static const Shortcut shortcuts[] = {
    { "archive-create", "ArchiveCreate", "<Actions>/DirViewActions/ArchiveCreate",
      "<Alt>F5", "<Control><Alt>F8" },
    { "archive-move", "ArchiveMove", "<Actions>/DirViewActions/ArchiveMove",
      "<Alt><Shift>F5", "<Control><Alt>F9" }
};

static void
assert_accelerator (const Shortcut *shortcut, const char *expected)
{
    guint key;
    GdkModifierType modifiers;
    GtkAccelKey mapped;
    gtk_accelerator_parse (expected, &key, &modifiers);
    g_assert_true (gtk_accel_map_lookup_entry (shortcut->path, &mapped));
    g_assert_cmpuint (mapped.accel_key, ==, key);
    g_assert_cmpuint (mapped.accel_mods, ==, modifiers);
}

static void
test_registry_and_schema (gconstpointer data)
{
    const Shortcut *shortcut = data;
    const NemoKeybindingEntry *entry = NULL;
    for (gint i = 0; i < nemo_keybinding_entries_count; i++) {
        if (g_str_equal (nemo_keybinding_entries[i].settings_key, shortcut->key)) {
            g_assert_null (entry);
            entry = &nemo_keybinding_entries[i];
        }
    }
    g_assert_nonnull (entry);
    g_assert_cmpstr (entry->accel_path, ==, shortcut->path);
    g_assert_cmpstr (entry->default_accel, ==, shortcut->accelerator);
    g_assert_cmpstr (entry->category, ==, "File Operations");
    g_assert_null (entry->binding_set_name);
    g_assert_null (entry->signal_name);
    g_autoptr (GVariant) value = g_settings_get_default_value (nemo_keybinding_settings, shortcut->key);
    g_assert_nonnull (value);
    g_assert_cmpstr (g_variant_get_string (value, NULL), ==, shortcut->accelerator);
}

static void
activated (GtkAction *action, gpointer data)
{
    (*(guint *) data)++;
}

static gboolean
activate (GtkWidget *window, const char *accelerator)
{
    guint key;
    GdkModifierType modifiers;
    gtk_accelerator_parse (accelerator, &key, &modifiers);
    return gtk_accel_groups_activate (G_OBJECT (window), key, modifiers);
}

static void
test_live_binding (gconstpointer data)
{
    const Shortcut *shortcut = data;
    guint calls = 0;
    GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    GtkAccelGroup *group = gtk_accel_group_new ();
    GtkAction *action = gtk_action_new (shortcut->action, "Archive", "Create an archive", NULL);
    gtk_window_add_accel_group (GTK_WINDOW (window), group);
    gtk_action_set_accel_path (action, shortcut->path);
    gtk_action_set_accel_group (action, group);
    guint key;
    GdkModifierType modifiers;
    gtk_accelerator_parse (shortcut->accelerator, &key, &modifiers);
    /* Production GtkActionEntry supplies defaults; this small fixture supplies
     * its own entry to test live settings application without a whole view. */
    gtk_accel_map_add_entry (shortcut->path, key, modifiers);
    gtk_action_connect_accelerator (action);
    g_signal_connect (action, "activate", G_CALLBACK (activated), &calls);
    gtk_widget_show (window);

    assert_accelerator (shortcut, shortcut->accelerator);
    g_assert_true (activate (window, shortcut->accelerator));
    g_assert_cmpuint (calls, ==, 1);
    g_assert_false (activate (window, shortcut == &shortcuts[0] ? shortcuts[1].accelerator : shortcuts[0].accelerator));

    nemo_keybindings_set_for_action (shortcut->key, shortcut->replacement);
    assert_accelerator (shortcut, shortcut->replacement);
    g_assert_false (activate (window, shortcut->accelerator));
    g_assert_true (activate (window, shortcut->replacement));
    g_assert_cmpuint (calls, ==, 2);
    g_autofree char *tooltip = nemo_keybindings_get_action_tooltip (action);
    gtk_accelerator_parse (shortcut->replacement, &key, &modifiers);
    g_autofree char *label = gtk_accelerator_get_label (key, modifiers);
    g_assert_nonnull (strstr (tooltip, label));

    nemo_keybindings_set_for_action (shortcut->key, "");
    assert_accelerator (shortcut, "");
    g_assert_false (activate (window, shortcut->replacement));
    g_clear_pointer (&tooltip, g_free);
    tooltip = nemo_keybindings_get_action_tooltip (action);
    g_assert_cmpstr (tooltip, ==, "Create an archive");
    g_settings_reset (nemo_keybinding_settings, shortcut->key);
    assert_accelerator (shortcut, shortcut->accelerator);
    g_assert_true (activate (window, shortcut->accelerator));
    g_assert_cmpuint (calls, ==, 3);

    gtk_action_disconnect_accelerator (action);
    gtk_widget_destroy (window);
    g_object_unref (action);
    g_object_unref (group);
}

int
main (int argc, char **argv)
{
    if (g_strcmp0 (g_getenv ("NEMO_TEST_ISOLATED"), "1") != 0) {
        g_printerr ("Run archive-shortcuts tests through run-isolated-regression.py.\n");
        return 77;
    }
    g_assert_cmpstr (g_getenv ("GSETTINGS_BACKEND"), ==, "memory");
    g_assert_cmpstr (g_getenv ("GDK_BACKEND"), ==, "x11");
    g_assert_null (g_getenv ("WAYLAND_DISPLAY"));
    gtk_test_init (&argc, &argv, NULL);
    nemo_keybindings_init ();
    g_test_add_data_func ("/archive-shortcuts/copy/registry", &shortcuts[0], test_registry_and_schema);
    g_test_add_data_func ("/archive-shortcuts/move/registry", &shortcuts[1], test_registry_and_schema);
    g_test_add_data_func ("/archive-shortcuts/copy/live", &shortcuts[0], test_live_binding);
    g_test_add_data_func ("/archive-shortcuts/move/live", &shortcuts[1], test_live_binding);
    return g_test_run ();
}
