#include <config.h>
#include <gtk/gtk.h>
#include <libnemo-private/nemo-type-jump.h>
#include <libnemo-private/nemo-global-preferences.h>

static NemoTypeJumpMatch
match (const char *query, const char *name, gboolean prefix)
{
    g_autofree char *normalized = nemo_type_jump_normalize (query);
    return nemo_type_jump_match (normalized, name, prefix);
}

static void
test_contiguous_ranking (void)
{
    g_assert_cmpint (match ("mi", "misscurve.txt", FALSE), ==, NEMO_TYPE_JUMP_PREFIX);
    g_assert_cmpint (match ("cu", "misscurve.txt", FALSE), ==, NEMO_TYPE_JUMP_SUBSTRING);
    g_assert_cmpint (match ("curv", "misscurve.txt", FALSE), ==, NEMO_TYPE_JUMP_SUBSTRING);
    g_assert_cmpint (match ("CURVE", "curveycase.txt", FALSE), ==, NEMO_TYPE_JUMP_PREFIX);
    g_assert_cmpint (match ("mcv", "misscurve.txt", FALSE), ==, NEMO_TYPE_JUMP_NO_MATCH);
    g_assert_cmpint (match ("curve", "cu-r-ve.txt", FALSE), ==, NEMO_TYPE_JUMP_NO_MATCH);
    g_assert_cmpint (match ("curve", "misscurve.txt", TRUE), ==, NEMO_TYPE_JUMP_NO_MATCH);
    g_assert_cmpint (match ("", "misscurve.txt", FALSE), ==, NEMO_TYPE_JUMP_NO_MATCH);
    g_assert_cmpint (match ("strasse", "Straße.txt", FALSE), ==, NEMO_TYPE_JUMP_PREFIX);
    g_assert_cmpint (match ("CAFÉ", "Cafe\xcc\x81.txt", FALSE), ==, NEMO_TYPE_JUMP_PREFIX);
}

static void
assert_bold (const char *query, const char *name, gboolean prefix, guint start, guint end)
{
    PangoAttrList *attrs = nemo_type_jump_match_attrs (query, name, prefix);
    g_assert_nonnull (attrs);
    PangoAttrIterator *iter = pango_attr_list_get_iterator (attrs);
    guint found = 0;
    do {
        PangoAttribute *weight = pango_attr_iterator_get (iter, PANGO_ATTR_WEIGHT);
        if (weight != NULL) {
            g_assert_cmpuint (weight->start_index, ==, start);
            g_assert_cmpuint (weight->end_index, ==, end);
            g_assert_cmpint (((PangoAttrInt *) weight)->value, ==, PANGO_WEIGHT_BOLD);
            found++;
        }
    } while (pango_attr_iterator_next (iter));
    g_assert_cmpuint (found, ==, 1);
    pango_attr_iterator_destroy (iter);
    pango_attr_list_unref (attrs);
}

static void
test_highlight_offsets (void)
{
    assert_bold ("CU", "misscurve.txt", FALSE, 4, 6);
    assert_bold ("curv", "curveycase.txt", TRUE, 0, 4);
    assert_bold ("strasse", "Straße.txt", FALSE, 0, 7);
    assert_bold ("sse", "Straße.txt", FALSE, 4, 7);
    assert_bold ("cafe", "Café.txt", FALSE, 0, 5);
    assert_bold ("café", "Cafe\xcc\x81.txt", FALSE, 0, 6);
    assert_bold ("ẹ́", "e\xcc\x81\xcc\xa3.txt", FALSE, 0, 5);
    assert_bold ("fi", "ﬃ.txt", FALSE, 0, 3);
    assert_bold ("ss.c", "miss.curve.txt", FALSE, 2, 6);
    g_assert_null (nemo_type_jump_match_attrs ("mcv", "misscurve.txt", FALSE));
    g_assert_null (nemo_type_jump_match_attrs ("curve", "misscurve.txt", TRUE));
    g_assert_null (nemo_type_jump_match_attrs ("", "misscurve.txt", FALSE));
    PangoAttrList *attrs = nemo_type_jump_match_attrs ("cur", "cur_cur.txt", FALSE);
    PangoAttrIterator *iter = pango_attr_list_get_iterator (attrs);
    guint found = 0;
    do {
        PangoAttribute *weight = pango_attr_iterator_get (iter, PANGO_ATTR_WEIGHT);
        if (weight != NULL) {
            g_assert_cmpuint (weight->start_index, ==, found == 0 ? 0 : 4);
            g_assert_cmpuint (weight->end_index, ==, found == 0 ? 3 : 7);
            found++;
        }
    } while (pango_attr_iterator_next (iter));
    g_assert_cmpuint (found, ==, 2);
    pango_attr_iterator_destroy (iter);
    pango_attr_list_unref (attrs);
}

static void
test_navigation_bindings (void)
{
    GdkEventKey event = { .type = GDK_KEY_PRESS };
    const guint forward[] = { GDK_KEY_Down, GDK_KEY_KP_Down };
    const guint backward[] = { GDK_KEY_Up, GDK_KEY_KP_Up };
    for (guint i = 0; i < G_N_ELEMENTS (forward); i++) {
        event.keyval = forward[i];
        g_assert_cmpint (nemo_type_jump_key_direction (&event), ==, 1);
        event.keyval = backward[i];
        g_assert_cmpint (nemo_type_jump_key_direction (&event), ==, -1);
    }
    const guint unused[] = { GDK_KEY_Right, GDK_KEY_Left, GDK_KEY_KP_Right, GDK_KEY_KP_Left };
    for (guint i = 0; i < G_N_ELEMENTS (unused); i++) {
        event.keyval = unused[i];
        g_assert_cmpint (nemo_type_jump_key_direction (&event), ==, 0);
    }
    event.keyval = GDK_KEY_G;
    event.state = GDK_CONTROL_MASK | GDK_LOCK_MASK;
    g_assert_cmpint (nemo_type_jump_key_direction (&event), ==, 0);
    event.state = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
    g_assert_cmpint (nemo_type_jump_key_direction (&event), ==, 0);
    g_settings_set_string (nemo_keybinding_settings, "type-jump-next", "<Alt>n");
    event.keyval = GDK_KEY_Down;
    event.state = 0;
    g_assert_cmpint (nemo_type_jump_key_direction (&event), ==, 0);
    event.keyval = GDK_KEY_n;
    event.state = GDK_MOD1_MASK | GDK_LOCK_MASK;
    g_assert_cmpint (nemo_type_jump_key_direction (&event), ==, 1);
    g_settings_set_string (nemo_keybinding_settings, "type-jump-previous", "Left");
    event.keyval = GDK_KEY_KP_Left;
    event.state = 0;
    g_assert_cmpint (nemo_type_jump_key_direction (&event), ==, -1);
    g_settings_set_string (nemo_keybinding_settings, "type-jump-next", "");
    g_settings_set_string (nemo_keybinding_settings, "type-jump-previous", "");
    const char *legacy[] = {
        "type-jump-next-alt", "type-jump-previous-alt",
        "type-jump-next-secondary", "type-jump-previous-secondary"
    };
    for (guint i = 0; i < G_N_ELEMENTS (legacy); i++) {
        g_settings_set_string (nemo_keybinding_settings, legacy[i], "F8");
        event.keyval = GDK_KEY_F8;
        g_assert_cmpint (nemo_type_jump_key_direction (&event), ==, 0);
    }
    event.keyval = GDK_KEY_n;
    event.state = GDK_MOD1_MASK;
    g_assert_cmpint (nemo_type_jump_key_direction (&event), ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
    nemo_keybinding_settings = g_settings_new ("org.nemo.keybindings");
    g_test_add_func ("/type-jump/contiguous-ranking", test_contiguous_ranking);
    g_test_add_func ("/type-jump/highlight-offsets", test_highlight_offsets);
    g_test_add_func ("/type-jump/navigation-bindings", test_navigation_bindings);
    int result = g_test_run ();
    g_clear_object (&nemo_keybinding_settings);
    return result;
}
