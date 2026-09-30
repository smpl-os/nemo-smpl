#!/usr/bin/env python3
"""Exercise the isolated document worker using generated, trusted documents."""

import base64
from collections import Counter
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import unittest
import uuid
import zipfile
import zlib


RENDERER = None
WORK = None


def png_chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload
            + struct.pack(">I", zlib.crc32(kind + payload)))


def make_png(width=96, height=64, color=(235, 20, 25)):
    rows = (b"\0" + bytes(color) * width) * height
    return (b"\x89PNG\r\n\x1a\n"
            + png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + png_chunk(b"IDAT", zlib.compress(rows))
            + png_chunk(b"IEND", b""))


def read_png(path):
    """Decode Cairo's noninterlaced 8-bit RGB/RGBA PNG without extra packages."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssertionError("Not a PNG")
    position = 8
    compressed = bytearray()
    width = height = channels = None
    while position < len(data):
        length = struct.unpack_from(">I", data, position)[0]
        kind = data[position + 4:position + 8]
        payload = data[position + 8:position + 8 + length]
        if kind == b"IHDR":
            width, height, bits, color, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", payload)
            if (bits != 8 or color not in (2, 6)
                    or (compression, filtering, interlace) != (0, 0, 0)):
                raise AssertionError("Unexpected PNG encoding")
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            compressed.extend(payload)
        position += length + 12
    raw = zlib.decompress(compressed)
    stride = width * channels
    if len(raw) != (stride + 1) * height:
        raise AssertionError("Wrong PNG data length")
    previous = bytearray(stride)
    pixels = []
    position = 0
    for _ in range(height):
        filtering = raw[position]
        row = bytearray(raw[position + 1:position + 1 + stride])
        position += stride + 1
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            above = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            if filtering == 1:
                prediction = left
            elif filtering == 2:
                prediction = above
            elif filtering == 3:
                prediction = (left + above) // 2
            elif filtering == 4:
                p = left + above - upper_left
                distances = (abs(p - left), abs(p - above), abs(p - upper_left))
                prediction = (left, above, upper_left)[distances.index(min(distances))]
            elif filtering == 0:
                prediction = 0
            else:
                raise AssertionError("Unknown PNG filter")
            row[x] = (row[x] + prediction) & 255
        pixels.extend(tuple(row[x:x + 3]) for x in range(0, stride, channels))
        if channels == 4 and any(row[x] != 255 for x in range(3, stride, channels)):
            raise AssertionError("Page background must be opaque")
        previous = row
    return width, height, pixels


def make_pdf(path, labels=("FirstSentinel", "MiddleSentinel", "LastSentinel"),
             width=600, height=800, metadata=None):
    """Build small original PDF fixtures, including a proper cross-reference table."""
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        ("<< /Type /Pages /Count %d /Kids [%s] >>" % (
            len(labels), " ".join(f"{4 + 2 * i} 0 R" for i in range(len(labels))))).encode(),
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    for index, label in enumerate(labels):
        content = (f"BT /F1 18 Tf 50 730 Td ({label}) Tj ET\n"
                   "0.92 0.08 0.1 rg 50 560 96 64 re f\n").encode()
        objects.extend([
            (f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {width} {height}] "
             f"/Resources << /Font << /F1 3 0 R >> >> /Contents {5 + 2 * index} 0 R >>").encode(),
            f"<< /Length {len(content)} >>\nstream\n".encode() + content + b"endstream",
        ])
    info = ""
    if metadata:
        objects.append(f"<< /Title ({metadata}) /Author (Fixture Author) >>".encode())
        info = f" /Info {len(objects)} 0 R"
    output = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for number, item in enumerate(objects, 1):
        offsets.append(len(output))
        output.extend(f"{number} 0 obj\n".encode() + item + b"\nendobj\n")
    start = len(output)
    output.extend(f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode())
    for offset in offsets[1:]:
        output.extend(f"{offset:010d} 00000 n \n".encode())
    output.extend((f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R{info} >>\n"
                   f"startxref\n{start}\n%%EOF\n").encode())
    path.write_bytes(output)


def make_epub(path, image, publisher_styles=False):
    inline_style = (' style="color:#000000 !important;background:#ffffff !important"'
                    if publisher_styles else "")
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as book:
        book.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        book.writestr("META-INF/container.xml", """<?xml version="1.0"?>
<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">
<rootfiles><rootfile full-path="OEBPS/book.opf"
media-type="application/oebps-package+xml"/></rootfiles></container>""")
        book.writestr("OEBPS/book.opf", """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="id">urn:nemo:fixture</dc:identifier><dc:title>EPUB Fixture</dc:title>
<dc:creator>Fixture Author</dc:creator><dc:language>en</dc:language></metadata>
<manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
<item id="style" href="style.css" media-type="text/css"/>
<item id="red" href="red.png" media-type="image/png"/>
<item id="toc" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest>
<spine toc="toc"><itemref idref="chapter"/></spine></package>""")
        book.writestr("OEBPS/toc.ncx", """<?xml version="1.0"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
<head/><docTitle><text>EPUB Fixture</text></docTitle><navMap>
<navPoint id="first" playOrder="1"><navLabel><text>First</text></navLabel>
<content src="chapter.xhtml"/></navPoint></navMap></ncx>""")
        book.writestr("OEBPS/chapter.xhtml", f"""<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>EPUB Fixture</title>
<link rel="stylesheet" type="text/css" href="style.css"/></head><body{inline_style}>
<h1{inline_style}>EPUBSentinel</h1><p{inline_style}>Original generated preview fixture.</p>
<div{inline_style}><p{inline_style}>InlineSentinel</p></div>
<img src="red.png" alt="Embedded red rectangle"/></body></html>""")
        stylesheet = "body { color: #111; } img { width: 96px; height: 64px; }"
        if publisher_styles:
            stylesheet += ("html,body,h1,p,div { color: #000 !important; "
                           "background: #fff !important; }")
        book.writestr("OEBPS/style.css", stylesheet)
        book.writestr("OEBPS/red.png", image)


def make_fb2(path, image):
    path.write_text("""<?xml version="1.0" encoding="UTF-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0"
xmlns:l="http://www.w3.org/1999/xlink">
<description><title-info><genre>science</genre>
<author><first-name>Fixture</first-name><last-name>Author</last-name></author>
<book-title>FB2 Fixture</book-title><lang>en</lang></title-info>
<document-info><author><nickname>Nemo Test</nickname></author>
<date>2026-01-01</date><id>nemo-fixture</id><version>1</version></document-info></description>
<body><section><title><p>FB2Sentinel</p></title>
<p>Original generated preview fixture.</p><image l:href="#red"/></section></body>
<binary id="red" content-type="image/png">""" + base64.b64encode(image).decode()
                    + "</binary></FictionBook>", encoding="utf-8")


def make_mobi(path, compression=1, encryption=0, publisher_styles=False):
    text = (b"<html><head><title>MOBI Fixture</title></head><body>"
            b"<h1>MOBISentinel</h1><p>Original generated PalmDOC fixture.</p></body></html>")
    if publisher_styles:
        for tag in (b"body", b"h1", b"p"):
            text = text.replace(b"<" + tag + b">", b"<" + tag
                                + b' style="background:#fff !important;color:#000 !important">')
    payload = text
    if compression == 2:
        payload = b"".join(bytes([len(text[i:i + 8])]) + text[i:i + 8]
                           for i in range(0, len(text), 8))
    mobi = b"MOBI" + struct.pack(">IIIII", 232, 2, 65001, 1, 6) + bytes(208)
    palm = struct.pack(">HHIHHHH", compression, 0, len(text), 1, 4096, encryption, 0)
    header = bytearray(78)
    header[:12] = b"Nemo fixture"
    header[60:68] = b"BOOKMOBI"
    struct.pack_into(">H", header, 76, 2)
    first = 78 + 2 * 8 + 2
    records = struct.pack(">IIII", first, 0, first + len(palm + mobi), 1)
    path.write_bytes(header + records + b"\0\0" + palm + mobi + payload)


class RendererTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixtures = WORK / "fixtures"
        cls.fixtures.mkdir(mode=0o700)
        cls.image = make_png()
        (cls.fixtures / "red.png").write_bytes(cls.image)
        (cls.fixtures / "input.md").write_text(
            "# MarkdownSentinel\n\nOriginal **bold** fixture.\n\n"
            "| Column | Value |\n| --- | --- |\n| TableSentinel | 42 |\n\n"
            "```c\nint example = 42;\n```\n", encoding="utf-8")
        make_epub(cls.fixtures / "input.epub", cls.image)
        make_fb2(cls.fixtures / "input.fb2", cls.image)
        make_mobi(cls.fixtures / "input.mobi")
        make_mobi(cls.fixtures / "palmdoc.mobi", compression=2)
        make_pdf(cls.fixtures / "input.pdf", metadata="PDF Fixture")
        with zipfile.ZipFile(cls.fixtures / "input.cbz", "w") as archive:
            archive.writestr("001.png", cls.image)
            archive.writestr("002.png", make_png(color=(10, 30, 230)))

    def setUp(self):
        self.work = WORK / self.id().rsplit(".", 1)[-1]
        self.work.mkdir(mode=0o700)
        self.sequence = 0

    def run_helper(self, *args, ok=True, env=None):
        command = [str(RENDERER), *(str(arg) for arg in args)]
        result = subprocess.run(command, capture_output=True, timeout=40, env=env)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        else:
            self.assertNotEqual(result.returncode, 0, command)
            self.assertTrue(result.stderr, "Failure must have a diagnostic")
        return result

    def prepare(self, kind, input_path=None, theme=None, **kwargs):
        self.sequence += 1
        pdf = self.work / f"document-{self.sequence}.pdf"
        metadata = self.work / f"document-{self.sequence}.json"
        extension = "md" if kind == "markdown" else kind
        input_path = input_path or self.fixtures / f"input.{extension}"
        args = ["prepare", kind, input_path, pdf, metadata]
        if theme is not None:
            args.extend(theme)
        self.run_helper(*args, **kwargs)
        self.assertFalse(list(self.work.glob(f"{pdf.name}.*")), "Conversion scratch files leaked")
        if not kwargs.get("ok", True):
            self.assertFalse(metadata.exists())
            self.assertFalse(pdf.exists())
            return None
        details = json.loads(metadata.read_text())
        self.assertGreaterEqual(details["page_count"], 1)
        self.assertLessEqual(details["page_count"], 10000)
        self.assertTrue(pdf.read_bytes().startswith(b"%PDF-"))
        for key in ("title", "author"):
            if key in details:
                self.assertIsInstance(details[key], str)
                self.assertLessEqual(len(details[key].encode()), 4096)
        return pdf, details

    def render(self, pdf, page=0, width=600, height=800, query=None, background=None):
        png = self.work / f"page-{self.sequence}.png"
        args = ["render-themed" if background is not None else "render",
                pdf, page, width, height, png]
        if background is not None:
            args.append(background)
        if query is not None:
            args.append(query)
        self.run_helper(*args)
        return read_png(png)

    def search(self, pdf, needle, start=0, direction=1):
        result = self.work / "search.json"
        self.run_helper("search", pdf, needle, start, direction, result)
        return json.loads(result.read_text())

    def assert_visible(self, pixels, image=False):
        colors = Counter(pixels)
        dark = sum(count for color, count in colors.items() if max(color) < 180)
        red = sum(count for (r, g, b), count in colors.items() if r > 170 and g < 90 and b < 90)
        self.assertGreater(dark, 20, "Expected visible text")
        self.assertGreater(colors[(255, 255, 255)], 100, "Expected a white background")
        if image:
            self.assertGreater(red, 400, "Expected the embedded red image")

    def test_markdown_heading_table_and_search(self):
        pdf, _ = self.prepare("markdown")
        width, height, pixels = self.render(pdf)
        self.assertEqual((width, height), (600, 800))
        self.assert_visible(pixels)
        for needle in ("MarkdownSentinel", "TableSentinel"):
            self.assertEqual(self.search(pdf, needle), {"found": True, "page": 0})

    def test_markdown_omits_linked_images_and_raw_html(self):
        source = self.work / "linked.md"
        (self.work / "red.png").write_bytes(self.image)
        source.write_text(
            "# LinkedSentinel\n\n![local](red.png)\n\n"
            f"![absolute]({(self.work / 'red.png').as_uri()})\n\n"
            "![remote](https://example.invalid/red.png)\n\n"
            "<img src=\"red.png\"/>\n\n", encoding="utf-8")
        pdf, _ = self.prepare("markdown", source)
        _, _, pixels = self.render(pdf)
        self.assert_visible(pixels)
        self.assertFalse(any(r > 170 and g < 90 and b < 90 for r, g, b in pixels))
        self.assertEqual(self.search(pdf, "<img"), {"found": True, "page": 0})

    def test_epub_with_embedded_image(self):
        pdf, _ = self.prepare("epub")
        _, _, pixels = self.render(pdf)
        self.assert_visible(pixels, image=True)
        self.assertEqual(self.search(pdf, "EPUBSentinel"), {"found": True, "page": 0})

    def test_fb2_with_embedded_image(self):
        pdf, metadata = self.prepare("fb2")
        self.assertEqual(metadata["page_count"], 1, "No blank page before the first chapter")
        _, _, pixels = self.render(pdf)
        self.assert_visible(pixels, image=True)
        self.assertEqual(self.search(pdf, "FB2Sentinel"), {"found": True, "page": 0})

    def test_fb2_preserves_cover_and_chapter_content(self):
        original = (self.fixtures / "input.fb2").read_text(encoding="utf-8")
        chapters = original.replace(
            "</section></body>",
            "</section><section><title><p>SecondChapterSentinel</p></title>"
            "<p>A second chapter.</p></section></body>")
        for cover in (False, True):
            source = self.work / f"chapters-{cover}.fb2"
            text = chapters
            if cover:
                text = text.replace(
                    "<lang>en</lang>",
                    '<coverpage><image l:href="#red"/></coverpage><lang>en</lang>')
            source.write_text(text, encoding="utf-8")
            for theme in (None, ("#000000", "#00FF00"), ("#F4EEDF", "#24364F")):
                with self.subTest(cover=cover, theme=theme):
                    pdf, metadata = self.prepare("fb2", source, theme=theme)
                    self.assertEqual(self.search(pdf, "FB2Sentinel"),
                                     {"found": True, "page": int(cover)})
                    second = self.search(pdf, "SecondChapterSentinel")
                    self.assertTrue(second["found"])
                    # MuPDF versions paginate short adjacent chapters differently.
                    self.assertIn(second["page"], (int(cover), 1 + int(cover)))
                    self.assertEqual(metadata["page_count"], second["page"] + 1)
                    rendered = self.render(pdf, page=int(cover),
                                           background=theme[0] if theme else None)
                    if theme:
                        self.assert_themed(rendered, theme, image=True)
                    else:
                        self.assert_visible(rendered[2], image=True)
                    if cover:
                        _, _, pixels = self.render(pdf)
                        self.assertGreater(sum(r > 170 and g < 90 and b < 90
                                               for r, g, b in pixels), 400)

    def test_fb2_rejects_malformed_xml(self):
        source = self.work / "invalid.fb2"
        for text in (
                "<html><body>Wrong format</body></html>",
                "<FictionBook><body>Unclosed body</FictionBook>",
                "<FictionBook><description>No body</description></FictionBook>",
                "<FictionBook><body/></FictionBook><FictionBook><body/></FictionBook>",
                "<!DOCTYPE FictionBook><FictionBook><body/></FictionBook>",
                "<FictionBook><body>" + "<section>" * 128 + "Nested"
                + "</section>" * 128 + "</body></FictionBook>"):
            source.write_text(text, encoding="utf-8")
            with self.subTest(text=text[:80]):
                self.prepare("fb2", source, ok=False)

    def test_mobi_uncompressed_and_palmdoc(self):
        for name in ("input.mobi", "palmdoc.mobi"):
            with self.subTest(name=name):
                pdf, _ = self.prepare("mobi", self.fixtures / name)
                _, _, pixels = self.render(pdf)
                self.assert_visible(pixels)
                self.assertEqual(self.search(pdf, "MOBISentinel"), {"found": True, "page": 0})

    def test_pdf_normalization_search_wrap_and_highlight(self):
        pdf, metadata = self.prepare("pdf")
        self.assertEqual(metadata["page_count"], 3)
        self.assertNotEqual(pdf.read_bytes(), (self.fixtures / "input.pdf").read_bytes())
        self.assert_search_and_highlight(pdf)

    def test_existing_pdf_search_wrap_and_highlight(self):
        self.assert_search_and_highlight(self.fixtures / "input.pdf")

    def assert_search_and_highlight(self, pdf):
        for needle, start, direction, expected in (
                ("FirstSentinel", 0, 1, 0), ("MiddleSentinel", 1, -1, 1),
                ("FirstSentinel", 2, 1, 0), ("LastSentinel", 0, -1, 2),
                ("middleSENTINEL", 2, -1, 1)):
            self.assertEqual(self.search(pdf, needle, start, direction),
                             {"found": True, "page": expected})
        for direction in (1, -1):
            self.assertEqual(self.search(pdf, "NotInThisBook", 1, direction), {"found": False})
        width, height, original = self.render(pdf)
        _, _, highlighted = self.render(pdf, query="FirstSentinel")
        self.assertEqual((width, height), (600, 800))
        self.assert_visible(highlighted, image=True)
        changed = [i for i, (a, b) in enumerate(zip(original, highlighted)) if a != b]
        self.assertGreater(len(changed), 100, "Search must highlight visible matches")
        self.assertTrue(all(40 <= i // width <= 85 for i in changed),
                        "Highlight should cover the text, not its vertically mirrored location")
        self.assertTrue(all(max(highlighted[i]) < 80 for i, p in enumerate(original)
                            if max(p) < 80), "Highlight must not obscure dark text")

    def test_cbz_images_and_pages(self):
        pdf, metadata = self.prepare("cbz")
        self.assertEqual(metadata["page_count"], 2)
        width, height, pixels = self.render(pdf)
        self.assertLessEqual(width, 600)
        self.assertLessEqual(height, 800)
        self.assertGreater(sum(r > 170 and g < 90 and b < 90 for r, g, b in pixels), 400)
        _, _, pixels = self.render(pdf, page=1)
        self.assertGreater(sum(b > 170 and r < 90 for r, _, b in pixels), 400)

    def test_render_aspect_fit_and_tiny_dimensions(self):
        pdf = self.fixtures / "input.pdf"
        for requested, expected in (((300, 300), (225, 300)), ((600, 200), (150, 200)),
                                    ((1, 1), (1, 1)), ((240, 320), (240, 320))):
            with self.subTest(requested=requested):
                width, height, _ = self.render(pdf, width=requested[0], height=requested[1])
                self.assertEqual((width, height), expected)
        output = self.work / "maximum.png"
        self.run_helper("render", pdf, 0, 2400, 3200, output)
        self.assertEqual(struct.unpack_from(">II", output.read_bytes(), 16), (2400, 3200))

    def test_invalid_arguments(self):
        pdf = self.fixtures / "input.pdf"
        png = self.work / "invalid.png"
        for page, width, height in (
                ("-1", "600", "800"), ("3", "600", "800"), ("10000", "600", "800"),
                ("0", "0", "800"), ("0", "600", "0"), ("0", "2401", "800"),
                ("0", "600", "3201"), ("+0", "600", "800"), (" 0", "600", "800"),
                ("0", "1.5", "800"), ("0x1", "600", "800"), ("0", "9" * 80, "800"),
                ("0", "600", "nan"), ("", "600", "800")):
            with self.subTest(page=page, width=width, height=height):
                self.run_helper("render", pdf, page, width, height, png, ok=False)
                self.assertFalse(png.exists())
        for needle, start, direction in (
                ("", 0, 1), ("x" * 1025, 0, 1), ("x", -1, 1),
                ("x", 3, 1), ("x", 0, 0), ("x", 0, "+1"), ("x", 0, "1junk")):
            self.run_helper("search", pdf, needle, start, direction,
                            self.work / "invalid.json", ok=False)
        for needle in ("", "x" * 1025):
            self.run_helper("render", pdf, 0, 600, 800, png, needle, ok=False)
        self.run_helper("prepare", "html", self.fixtures / "input.md",
                        self.work / "no.pdf", self.work / "no.json", ok=False)
        self.run_helper("render", ok=False)
        self.run_helper(ok=False)

    def test_invalid_utf8_and_markdown_limits(self):
        source = self.work / "bad.md"
        for text in (b"\xff", b"text\0hidden"):
            source.write_bytes(text)
            self.prepare("markdown", source, ok=False)
        with source.open("wb") as stream:
            stream.truncate(16 * 1024 * 1024 + 1)
        self.prepare("markdown", source, ok=False)
        source.write_bytes(b"&" * (4 * 1024 * 1024))
        result = self.run_helper("prepare", "markdown", source, self.work / "long.pdf",
                                 self.work / "long.json", ok=False)
        self.assertIn(b"HTML", result.stderr)
        pdf = self.fixtures / "input.pdf"
        invalid_query = os.fsdecode(b"\xff")
        self.run_helper("search", pdf, invalid_query, 0, 1,
                        self.work / "invalid.json", ok=False)

    def test_mobi_drm_compression_and_malformed_headers(self):
        source = self.work / "invalid.mobi"
        for encryption in (1, 2, 65535):
            make_mobi(source, encryption=encryption)
            result = self.run_helper("prepare", "mobi", source, self.work / "bad.pdf",
                                     self.work / "bad.json", ok=False)
            self.assertIn(b"DRM", result.stderr)
        for compression in (0, 3, 17480):
            make_mobi(source, compression=compression)
            result = self.run_helper("prepare", "mobi", source, self.work / "bad.pdf",
                                     self.work / "bad.json", ok=False)
            self.assertIn(b"compression", result.stderr)
        good = (self.fixtures / "input.mobi").read_bytes()
        for variant in ("truncated", "past-eof", "inside-table", "descending",
                        "record-count", "text-records", "huge-text", "short-header"):
            data = bytearray(good)
            if variant == "truncated":
                data = data[:30]
            elif variant == "past-eof":
                struct.pack_into(">I", data, 78, len(data) + 1)
            elif variant == "inside-table":
                struct.pack_into(">I", data, 78, 20)
            elif variant == "descending":
                struct.pack_into(">I", data, 86, 96)
            elif variant == "record-count":
                struct.pack_into(">H", data, 76, 65535)
            elif variant == "text-records":
                struct.pack_into(">H", data, 96 + 8, 2)
            elif variant == "huge-text":
                struct.pack_into(">I", data, 96 + 4, 0xffffffff)
            elif variant == "short-header":
                struct.pack_into(">I", data, 96 + 20, 0xffffffff)
            source.write_bytes(data)
            with self.subTest(variant=variant):
                self.prepare("mobi", source, ok=False)
        make_mobi(source, compression=2)
        compressed = source.read_bytes()
        for payload in (compressed[:-5], compressed[:344] + b"\x80\x00"):
            source.write_bytes(payload)
            self.prepare("mobi", source, ok=False)

    def test_malformed_documents_and_oversized_pdf(self):
        for kind in ("epub", "fb2", "pdf", "cbz"):
            source = self.work / f"invalid.{kind}"
            source.write_bytes(b"This is not a document\xff")
            self.prepare(kind, source, ok=False)
        missing = self.work / "missing.pdf"
        self.run_helper("render", missing, 0, 600, 800, self.work / "no.png", ok=False)
        large = self.work / "oversized.pdf"
        with large.open("wb") as stream:
            stream.truncate(128 * 1024 * 1024 + 1)
        self.run_helper("render", large, 0, 600, 800, self.work / "no.png", ok=False)
        self.prepare("pdf", large, ok=False)
        for dimensions in ((20001, 800), (600, 20001), (0.001, 800)):
            make_pdf(large, width=dimensions[0], height=dimensions[1])
            with self.subTest(dimensions=dimensions):
                self.run_helper("render", large, 0, 600, 800, self.work / "no.png", ok=False)
        make_pdf(large, labels=("fixture",) * 10001)
        self.run_helper("render", large, 0, 600, 800, self.work / "no.png", ok=False)
        self.run_helper("search", large, "fixture", 0, 1,
                        self.work / "no.json", ok=False)

    def test_file_types_and_output_aliases(self):
        pdf = self.fixtures / "input.pdf"
        self.run_helper("render", self.work, 0, 600, 800, self.work / "no.png", ok=False)
        link = self.work / "link.pdf"
        link.symlink_to(pdf)
        self.run_helper("render", link, 0, 600, 800, self.work / "no.png", ok=False)
        fifo = self.work / "pipe.pdf"
        os.mkfifo(fifo, 0o600)
        self.run_helper("render", fifo, 0, 600, 800, self.work / "no.png", ok=False)
        original = pdf.read_bytes()
        self.run_helper("render", pdf, 0, 600, 800, pdf, ok=False)
        self.run_helper("search", pdf, "x", 0, 1, pdf, ok=False)
        self.run_helper("prepare", "pdf", pdf, pdf, self.work / "no.json", ok=False)
        self.run_helper("prepare", "pdf", pdf, self.work / "no.pdf", pdf, ok=False)
        self.run_helper("render", pdf, 0, 600, 800, self.work / "missing" / "no.png", ok=False)
        self.assertEqual(pdf.read_bytes(), original)

    def test_converter_failure_and_invalid_success_output(self):
        stub_dir = self.work / "bin"
        stub_dir.mkdir(mode=0o700)
        converter = stub_dir / "mutool"
        env = os.environ.copy()
        env["PATH"] = str(stub_dir)
        for body in (
                "sys.exit(37)",
                "sys.exit(0)",
                "os.unlink(sys.argv[sys.argv.index('-o') + 1])",
                "open(sys.argv[sys.argv.index('-o') + 1], 'w').write('Not a PDF')",
                "f = open(sys.argv[sys.argv.index('-o') + 1], 'wb'); "
                "f.truncate(128 * 1024 * 1024 + 1); f.close()"):
            converter.write_text(f"#!{sys.executable}\nimport os, sys\n{body}\n", encoding="utf-8")
            converter.chmod(0o700)
            with self.subTest(body=body):
                self.prepare("pdf", ok=False, env=env)
        converter.unlink()
        self.prepare("pdf", ok=False, env=env)

    def test_metadata_bounds_and_preserving_outputs_on_converter_failure(self):
        source = self.work / "metadata.pdf"
        make_pdf(source, metadata="T" * 8192)
        stub_dir = self.work / "bin"
        stub_dir.mkdir(mode=0o700)
        converter = stub_dir / "mutool"
        converter.write_text(
            f"#!{sys.executable}\nimport shutil, sys\n"
            "shutil.copyfile(sys.argv[-1], sys.argv[sys.argv.index('-o') + 1])\n",
            encoding="utf-8")
        converter.chmod(0o700)
        env = os.environ.copy()
        env["PATH"] = str(stub_dir)
        pdf, metadata = self.prepare("pdf", source, env=env)
        self.assertEqual(metadata["title"], "T" * 4096)
        self.assertEqual(metadata["author"], "Fixture Author")
        metadata_path = pdf.with_suffix(".json")
        old_pdf, old_metadata = pdf.read_bytes(), metadata_path.read_bytes()
        converter.write_text(f"#!{sys.executable}\nimport sys\nsys.exit(37)\n", encoding="utf-8")
        self.run_helper("prepare", "pdf", source, pdf, metadata_path, ok=False, env=env)
        self.assertEqual(pdf.read_bytes(), old_pdf)
        self.assertEqual(metadata_path.read_bytes(), old_metadata)

    def assert_themed(self, rendered, theme, image=False):
        width, height, pixels = rendered
        background, foreground = (tuple(bytes.fromhex(value[1:])) for value in theme)
        colors = Counter(pixels)
        self.assertGreater(colors[background], width * height * 0.8,
                           "Theme background must cover the whole page")
        self.assertGreater(colors[foreground], 100, "Expected readable theme-colored text")
        edges = (pixels[:width] + pixels[-width:] + pixels[::width]
                 + pixels[width - 1::width])
        self.assertTrue(all(pixel == background for pixel in edges),
                        "Margins must use the theme background")
        self.assertEqual(colors[(255, 255, 255)], 0, "Publisher/code white backgrounds leaked")
        if image:
            self.assertGreater(sum(count for (r, g, b), count in colors.items()
                                   if r > 170 and g < 90 and b < 90), 400,
                               "The embedded image must retain its original red pixels")

    def test_theme_markdown_dark_and_light(self):
        for theme in (("#000000", "#00FF00"), ("#F4EeDF", "#24364f")):
            with self.subTest(theme=theme):
                pdf, _ = self.prepare("markdown", theme=theme)
                self.assert_themed(self.render(pdf, background=theme[0]), theme)
                self.assertEqual(self.search(pdf, "TableSentinel"), {"found": True, "page": 0})

    def test_theme_epub_overrides_publisher_and_inline_css(self):
        source = self.work / "publisher.epub"
        make_epub(source, self.image, publisher_styles=True)
        for theme in (("#000000", "#00FF00"), ("#F4EEDF", "#24364F")):
            with self.subTest(theme=theme):
                pdf, _ = self.prepare("epub", source, theme=theme)
                self.assert_themed(self.render(pdf, background=theme[0]), theme, image=True)
                self.assertEqual(self.search(pdf, "InlineSentinel"), {"found": True, "page": 0})

    def test_theme_fb2_and_both_mobi_compressions(self):
        for compression in (1, 2):
            make_mobi(self.work / f"publisher-{compression}.mobi",
                      compression=compression, publisher_styles=True)
        cases = (("fb2", self.fixtures / "input.fb2", "FB2Sentinel"),
                 ("mobi", self.work / "publisher-1.mobi", "MOBISentinel"),
                 ("mobi", self.work / "publisher-2.mobi", "MOBISentinel"))
        for kind, source, needle in cases:
            for theme in (("#000000", "#00FF00"), ("#F4EEDF", "#24364F")):
                with self.subTest(kind=kind, source=source.name, theme=theme):
                    pdf, _ = self.prepare(kind, source, theme=theme)
                    self.assert_themed(self.render(pdf, background=theme[0]), theme,
                                       image=kind == "fb2")
                    self.assertEqual(self.search(pdf, needle), {"found": True, "page": 0})

    def test_theme_highlight_stays_visible_and_preserves_text(self):
        theme = ("#000000", "#00FF00")
        pdf, _ = self.prepare("markdown", theme=theme)
        width, height, original = self.render(pdf, background=theme[0])
        _, _, highlighted = self.render(pdf, background=theme[0], query="MarkdownSentinel")
        self.assertGreater(sum(a != b for a, b in zip(original, highlighted)), 100)
        self.assertTrue(all(highlighted[i][1] == 255 for i, pixel in enumerate(original)
                            if pixel == (0, 255, 0)), "Bright text must remain readable")
        self.assertEqual((width, height), (600, 800))
        _, _, not_found = self.render(pdf, background=theme[0], query="MissingSentinel")
        self.assertEqual(not_found, original)

    def test_theme_strict_colors_args_and_fixed_layout_rejection(self):
        source = self.fixtures / "input.md"
        pdf, metadata, png = (self.work / name for name in ("bad.pdf", "bad.json", "bad.png"))
        invalid_colors = ("", "red", "#fff", "112233", "#00112233", "#GG1122", " #abcdef",
                          "#123456 ", "#12345\n", "#１２３４５６", "#000000;body{}",
                          "rgba(0,0,0,1)")
        for color in invalid_colors:
            with self.subTest(color=color):
                for theme in ((color, "#ffffff"), ("#000000", color)):
                    self.run_helper("prepare", "markdown", source, pdf, metadata, *theme, ok=False)
                self.run_helper("render-themed", self.fixtures / "input.pdf",
                                0, 600, 800, png, color, ok=False)
        for kind in ("pdf", "cbz"):
            self.prepare(kind, theme=("#000000", "#ffffff"), ok=False)
        for extra in (("#000000",), ("#000000", "#ffffff", "extra")):
            self.run_helper("prepare", "markdown", source, pdf, metadata, *extra, ok=False)
        for extra in ((), ("#000000", "query", "extra"), ("#000000", ""),
                      ("#000000", "x" * 1025)):
            self.run_helper("render-themed", self.fixtures / "input.pdf",
                            0, 600, 800, png, *extra, ok=False)
        for page, width, height in ((-1, 600, 800), (3, 600, 800), (0, 0, 800),
                                   (0, 2401, 800), (0, 600, 3201)):
            self.run_helper("render-themed", self.fixtures / "input.pdf",
                            page, width, height, png, "#000000", ok=False)
        drm = self.work / "drm.mobi"
        make_mobi(drm, encryption=2)
        self.prepare("mobi", drm, theme=("#000000", "#ffffff"), ok=False)
        self.assertFalse(pdf.exists())
        self.assertFalse(metadata.exists())
        self.assertFalse(png.exists())

    def test_theme_converter_failure_cleans_stylesheet(self):
        stub_dir = self.work / "bin"
        stub_dir.mkdir(mode=0o700)
        converter = stub_dir / "mutool"
        converter.write_text(
            f"#!{sys.executable}\nimport pathlib, sys\n"
            "style = pathlib.Path(sys.argv[sys.argv.index('-U') + 1])\n"
            "assert style.is_file()\n"
            "assert '#00FF00' in style.read_text()\n"
            "sys.exit(37)\n", encoding="utf-8")
        converter.chmod(0o700)
        env = os.environ.copy()
        env["PATH"] = str(stub_dir)
        self.prepare("markdown", theme=("#000000", "#00FF00"), ok=False, env=env)
        self.assertFalse(list(self.work.glob("*.css")))

    def test_render_theme_fills_canvas_without_recoloring_artwork(self):
        pdf = self.fixtures / "input.pdf"
        _, _, original = self.render(pdf)
        width, height, themed = self.render(pdf, background="#CcDdEE")
        self.assertEqual(themed[0], (204, 221, 238))
        self.assertEqual(themed[-1], (204, 221, 238))
        original_red = Counter(pixel for pixel in original
                               if pixel[0] > 170 and pixel[1] < 90 and pixel[2] < 90)
        themed_red = Counter(pixel for pixel in themed
                             if pixel[0] > 170 and pixel[1] < 90 and pixel[2] < 90)
        self.assertGreater(sum(original_red.values()), 400)
        self.assertEqual(original_red, themed_red)
        self.assertEqual((width, height), (600, 800))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("Usage: test-document-renderer.py /path/to/nemo-document-renderer [unittest options]")
    RENDERER = Path(sys.argv.pop(1)).resolve()
    if not RENDERER.is_file():
        sys.exit(f"Renderer does not exist: {RENDERER}")
    if shutil.which("mutool") is None:
        sys.exit("mutool is required: install the official mupdf-tools package before running these tests")
    # Never use the global temporary directory or touch a user's desktop/files.
    root = Path.cwd() / "build" / "native-doc-helper"
    root.mkdir(parents=True, exist_ok=True)
    WORK = root / f"fixtures-{uuid.uuid4().hex}"
    WORK.mkdir(mode=0o700)
    try:
        unittest.main(verbosity=2)
    finally:
        shutil.rmtree(WORK)
