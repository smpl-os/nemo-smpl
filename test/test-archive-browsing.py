#!/usr/bin/env python3
"""Generate real archives and exercise Nemo's read-only archive GFile backend."""

import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import wave
import zipfile
import zlib


def main():
    if os.environ.get("NEMO_TEST_ISOLATED") != "1":
        raise RuntimeError("Archive regressions require run-isolated-regression.py")
    bsdtar = shutil.which("bsdtar")
    if bsdtar is None:
        raise RuntimeError("Archive regressions require libarchive-tools (bsdtar)")
    root = Path(os.environ["NEMO_TEST_PROFILE"]) / "archives"
    source = root / "source"
    (source / "nested" / "deeper").mkdir(parents=True)
    text = "".join(f"archive-regression-text line {i:04d}\n" for i in range(300))
    (source / "text.txt").write_text(text)
    (source / "binary.dat").write_bytes(
        bytes(range(256)) * 64 + b"archive-regression-binary"
    )
    (source / "nested" / "deeper" / "payload.txt").write_text("nested archive payload\n")
    (source / "nested" / "deeper" / "uri # % café.txt").write_text("escaped archive member\n")
    (source / "nested" / "empty").mkdir()
    subprocess.run(
        [bsdtar, "--format", "7zip", "-cf", str(source / "0-inner # %.7z"),
         "-C", str(source), "text.txt", "binary.dat", "nested"],
        check=True,
    )
    members = sorted(source.rglob("*"))
    # Spaces, Unicode and URI metacharacters exercise both URI escaping layers.
    prefix = "fixture # % café"
    archives = []
    for suffix, mode in ((".tar", "w"), (".tar.gz", "w:gz"), (".tar.xz", "w:xz")):
        archive = root / (prefix + suffix)
        with tarfile.open(archive, mode) as writer:
            for member in members:
                writer.add(member, arcname=str(member.relative_to(source)), recursive=False)
        archives.append(archive)
    archive = root / (prefix + ".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as writer:
        for member in members:
            writer.write(member, str(member.relative_to(source)))
    archives.insert(0, archive)
    for suffix, format_name in ((".7z", "7zip"), (".iso", "iso9660")):
        archive = root / (prefix + suffix)
        subprocess.run(
            [bsdtar, "--format", format_name, "-cf", str(archive),
             "-C", str(source), "text.txt", "binary.dat", "nested", "0-inner # %.7z"],
            check=True,
        )
        archives.append(archive)
    corrupt = root / "corrupt.zip"
    corrupt.write_bytes(b"PK\x03\x04broken archive central directory")
    encrypted = root / "encrypted.zip"
    subprocess.run(
        [bsdtar, "--format", "zip", "--options", "zip:encryption=aes256",
         "--passphrase", "regression-only", "-cf", str(encrypted),
         "-C", str(source), "text.txt"],
        check=True,
    )
    archives.extend((corrupt, encrypted))
    media = root / "media"
    media.mkdir()

    def png_chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))

    pixels = (b"\0" + bytes((0x12, 0xAB, 0x34)) * 16) * 16
    (media / "image.png").write_bytes(
        b"\x89PNG\r\n\x1a\n" +
        png_chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 2, 0, 0, 0)) +
        png_chunk(b"IDAT", zlib.compress(pixels)) + png_chunk(b"IEND", b"")
    )
    with wave.open(str(media / "tone.wav"), "wb") as writer:
        writer.setparams((1, 2, 8000, 16000, "NONE", "not compressed"))
        writer.writeframes(b"".join(struct.pack("<h", (i % 80 - 40) * 500)
                                   for i in range(16000)))
    media_archive = root / "media # %.zip"
    with zipfile.ZipFile(media_archive, "w", zipfile.ZIP_DEFLATED) as writer:
        for member in sorted(media.iterdir()):
            writer.write(member, member.name)
    archives.append(media_archive)
    before = {p: (hashlib.sha256(p.read_bytes()).digest(), p.stat().st_mtime_ns)
              for p in archives}
    env = os.environ.copy()
    env["NEMO_TEST_ARCHIVES"] = str(root)
    # The isolated profile may be tmpfs, deliberately unsupported as a durable
    # Nemo transfer destination. Match the existing copy tests' build filesystem.
    with tempfile.TemporaryDirectory(prefix=".archive-copy-", dir=Path.cwd()) as copy_root:
        env["NEMO_TEST_ARCHIVE_COPY_ROOT"] = copy_root
        result = subprocess.run([sys.argv[1], *sys.argv[2:]], env=env, check=False)
    for archive, original in before.items():
        assert (hashlib.sha256(archive.read_bytes()).digest(),
                archive.stat().st_mtime_ns) == original, f"Archive modified: {archive}"
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
