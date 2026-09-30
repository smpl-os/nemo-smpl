/* Shared real-window fixture; run only on the isolated regression desktop. */
#ifndef NEMO_WINDOW_TEST_FIXTURE_H
#define NEMO_WINDOW_TEST_FIXTURE_H

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include "../src/nemo-main-application.h"
#include "../src/nemo-window-private.h"
#include "../src/nemo-view.h"
#include <libnemo-private/nemo-global-preferences.h>

static NemoApplication *application;
static guint fixture_number;

void __wrap_nemo_overview_start_lazy_cache (void) {}

static void
iterate (void)
{
    while (g_main_context_iteration (NULL, FALSE));
    g_usleep (1000);
}

#define WAIT_FOR(condition) G_STMT_START { \
    gint64 deadline = g_get_monotonic_time () + 10000000; \
    while (!(condition) && g_get_monotonic_time () < deadline) iterate (); \
    g_assert_true (condition); \
} G_STMT_END

typedef struct {
    NemoWindow *window;
    NemoWindowSlot *slot;
    GFile *origin, *containing, *item;
} Fixture;

static gboolean
slot_at (NemoWindowSlot *slot, GFile *location)
{
    return slot->pending_location == NULL && slot->location != NULL &&
           g_file_equal (slot->location, location) && slot->content_view != NULL &&
           !nemo_view_get_loading (slot->content_view);
}

static Fixture
fixture_new (void)
{
    g_assert_cmpstr (g_getenv ("NEMO_TEST_ISOLATED"), ==, "1");
    g_autofree char *name = g_strdup_printf ("search-%u", fixture_number++);
    g_autofree char *origin = g_build_filename (g_getenv ("NEMO_TEST_PROFILE"), name, NULL);
    g_autofree char *containing = g_build_filename (origin, "nested", NULL);
    g_autofree char *item = g_build_filename (containing, "needle.txt", NULL);
    g_assert_cmpint (g_mkdir_with_parents (containing, 0700), ==, 0);
    g_assert_true (g_file_set_contents (item, "temporary search fixture", -1, NULL));
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_START_WITH_DUAL_PANE, FALSE);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_ALWAYS_USE_BROWSER, TRUE);
    g_settings_set_boolean (nemo_preferences, NEMO_PREFERENCES_RESTORE_TABS_ON_STARTUP, FALSE);
    Fixture fixture = { 0 };
    fixture.origin = g_file_new_for_path (origin);
    fixture.containing = g_file_new_for_path (containing);
    fixture.item = g_file_new_for_path (item);
    fixture.window = nemo_application_create_window (application, gdk_screen_get_default ());
    g_object_ref (fixture.window);
    fixture.slot = nemo_window_get_active_slot (fixture.window);
    nemo_window_slot_open_location (fixture.slot, fixture.origin, NEMO_WINDOW_OPEN_FLAG_SAME_SLOT);
    gtk_widget_show (GTK_WIDGET (fixture.window));
    WAIT_FOR (slot_at (fixture.slot, fixture.origin));
    gtk_window_present (GTK_WINDOW (fixture.window));
    WAIT_FOR (gtk_window_is_active (GTK_WINDOW (fixture.window)));
    return fixture;
}

static void
fixture_clear (Fixture *fixture)
{
    gtk_widget_destroy (GTK_WIDGET (fixture->window));
    g_object_unref (fixture->window);
    g_assert_true (g_file_delete (fixture->item, NULL, NULL));
    g_assert_true (g_file_delete (fixture->containing, NULL, NULL));
    g_assert_true (g_file_delete (fixture->origin, NULL, NULL));
    g_object_unref (fixture->item);
    g_object_unref (fixture->containing);
    g_object_unref (fixture->origin);
    iterate ();
}

static gboolean
key_press (NemoWindow *window, const char *accelerator)
{
    guint key;
    GdkModifierType modifiers;
    gtk_accelerator_parse (accelerator, &key, &modifiers);
    /* X11 synthetic modifiers do not reliably reach GTK accelerators. */
    if (modifiers != 0)
        return gtk_accel_groups_activate (G_OBJECT (window), key, modifiers);
    GdkWindow *native = gtk_widget_get_window (GTK_WIDGET (window));
    g_assert_nonnull (native);
    return gdk_test_simulate_key (native, -1, -1, key, modifiers, GDK_KEY_PRESS) &&
           gdk_test_simulate_key (native, -1, -1, key, modifiers, GDK_KEY_RELEASE);
}

#endif
