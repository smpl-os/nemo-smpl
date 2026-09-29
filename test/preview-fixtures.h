#ifndef NEMO_PREVIEW_FIXTURES_H
#define NEMO_PREVIEW_FIXTURES_H

#include <glib.h>

static void
fixture_u16 (GByteArray *bytes, guint16 value)
{
	value = GUINT16_TO_LE (value);
	g_byte_array_append (bytes, (guint8 *) &value, 2);
}

static void
fixture_u32 (GByteArray *bytes, guint32 value)
{
	value = GUINT32_TO_LE (value);
	g_byte_array_append (bytes, (guint8 *) &value, 4);
}

static void
fixture_tag (GByteArray *bytes, guint16 tag, guint16 type, guint32 count, guint32 value)
{
	fixture_u16 (bytes, tag);
	fixture_u16 (bytes, type);
	fixture_u32 (bytes, count);
	fixture_u32 (bytes, value);
}

/* A complete little-endian DNG: a 16x12 RGB thumbnail and a 128x96 CFA
 * SubIFD. Generic TIFF loaders accept the thumbnail without decoding RAW. */
static void
write_dng_fixture (const char *path)
{
	enum { THUMB_TAGS = 13, RAW_TAGS = 17 };
	guint32 raw_ifd = 8 + 2 + 12 * THUMB_TAGS + 4;
	guint32 extra = raw_ifd + 2 + 12 * RAW_TAGS + 4;
	guint32 model = extra + 6;
	const char camera[] = "Nemo test camera";
	guint32 matrix = model + sizeof camera;
	guint32 neutral = matrix + 9 * 8;
	guint32 thumbnail = neutral + 3 * 8;
	guint32 raw_data = thumbnail + 16 * 12 * 3;
	GByteArray *bytes = g_byte_array_new ();
	GError *error = NULL;

	g_byte_array_append (bytes, (const guint8 *) "II", 2);
	fixture_u16 (bytes, 42);
	fixture_u32 (bytes, 8);
	fixture_u16 (bytes, THUMB_TAGS);
	fixture_tag (bytes, 254, 4, 1, 1);                  /* Reduced-resolution IFD. */
	fixture_tag (bytes, 256, 4, 1, 16);
	fixture_tag (bytes, 257, 4, 1, 12);
	fixture_tag (bytes, 258, 3, 3, extra);
	fixture_tag (bytes, 259, 3, 1, 1);
	fixture_tag (bytes, 262, 3, 1, 2);                  /* RGB thumbnail. */
	fixture_tag (bytes, 273, 4, 1, thumbnail);
	fixture_tag (bytes, 277, 3, 1, 3);
	fixture_tag (bytes, 278, 4, 1, 12);
	fixture_tag (bytes, 279, 4, 1, 16 * 12 * 3);
	fixture_tag (bytes, 330, 4, 1, raw_ifd);
	fixture_tag (bytes, 50706, 1, 4, 0x00000401);      /* DNG 1.4. */
	fixture_tag (bytes, 50708, 2, sizeof camera, model);
	fixture_u32 (bytes, 0);
	g_assert_cmpuint (bytes->len, ==, raw_ifd);

	fixture_u16 (bytes, RAW_TAGS);
	fixture_tag (bytes, 254, 4, 1, 0);
	fixture_tag (bytes, 256, 4, 1, 128);
	fixture_tag (bytes, 257, 4, 1, 96);
	fixture_tag (bytes, 258, 3, 1, 16);
	fixture_tag (bytes, 259, 3, 1, 1);
	fixture_tag (bytes, 262, 3, 1, 32803);             /* CFA. */
	fixture_tag (bytes, 273, 4, 1, raw_data);
	fixture_tag (bytes, 277, 3, 1, 1);
	fixture_tag (bytes, 278, 4, 1, 96);
	fixture_tag (bytes, 279, 4, 1, 128 * 96 * 2);
	fixture_tag (bytes, 284, 3, 1, 1);
	fixture_tag (bytes, 33421, 3, 2, 0x00020002);
	fixture_tag (bytes, 33422, 1, 4, 0x02010100);      /* RGGB. */
	fixture_tag (bytes, 50717, 4, 1, 65535);
	fixture_tag (bytes, 50721, 10, 9, matrix);
	fixture_tag (bytes, 50728, 5, 3, neutral);
	fixture_tag (bytes, 50778, 3, 1, 21);             /* D65. */
	fixture_u32 (bytes, 0);
	g_assert_cmpuint (bytes->len, ==, extra);
	for (guint i = 0; i < 3; i++)
		fixture_u16 (bytes, 8);
	g_byte_array_append (bytes, (const guint8 *) camera, sizeof camera);
	for (guint i = 0; i < 9; i++) {
		fixture_u32 (bytes, i % 4 == 0 ? 1 : 0);
		fixture_u32 (bytes, 1);
	}
	for (guint i = 0; i < 3; i++) {
		fixture_u32 (bytes, 1);
		fixture_u32 (bytes, 1);
	}
	g_assert_cmpuint (bytes->len, ==, thumbnail);
	for (guint i = 0; i < 16 * 12; i++)
		g_byte_array_append (bytes, (const guint8 *) "\0\0\xff", 3);
	g_assert_cmpuint (bytes->len, ==, raw_data);
	for (guint y = 0; y < 96; y++)
		for (guint x = 0; x < 128; x++)
			fixture_u16 (bytes, 10000 + x * 100 + y * 100);
	g_assert_true (g_file_set_contents (path, (const char *) bytes->data, bytes->len, &error));
	g_assert_no_error (error);
	g_byte_array_unref (bytes);
}

#endif
