/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nemo-archive-source.h"
#include "../libnemo-private/nemo-archive-file.h"

#include <gst/gst.h>
#include <string.h>

typedef struct {
    GstBin parent;
    GstElement *source;
    char *uri;
    gboolean ready;
} NemoArchiveSource;

typedef GstBinClass NemoArchiveSourceClass;

static void archive_uri_iface_init (GstURIHandlerInterface *iface);
G_DEFINE_TYPE_WITH_CODE (NemoArchiveSource, nemo_archive_source, GST_TYPE_BIN,
                        G_IMPLEMENT_INTERFACE (GST_TYPE_URI_HANDLER, archive_uri_iface_init))

static GstStaticPadTemplate source_template =
    GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);

static GstURIType
archive_uri_type (GType type)
{
    return GST_URI_SRC;
}

static const char * const *
archive_uri_protocols (GType type)
{
    static const char *protocols[] = { "nemo-archive", NULL };
    return protocols;
}

static char *
archive_uri_get (GstURIHandler *handler)
{
    NemoArchiveSource *self = (NemoArchiveSource *) handler;
    char *uri;
    GST_OBJECT_LOCK (self);
    uri = g_strdup (self->uri);
    GST_OBJECT_UNLOCK (self);
    return uri;
}

static gboolean
archive_uri_set (GstURIHandler *handler, const char *uri, GError **error)
{
    NemoArchiveSource *self = (NemoArchiveSource *) handler;
    GFile *file = NULL, *archive = NULL;
    char *scheme = uri ? g_uri_parse_scheme (uri) : NULL;
    gboolean success = FALSE;

    if (uri && (!scheme || g_ascii_strcasecmp (scheme, "nemo-archive") != 0)) {
        g_set_error_literal (error, GST_URI_ERROR, GST_URI_ERROR_UNSUPPORTED_PROTOCOL,
                             "This source supports only nemo-archive URIs");
        g_free (scheme);
        return FALSE;
    }
    g_free (scheme);
    if (uri) {
        file = g_file_new_for_uri (uri);
        archive = nemo_archive_file_get_archive (file);
        if (!archive) {
            g_set_error_literal (error, GST_URI_ERROR, GST_URI_ERROR_BAD_URI,
                                 "Invalid archive member URI");
            g_object_unref (file);
            return FALSE;
        }
        g_object_unref (archive);
    }
    GST_STATE_LOCK (self);
    if (GST_STATE (self) > GST_STATE_READY || GST_STATE_PENDING (self) > GST_STATE_READY) {
        g_set_error_literal (error, GST_URI_ERROR, GST_URI_ERROR_BAD_STATE,
                             "Cannot change the archive URI while the source is active");
    } else if (!self->ready) {
        g_set_error_literal (error, GST_URI_ERROR, GST_URI_ERROR_BAD_REFERENCE,
                             "The GStreamer GIO source is unavailable");
    } else {
        /* giosrc's registry-cached protocol list predates our GIO registration.
         * Its GFile property bypasses URI factory selection while retaining its
         * streaming, scheduling, cancellation, query and seek implementation. */
        g_object_set (self->source, "file", file, NULL);
        GST_OBJECT_LOCK (self);
        g_free (self->uri);
        self->uri = uri ? g_file_get_uri (file) : NULL;
        GST_OBJECT_UNLOCK (self);
        success = TRUE;
    }
    GST_STATE_UNLOCK (self);
    g_clear_object (&file);
    return success;
}

static void
archive_uri_iface_init (GstURIHandlerInterface *iface)
{
    iface->get_type = archive_uri_type;
    iface->get_protocols = archive_uri_protocols;
    iface->get_uri = archive_uri_get;
    iface->set_uri = archive_uri_set;
}

static GstStateChangeReturn
archive_source_change_state (GstElement *element, GstStateChange transition)
{
    NemoArchiveSource *self = (NemoArchiveSource *) element;
    if (transition == GST_STATE_CHANGE_NULL_TO_READY && !self->ready) {
        GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN,
                           ("The GStreamer GIO source is unavailable"), (NULL));
        return GST_STATE_CHANGE_FAILURE;
    }
    if (transition == GST_STATE_CHANGE_READY_TO_PAUSED && !self->uri) {
        GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ,
                           ("No archive member URI was supplied"), (NULL));
        return GST_STATE_CHANGE_FAILURE;
    }
    return GST_ELEMENT_CLASS (nemo_archive_source_parent_class)->change_state (element, transition);
}

static void
archive_source_finalize (GObject *object)
{
    NemoArchiveSource *self = (NemoArchiveSource *) object;
    g_free (self->uri);
    G_OBJECT_CLASS (nemo_archive_source_parent_class)->finalize (object);
}

static void
nemo_archive_source_class_init (NemoArchiveSourceClass *klass)
{
    GstElementClass *element = GST_ELEMENT_CLASS (klass);
    G_OBJECT_CLASS (klass)->finalize = archive_source_finalize;
    element->change_state = archive_source_change_state;
    gst_element_class_set_static_metadata (element, "Nemo archive member source",
                                            "Source/File", "Stream read-only archive members through GIO",
                                            "Nemo contributors");
    gst_element_class_add_static_pad_template (element, &source_template);
}

static void
nemo_archive_source_init (NemoArchiveSource *self)
{
    GstPad *target, *ghost;
    GstPadTemplate *template;
    GParamSpec *property;
    GST_OBJECT_FLAG_SET (self, GST_ELEMENT_FLAG_SOURCE);
    self->source = gst_element_factory_make ("giosrc", NULL);
    if (!self->source)
        return;
    property = g_object_class_find_property (G_OBJECT_GET_CLASS (self->source), "file");
    if (!property || !(property->flags & G_PARAM_WRITABLE) ||
        !g_type_is_a (G_PARAM_SPEC_VALUE_TYPE (property), G_TYPE_FILE)) {
        gst_object_unref (self->source);
        self->source = NULL;
        return;
    }
    if (!gst_bin_add (GST_BIN (self), self->source)) {
        gst_object_unref (self->source);
        self->source = NULL;
        return;
    }
    target = gst_element_get_static_pad (self->source, "src");
    if (!target)
        return;
    template = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (self), "src");
    ghost = gst_ghost_pad_new_from_template ("src", target, template);
    gst_object_unref (target);
    if (!ghost)
        return;
    if (!gst_element_add_pad (GST_ELEMENT (self), ghost)) {
        gst_object_unref (ghost);
        return;
    }
    self->ready = TRUE;
}

gboolean
nemo_archive_source_register (GError **error)
{
    static gsize initialized;
    static GError *registration_error;

    if (!gst_is_initialized ()) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                             "GStreamer must be initialized before registering archive playback");
        return FALSE;
    }
    if (g_once_init_enter (&initialized)) {
        nemo_archive_file_register ();
        if (!gst_element_register (NULL, "nemoarchivesrc", GST_RANK_PRIMARY,
                                   nemo_archive_source_get_type ()))
            g_set_error_literal (&registration_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "Cannot register the archive playback source");
        g_once_init_leave (&initialized, 1);
    }
    if (registration_error) {
        g_propagate_error (error, g_error_copy (registration_error));
        return FALSE;
    }
    return TRUE;
}
