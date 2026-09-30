/*
 * Native document preview worker. Run only inside the preview sandbox.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <md4c-html.h>
#include <poppler.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_DOCUMENT_BYTES (128 * 1024 * 1024)
#define MAX_MARKDOWN_BYTES (16 * 1024 * 1024)
#define MAX_HTML_BYTES (16 * 1024 * 1024)
#define MAX_PNG_BYTES (40 * 1024 * 1024)
#define MAX_PAGES 10000
#define MAX_WIDTH 2400
#define MAX_HEIGHT 3200
#define MAX_PIXELS 8000000
#define MIN_PAGE_POINTS 1.0
#define MAX_PAGE_POINTS 20000.0
#define MAX_QUERY_BYTES 1024
#define MAX_METADATA_BYTES 4096

typedef struct {
    GString *text;
    gboolean overflow;
} HtmlOutput;

typedef struct {
    int fd;
    gsize written;
} PngOutput;

typedef struct {
    guint depth;
    gboolean root_seen;
    gboolean body_seen;
} Fb2Validation;

typedef struct {
    double red;
    double green;
    double blue;
} ThemeColor;

static gboolean
fail (GError **error, const char *message)
{
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, message);
    return FALSE;
}

static gboolean
file_error (GError **error, const char *operation)
{
    int saved_errno = errno;

    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved_errno),
                 "%s: %s", operation, g_strerror (saved_errno));
    return FALSE;
}

static gboolean
parse_color (const char *text, ThemeColor *color, GError **error)
{
    guint value = 0;
    guint i;

    if (strnlen (text, 8) != 7 || text[0] != '#')
        return fail (error, "Theme colors must be opaque #RRGGBB values");
    for (i = 1; i < 7; i++) {
        int digit = g_ascii_xdigit_value (text[i]);

        if (digit < 0)
            return fail (error, "Theme colors must be opaque #RRGGBB values");
        value = (value << 4) | digit;
    }
    if (color != NULL) {
        color->red = ((value >> 16) & 255) / 255.0;
        color->green = ((value >> 8) & 255) / 255.0;
        color->blue = (value & 255) / 255.0;
    }
    return TRUE;
}

static int
open_bounded (const char *path, goffset maximum, goffset *size, GError **error)
{
    struct stat st;
    int fd = g_open (path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0);

    if (fd < 0) {
        file_error (error, "Cannot open input");
        return -1;
    }
    if (fstat (fd, &st) != 0) {
        file_error (error, "Cannot inspect input");
        close (fd);
        return -1;
    }
    if (!S_ISREG (st.st_mode) || st.st_size <= 0 || st.st_size > maximum) {
        fail (error, "Input must be a nonempty regular file within the size limit");
        close (fd);
        return -1;
    }
    if (size != NULL)
        *size = st.st_size;
    return fd;
}

static gboolean
distinct_paths (const char *input, const char *output, GError **error)
{
    g_autofree char *a = g_canonicalize_filename (input, NULL);
    g_autofree char *b = g_canonicalize_filename (output, NULL);
    struct stat sa, sb;

    if (strcmp (a, b) == 0 ||
        (g_stat (input, &sa) == 0 && g_stat (output, &sb) == 0 &&
         sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino))
        return fail (error, "Input and output paths must be distinct");
    return TRUE;
}

static int
create_staging_file (const char *destination, char **path, GError **error)
{
    int fd;

    *path = g_strconcat (destination, ".XXXXXX", NULL);
    fd = g_mkstemp_full (*path, O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        file_error (error, "Cannot create output");
        g_clear_pointer (path, g_free);
    }
    return fd;
}

static gboolean
write_json (JsonBuilder *builder, const char *path, GError **error)
{
    g_autoptr(JsonGenerator) generator = json_generator_new ();
    g_autoptr(JsonNode) root = json_builder_get_root (builder);
    g_autofree char *text = NULL;
    gsize length;

    json_generator_set_root (generator, root);
    text = json_generator_to_data (generator, &length);
    return g_file_set_contents (path, text, length, error);
}

static guint16
read_be16 (const guchar *bytes)
{
    return ((guint16) bytes[0] << 8) | bytes[1];
}

static guint32
read_be32 (const guchar *bytes)
{
    return ((guint32) bytes[0] << 24) | ((guint32) bytes[1] << 16) |
           ((guint32) bytes[2] << 8) | bytes[3];
}

static gboolean
validate_palmdoc_record (FILE *file, guint32 begin, guint32 end,
                         guint32 expected, guint32 previous)
{
    guint32 position = begin, produced = 0;

    if (fseek (file, begin, SEEK_SET) != 0)
        return FALSE;
    while (produced < expected && position < end) {
        int byte = fgetc (file);
        guint length;

        if (byte == EOF)
            return FALSE;
        position++;
        if (byte >= 1 && byte <= 8) {
            length = byte;
            if (length > end - position || fseek (file, length, SEEK_CUR) != 0)
                return FALSE;
            position += length;
        } else if (byte <= 0x7f) {
            length = 1;
        } else if (byte <= 0xbf) {
            int next;
            guint pair, distance;

            if (position == end || (next = fgetc (file)) == EOF)
                return FALSE;
            position++;
            pair = (byte << 8) | next;
            distance = (pair >> 3) & 0x7ff;
            if (distance == 0 || distance > previous + produced)
                return FALSE;
            length = (pair & 7) + 3;
        } else {
            length = 2;
        }
        if (length > expected - produced)
            return FALSE;
        produced += length;
    }
    return produced == expected;
}

static gboolean
inspect_mobi (const char *path, GError **error)
{
    guchar header[78], palm[32];
    g_autofree guchar *records = NULL;
    goffset size;
    guint count, text_records, compression, i;
    guint32 previous = 0, first = 0, second = 0, text_length;
    int fd = open_bounded (path, MAX_DOCUMENT_BYTES, &size, error);
    FILE *file;
    gboolean result = FALSE;

    if (fd < 0)
        return FALSE;
    file = fdopen (fd, "rb");
    if (file == NULL) {
        close (fd);
        return file_error (error, "Cannot read MOBI");
    }
    if (fread (header, 1, sizeof header, file) != sizeof header)
        goto malformed;
    if (memcmp (header + 60, "BOOKMOBI", 8) != 0 &&
        memcmp (header + 60, "TEXtREAd", 8) != 0) {
        fail (error, "Unsupported MOBI/Palm database type");
        goto out;
    }
    count = read_be16 (header + 76);
    if (count < 2 || read_be32 (header + 72) != 0 ||
        size < 78 + count * 8 + 16)
        goto malformed;
    records = g_malloc (count * 8);
    if (fread (records, 8, count, file) != count)
        goto malformed;
    for (i = 0; i < count; i++) {
        guint32 offset = read_be32 (records + i * 8);

        if (offset < 78 + count * 8 || offset >= size ||
            (i > 0 && offset <= previous))
            goto malformed;
        if (i == 0)
            first = offset;
        else if (i == 1)
            second = offset;
        previous = offset;
    }
    if (second - first < 16 || fseek (file, first, SEEK_SET) != 0 ||
        fread (palm, 1, 16, file) != 16)
        goto malformed;
    if (read_be16 (palm + 12) != 0) {
        fail (error, "Encrypted/DRM-protected MOBI is not supported");
        goto out;
    }
    compression = read_be16 (palm);
    if (compression != 1 && compression != 2) {
        fail (error, "Unsupported MOBI compression (only uncompressed and PalmDOC; no HUFF/CDIC)");
        goto out;
    }
    text_length = read_be32 (palm + 4);
    text_records = read_be16 (palm + 8);
    if (text_records == 0 || text_records >= count || text_length == 0 ||
        text_length > MAX_MARKDOWN_BYTES ||
        read_be16 (palm + 10) != 4096 ||
        text_records != (text_length + 4095) / 4096)
        goto malformed;
    if (second - first > 16) {
        if (second - first < 32 || fread (palm + 16, 1, 16, file) != 16)
            goto malformed;
        if (memcmp (palm + 16, "MOBI", 4) == 0) {
            guint32 length = read_be32 (palm + 20);

            if (length < 16 || length > second - first - 16)
                goto malformed;
        }
    }
    for (i = 1; i <= text_records; i++) {
        guint32 begin = read_be32 (records + i * 8);
        guint32 end = i + 1 < count ? read_be32 (records + (i + 1) * 8) : size;
        guint32 previous_text = (i - 1) * 4096;
        guint32 required = MIN (4096, text_length - previous_text);

        if ((compression == 1 && end - begin < required) ||
            (compression == 2 &&
             !validate_palmdoc_record (file, begin, end, required, previous_text)))
            goto malformed;
    }
    result = TRUE;
    goto out;

malformed:
    fail (error, "Malformed or unsupported MOBI/PalmDOC header or record offsets");
out:
    fclose (file);
    return result;
}

static void
fb2_start_element (GMarkupParseContext *context, const char *name,
                   const char **attribute_names, const char **attribute_values,
                   gpointer userdata, GError **error)
{
    Fb2Validation *validation = userdata;
    const char *local = strrchr (name, ':');

    (void) context;
    (void) attribute_names;
    (void) attribute_values;
    local = local != NULL ? local + 1 : name;
    validation->depth++;
    if (validation->depth > 128) {
        fail (error, "FB2 XML nesting exceeds the preview limit");
    } else if (validation->depth == 1) {
        if (validation->root_seen || strcmp (local, "FictionBook") != 0)
            fail (error, "FB2 XML must have a single FictionBook root");
        validation->root_seen = TRUE;
    } else if (validation->depth == 2 && strcmp (local, "body") == 0) {
        validation->body_seen = TRUE;
    }
}

static void
fb2_end_element (GMarkupParseContext *context, const char *name,
                 gpointer userdata, GError **error)
{
    Fb2Validation *validation = userdata;

    (void) context;
    (void) name;
    (void) error;
    validation->depth--;
}

static void
fb2_passthrough (GMarkupParseContext *context, const char *text, gsize length,
                 gpointer userdata, GError **error)
{
    (void) context;
    (void) userdata;
    if (length >= 9 && memcmp (text, "<!DOCTYPE", 9) == 0)
        fail (error, "FB2 document type declarations are not supported");
}

static gboolean
inspect_fb2 (const char *path, GError **error)
{
    const GMarkupParser parser = {
        fb2_start_element, fb2_end_element, NULL, fb2_passthrough, NULL
    };
    Fb2Validation validation = { 0, FALSE, FALSE };
    g_autoptr(GMarkupParseContext) context = NULL;
    char buffer[4096];
    gsize length, total = 0;
    FILE *file;
    gboolean result = FALSE;
    int fd = open_bounded (path, MAX_DOCUMENT_BYTES, NULL, error);

    if (fd < 0)
        return FALSE;
    file = fdopen (fd, "rb");
    if (file == NULL) {
        close (fd);
        return file_error (error, "Cannot read FB2");
    }
    context = g_markup_parse_context_new (&parser, G_MARKUP_TREAT_CDATA_AS_TEXT,
                                          &validation, NULL);
    while ((length = fread (buffer, 1, sizeof buffer, file)) > 0) {
        if (length > MAX_DOCUMENT_BYTES - total) {
            fail (error, "FB2 exceeds the input size limit");
            goto out;
        }
        total += length;
        if (!g_markup_parse_context_parse (context, buffer, length, error))
            goto out;
    }
    if (ferror (file)) {
        file_error (error, "Cannot read FB2");
        goto out;
    }
    if (!g_markup_parse_context_end_parse (context, error))
        goto out;
    if (!validation.root_seen || !validation.body_seen) {
        fail (error, "FB2 XML must contain a FictionBook root and body");
        goto out;
    }
    result = TRUE;
out:
    fclose (file);
    return result;
}

static void
append_html (const MD_CHAR *text, MD_SIZE length, void *userdata)
{
    HtmlOutput *output = userdata;

    if (output->overflow)
        return;
    if (length > MAX_HTML_BYTES - output->text->len) {
        output->overflow = TRUE;
        return;
    }
    g_string_append_len (output->text, text, length);
}

static gboolean
markdown_to_html (const char *input, const char *output, GError **error)
{
    static const char prefix[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"/>"
        "<style>body{font-family:serif;line-height:1.5;margin:24px;color:#202020}"
        "h1,h2,h3,h4{font-family:sans-serif;line-height:1.2}"
        "pre,code{font-family:monospace;background:#f2f2f2}"
        "pre{padding:8px;white-space:pre-wrap}"
        "table{border-collapse:collapse}td,th{border:1px solid #888;padding:5px}"
        "blockquote{border-left:3px solid #888;margin-left:8px;padding-left:12px}"
        "a{color:#245b98}</style></head><body>";
    static const char suffix[] = "</body></html>";
    g_autofree char *source = NULL;
    HtmlOutput html = { g_string_new (NULL), FALSE };
    GString *safe = NULL;
    goffset size;
    gsize length;
    FILE *file = NULL;
    int fd = open_bounded (input, MAX_MARKDOWN_BYTES, &size, error);
    gboolean result = FALSE;
    const char *cursor;

    if (fd < 0)
        goto out;
    file = fdopen (fd, "rb");
    if (file == NULL) {
        close (fd);
        file_error (error, "Cannot read Markdown");
        goto out;
    }
    length = size;
    source = g_malloc (length + 1);
    if (fread (source, 1, length, file) != length || fgetc (file) != EOF || ferror (file)) {
        fail (error, "Cannot read bounded Markdown input");
        goto out;
    }
    source[size] = '\0';
    if (!g_utf8_validate (source, size, NULL)) {
        fail (error, "Markdown must contain valid UTF-8 without embedded NUL bytes");
        goto out;
    }
    append_html (prefix, sizeof prefix - 1, &html);
    if (md_html (source, size, append_html, &html,
                 MD_DIALECT_GITHUB | MD_FLAG_NOHTML, MD_HTML_FLAG_XHTML) != 0) {
        fail (error, "Cannot parse Markdown");
        goto out;
    }
    append_html (suffix, sizeof suffix - 1, &html);
    if (html.overflow) {
        fail (error, "Markdown HTML exceeds the 16 MiB output limit");
        goto out;
    }

    /* Only md4c-generated tags are present. Linked assets are not staged. */
    safe = g_string_sized_new (html.text->len);
    cursor = html.text->str;
    while (*cursor != '\0') {
        const char *image = strstr (cursor, "<img ");
        const char *end;

        if (image == NULL) {
            g_string_append (safe, cursor);
            break;
        }
        g_string_append_len (safe, cursor, image - cursor);
        end = strchr (image, '>');
        if (end == NULL) {
            fail (error, "Invalid generated Markdown image tag");
            goto out;
        }
        g_string_append (safe, "[image]");
        cursor = end + 1;
    }
    result = g_file_set_contents (output, safe->str, safe->len, error);
out:
    if (file != NULL)
        fclose (file);
    if (safe != NULL)
        g_string_free (safe, TRUE);
    g_string_free (html.text, TRUE);
    return result;
}

static PopplerDocument *
open_document (const char *path, GError **error)
{
    PopplerDocument *document;
    int pages;
    int fd = open_bounded (path, MAX_DOCUMENT_BYTES, NULL, error);

    if (fd < 0)
        return NULL;
    /* Poppler takes ownership of the descriptor, including on failure. */
    document = poppler_document_new_from_fd (fd, NULL, error);
    if (document == NULL)
        return NULL;
    pages = poppler_document_get_n_pages (document);
    if (pages < 1 || pages > MAX_PAGES) {
        fail (error, "PDF must contain between 1 and 10000 pages");
        g_object_unref (document);
        return NULL;
    }
    return document;
}

static void
add_metadata (JsonBuilder *builder, const char *name, char *value)
{
    gsize length;
    const char *end;

    if (value == NULL)
        return;
    length = strnlen (value, MAX_METADATA_BYTES);
    if (!g_utf8_validate (value, length, &end))
        length = end - value;
    value[length] = '\0';
    if (length > 0) {
        json_builder_set_member_name (builder, name);
        json_builder_add_string_value (builder, value);
    }
    g_free (value);
}

static gboolean
write_reader_stylesheet (const char *path, const char *kind, const char *background,
                         const char *foreground, GError **error)
{
    g_autoptr(GString) css = g_string_new ("");

    /* Older MuPDF versions apply a chapter break even before the first content. */
    if (strcmp (kind, "fb2") == 0)
        g_string_append (css,
            "description>title-info>coverpage{page-break-before:auto}"
            "body>section>title{page-break-before:auto}"
            "body>section+section{page-break-before:always}\n");
    if (background != NULL)
        g_string_append_printf (css,
        "body{font-family:serif;line-height:1.5;margin:24px}"
        "h1,h2,h3,h4{font-family:sans-serif;line-height:1.2}"
        "pre,code{font-family:monospace}"
        "pre{padding:8px;white-space:pre-wrap}"
        "table{border-collapse:collapse}td,th{border:1px solid %s;padding:5px}"
        "blockquote{border-left:3px solid %s;margin-left:8px;padding-left:12px}"
        "*{color:%s !important;background-color:%s !important;"
        "background-image:none !important;border-color:%s !important}"
        "html,body{background:%s !important}\n",
        foreground, foreground, foreground, background, foreground, background);

    return g_file_set_contents (path, css->str, css->len, error);
}

static gboolean
prepare_document (const char *kind, const char *input, const char *pdf,
                  const char *metadata, const char *background,
                  const char *foreground, GError **error)
{
    const char *kinds[] = { "markdown", "epub", "fb2", "mobi", "pdf", "cbz", NULL };
    g_autofree char *source = g_canonicalize_filename (input, NULL);
    g_autofree char *staged_pdf = NULL;
    g_autofree char *html = NULL;
    g_autofree char *stylesheet = NULL;
    g_autoptr(GSubprocess) converter = NULL;
    g_autoptr(JsonBuilder) builder = NULL;
    PopplerDocument *document = NULL;
    gboolean result = FALSE;
    const char *argv[18] = {
        "mutool", "convert", "-F", "pdf", "-W", "600", "-H", "800", "-S", "12"
    };
    guint argc = 10;
    int fd;

    if (!g_strv_contains (kinds, kind))
        return fail (error, "Unsupported document kind");
    if (background != NULL) {
        if (strcmp (kind, "pdf") == 0 || strcmp (kind, "cbz") == 0)
            return fail (error, "Theme colors are supported only for reflowable documents");
        if (!parse_color (background, NULL, error) ||
            !parse_color (foreground, NULL, error))
            return FALSE;
    }
    if (!distinct_paths (input, pdf, error) ||
        !distinct_paths (input, metadata, error) ||
        !distinct_paths (pdf, metadata, error))
        return FALSE;
    fd = open_bounded (input, MAX_DOCUMENT_BYTES, NULL, error);
    if (fd < 0)
        return FALSE;
    close (fd);
    if (strcmp (kind, "mobi") == 0 && !inspect_mobi (input, error))
        return FALSE;
    if (strcmp (kind, "fb2") == 0 && !inspect_fb2 (input, error))
        return FALSE;
    fd = create_staging_file (pdf, &staged_pdf, error);
    if (fd < 0)
        goto out;
    close (fd);
    if (strcmp (kind, "markdown") == 0) {
        html = g_strconcat (staged_pdf, ".html", NULL);
        if (!markdown_to_html (input, html, error))
            goto out;
        g_free (source);
        source = g_canonicalize_filename (html, NULL);
    }
    if (background != NULL || strcmp (kind, "fb2") == 0) {
        stylesheet = g_strconcat (staged_pdf, ".css", NULL);
        if (!write_reader_stylesheet (stylesheet, kind, background, foreground, error))
            goto out;
        argv[argc++] = "-U";
        argv[argc++] = stylesheet;
    }
    if (background != NULL) {
        /* MuPDF's user CSS alone does not override publisher inline colors. */
        argv[argc++] = "-X";
    }
    argv[argc++] = "-o";
    argv[argc++] = staged_pdf;
    argv[argc++] = source;
    argv[argc] = NULL;
    converter = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_NONE, error);
    if (converter == NULL || !g_subprocess_wait_check (converter, NULL, error))
        goto out;
    document = open_document (staged_pdf, error);
    if (document == NULL)
        goto out;
    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "page_count");
    json_builder_add_int_value (builder, poppler_document_get_n_pages (document));
    add_metadata (builder, "title", poppler_document_get_title (document));
    add_metadata (builder, "author", poppler_document_get_author (document));
    json_builder_end_object (builder);
    if (g_rename (staged_pdf, pdf) != 0) {
        file_error (error, "Cannot publish PDF");
        goto out;
    }
    result = write_json (builder, metadata, error);
out:
    if (document != NULL)
        g_object_unref (document);
    if (html != NULL)
        g_unlink (html);
    if (stylesheet != NULL)
        g_unlink (stylesheet);
    if (staged_pdf != NULL)
        g_unlink (staged_pdf);
    return result;
}

static gboolean
parse_number (const char *text, guint maximum, int *number, GError **error)
{
    guint value = 0;
    const char *p;

    if (*text == '\0')
        return fail (error, "Expected a nonnegative decimal integer");
    for (p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || value > maximum / 10 ||
            (value == maximum / 10 && (guint) (*p - '0') > maximum % 10))
            return fail (error, "Invalid or out-of-range numeric argument");
        value = value * 10 + (*p - '0');
    }
    *number = value;
    return TRUE;
}

static gboolean
check_query (const char *query, GError **error)
{
    gsize length = strnlen (query, MAX_QUERY_BYTES + 1);

    if (length == 0 || length > MAX_QUERY_BYTES ||
        !g_utf8_validate (query, length, NULL))
        return fail (error, "Search text must be nonempty UTF-8, at most 1024 bytes");
    return TRUE;
}

static PopplerPage *
get_page (PopplerDocument *document, int index, double *width, double *height,
          GError **error)
{
    PopplerPage *page;

    if (index >= poppler_document_get_n_pages (document)) {
        fail (error, "Page index is outside the document");
        return NULL;
    }
    page = poppler_document_get_page (document, index);
    if (page == NULL) {
        fail (error, "Cannot read PDF page");
        return NULL;
    }
    poppler_page_get_size (page, width, height);
    if (!isfinite (*width) || !isfinite (*height) ||
        *width < MIN_PAGE_POINTS || *height < MIN_PAGE_POINTS ||
        *width > MAX_PAGE_POINTS || *height > MAX_PAGE_POINTS) {
        fail (error, "Invalid or oversized PDF page dimensions");
        g_object_unref (page);
        return NULL;
    }
    return page;
}

static void
free_rectangle (gpointer rectangle)
{
    poppler_rectangle_free (rectangle);
}

static cairo_status_t
write_png_bytes (void *closure, const unsigned char *bytes, unsigned int length)
{
    PngOutput *output = closure;

    if (length > MAX_PNG_BYTES - output->written)
        return CAIRO_STATUS_WRITE_ERROR;
    while (length > 0) {
        ssize_t written = write (output->fd, bytes, length);

        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return CAIRO_STATUS_WRITE_ERROR;
        bytes += written;
        length -= written;
        output->written += written;
    }
    return CAIRO_STATUS_SUCCESS;
}

static gboolean
render_page (const char *pdf, const char *index_text, const char *width_text,
             const char *height_text, const char *png, const char *background,
             const char *query, GError **error)
{
    PopplerDocument *document = NULL;
    PopplerPage *page = NULL;
    cairo_surface_t *surface = NULL;
    cairo_t *cr = NULL;
    g_autofree char *staged_png = NULL;
    PngOutput output = { -1, 0 };
    ThemeColor color = { 1, 1, 1 };
    gboolean result = FALSE;
    int index, max_width, max_height, width, height;
    double page_width, page_height, scale;
    cairo_status_t status;

    if ((background != NULL && !parse_color (background, &color, error)) ||
        !parse_number (index_text, MAX_PAGES - 1, &index, error) ||
        !parse_number (width_text, MAX_WIDTH, &max_width, error) ||
        !parse_number (height_text, MAX_HEIGHT, &max_height, error))
        return FALSE;
    if (max_width == 0 || max_height == 0 ||
        max_width * max_height > MAX_PIXELS)
        return fail (error, "Render dimensions must be positive and at most 8 million pixels");
    if ((query != NULL && !check_query (query, error)) ||
        !distinct_paths (pdf, png, error))
        return FALSE;
    document = open_document (pdf, error);
    if (document == NULL)
        goto out;
    page = get_page (document, index, &page_width, &page_height, error);
    if (page == NULL)
        goto out;
    scale = MIN (max_width / page_width, max_height / page_height);
    width = CLAMP ((int) floor (page_width * scale + 0.5), 1, max_width);
    height = CLAMP ((int) floor (page_height * scale + 0.5), 1, max_height);
    surface = cairo_image_surface_create (CAIRO_FORMAT_RGB24, width, height);
    cr = cairo_create (surface);
    cairo_set_source_rgb (cr, color.red, color.green, color.blue);
    cairo_paint (cr);
    cairo_scale (cr, MIN (width / page_width, height / page_height),
                 MIN (width / page_width, height / page_height));
    poppler_page_render (page, cr);
    if (query != NULL) {
        GList *matches = poppler_page_find_text (page, query);
        GList *item;

        /* Multiplication is invisible over black; screen preserves bright glyphs. */
        if (0.2126 * color.red + 0.7152 * color.green + 0.0722 * color.blue < 0.5)
            cairo_set_operator (cr, CAIRO_OPERATOR_SCREEN);
        else
            cairo_set_operator (cr, CAIRO_OPERATOR_MULTIPLY);
        cairo_set_source_rgba (cr, 1, 0.8, 0, 0.35);
        for (item = matches; item != NULL; item = item->next) {
            PopplerRectangle *rect = item->data;

            if (!isfinite (rect->x1) || !isfinite (rect->x2) ||
                !isfinite (rect->y1) || !isfinite (rect->y2))
                continue;
            cairo_rectangle (cr, rect->x1, page_height - rect->y2,
                             rect->x2 - rect->x1, rect->y2 - rect->y1);
            cairo_fill (cr);
        }
        g_list_free_full (matches, free_rectangle);
    }
    status = cairo_status (cr);
    if (status == CAIRO_STATUS_SUCCESS)
        status = cairo_surface_status (surface);
    if (status != CAIRO_STATUS_SUCCESS)
        goto cairo_error;
    output.fd = create_staging_file (png, &staged_png, error);
    if (output.fd < 0)
        goto out;
    status = cairo_surface_write_to_png_stream (surface, write_png_bytes, &output);
    if (status != CAIRO_STATUS_SUCCESS)
        goto cairo_error;
    if (close (output.fd) != 0) {
        output.fd = -1;
        file_error (error, "Cannot close PNG output");
        goto out;
    }
    output.fd = -1;
    if (g_rename (staged_png, png) != 0) {
        file_error (error, "Cannot publish PNG");
        goto out;
    }
    result = TRUE;
    goto out;

cairo_error:
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "Cannot render PNG: %s", cairo_status_to_string (status));
out:
    if (output.fd >= 0)
        close (output.fd);
    if (staged_png != NULL)
        g_unlink (staged_png);
    if (cr != NULL)
        cairo_destroy (cr);
    if (surface != NULL)
        cairo_surface_destroy (surface);
    if (page != NULL)
        g_object_unref (page);
    if (document != NULL)
        g_object_unref (document);
    return result;
}

static gboolean
search_document (const char *pdf, const char *query, const char *start_text,
                 const char *direction_text, const char *json, GError **error)
{
    PopplerDocument *document;
    g_autoptr(JsonBuilder) builder = NULL;
    int start, direction, pages, i, found = -1;
    gboolean result = FALSE;

    if (!check_query (query, error) ||
        !parse_number (start_text, MAX_PAGES - 1, &start, error) ||
        !distinct_paths (pdf, json, error))
        return FALSE;
    if (strcmp (direction_text, "1") == 0)
        direction = 1;
    else if (strcmp (direction_text, "-1") == 0)
        direction = -1;
    else
        return fail (error, "Search direction must be 1 or -1");
    document = open_document (pdf, error);
    if (document == NULL)
        return FALSE;
    pages = poppler_document_get_n_pages (document);
    if (start >= pages) {
        fail (error, "Search start page is outside the document");
        goto out;
    }
    for (i = 0; i < pages; i++) {
        int index = (start + direction * i + pages) % pages;
        double width, height;
        PopplerPage *page = get_page (document, index, &width, &height, error);
        GList *matches;

        if (page == NULL)
            goto out;
        matches = poppler_page_find_text (page, query);
        g_object_unref (page);
        if (matches != NULL) {
            g_list_free_full (matches, free_rectangle);
            found = index;
            break;
        }
    }
    builder = json_builder_new ();
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "found");
    json_builder_add_boolean_value (builder, found >= 0);
    if (found >= 0) {
        json_builder_set_member_name (builder, "page");
        json_builder_add_int_value (builder, found);
    }
    json_builder_end_object (builder);
    result = write_json (builder, json, error);
out:
    g_object_unref (document);
    return result;
}

int
main (int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    gboolean success = FALSE;

    if ((argc == 6 || argc == 8) && strcmp (argv[1], "prepare") == 0)
        success = prepare_document (argv[2], argv[3], argv[4], argv[5],
                                    argc == 8 ? argv[6] : NULL,
                                    argc == 8 ? argv[7] : NULL, &error);
    else if ((argc == 7 || argc == 8) && strcmp (argv[1], "render") == 0)
        success = render_page (argv[2], argv[3], argv[4], argv[5], argv[6],
                               NULL, argc == 8 ? argv[7] : NULL, &error);
    else if ((argc == 8 || argc == 9) && strcmp (argv[1], "render-themed") == 0)
        success = render_page (argv[2], argv[3], argv[4], argv[5], argv[6],
                               argv[7], argc == 9 ? argv[8] : NULL, &error);
    else if (argc == 7 && strcmp (argv[1], "search") == 0)
        success = search_document (argv[2], argv[3], argv[4], argv[5], argv[6], &error);
    else
        fail (&error, "Usage: renderer prepare KIND INPUT PDF JSON [BG FG] | render PDF PAGE WIDTH HEIGHT PNG [QUERY] | render-themed PDF PAGE WIDTH HEIGHT PNG BG [QUERY] | search PDF NEEDLE START DIRECTION JSON");

    if (!success) {
        g_printerr ("nemo-document-renderer: %.512s\n",
                    error != NULL ? error->message : "Document operation failed");
        return 1;
    }
    return 0;
}
