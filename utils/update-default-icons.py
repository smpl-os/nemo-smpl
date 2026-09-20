#!/usr/bin/env python3
"""Vendor installed XSI/Adwaita artwork and regenerate Nemo's offline resource.

Maintainer-only: builds and installed Nemo never read the host's icon packages.
Run under test/run-isolated-regression.py to avoid the desktop's settings.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import xml.etree.ElementTree as ET

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk


ROOT = Path(__file__).resolve().parent.parent
DEST = ROOT / "gresources/default-icons"
XSI = Path("/usr/share/icons/hicolor/scalable/actions")
ADWAITA = Path("/usr/share/icons/Adwaita/scalable")

REGULAR_NAMES = {
    "application-x-executable", "application-x-generic", "application-octet-stream",
    "audio-x-generic", "image-x-generic", "video-x-generic", "text-html",
    "text-x-generic", "text-x-generic-template", "text-x-preview", "text-x-script",
    "image-loading", "image-missing", "folder-drag-accept", "folder-open",
    "folder-visiting", "emblem-symbolic-link", "starred", "non-starred",
    "user-bookmarks", "preferences-desktop", "media-removable",
}
REGULAR_FALLBACKS = {
    "application-x-generic": "application-x-executable",
    "application-octet-stream": "application-x-executable",
    "text-x-generic-template": "text-x-generic",
    "text-x-preview": "text-x-generic",
    "image-loading": "image-x-generic",
    "folder-drag-accept": "folder-open",
    "folder-visiting": "folder-open",
    "nemo-cd-burner": "media-optical",
}
ALIASES = {
    "xapp-favorite": "starred",
    "xapp-favorite-available": "non-starred",
    "xapp-user-favorites": "user-bookmarks",
    "smartphone-symbolic": "phone-symbolic",
    "camera-photo-symbolic": "camera-symbolic",
    "xsi-camera-photo-symbolic": "xsi-camera-symbolic",
    "xapp-prefs-behavior-symbolic": "preferences-symbolic",
    "xapp-prefs-display-symbolic": "display-symbolic",
    "xapp-prefs-plugins-symbolic": "addon-symbolic",
    "xapp-prefs-preview-symbolic": "preview-symbolic",
    "xapp-prefs-toolbar-symbolic": "toolbar-symbolic",
}


def main():
    if os.environ.get("NEMO_TEST_ISOLATED") != "1":
        raise SystemExit("Run through test/run-isolated-regression.py")
    Gtk.init([])
    theme = Gtk.IconTheme.new()
    theme.set_custom_theme("Adwaita")
    aliases = {}
    origins = {}

    def vendor(source, group, name=None):
        source = Path(source)
        name = name or source.stem
        relative = Path("icons") / group / (name + source.suffix)
        target = DEST / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target, follow_symlinks=True)
        origins[str(relative)] = {
            "source": str(source),
            "sha256": hashlib.sha256(target.read_bytes()).hexdigest(),
        }
        aliases[name] = str(relative)
        return str(relative)

    symbolic = sorted(XSI.glob("xsi-*.svg"))
    if not symbolic:
        raise RuntimeError("Install xapp-symbolic-icons before updating the vendored artwork")
    for source in symbolic:
        relative = vendor(source, "symbolic")
        aliases[source.stem.removeprefix("xsi-")] = relative

    for source in sorted(ADWAITA.rglob("*.svg")):
        vendor(source, "regular")

    for source in sorted((ROOT / "data/icons/hicolor").glob("*/scalable/*.svg")):
        relative = "../" + str(source.relative_to(ROOT))
        aliases[source.stem] = relative
        origins[relative] = {
            "source": "repo:" + str(source.relative_to(ROOT)),
            "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        }

    header = (ROOT / "libnemo-private/nemo-icon-names.h").read_text()
    regular_names = REGULAR_NAMES | {
        name for name in re.findall(r'#define\s+NEMO_ICON_\w+\s+"([^"]+)"', header)
        if not name.startswith(("xsi-", "xapp-"))
    }
    for name in sorted(regular_names):
        if name in aliases:
            continue
        candidate = REGULAR_FALLBACKS.get(name, name)
        info = theme.lookup_icon(candidate, 64, Gtk.IconLookupFlags.FORCE_REGULAR)
        if info is None:
            raise RuntimeError(f"No regular fallback for {name!r} (tried {candidate!r})")
        filename = info.get_filename()
        if filename is None or not filename.startswith(
            ("/usr/share/icons/Adwaita/", "/usr/share/icons/AdwaitaLegacy/")
        ):
            raise RuntimeError(f"Unexpected artwork origin for {name!r}: {filename!r}")
        vendor(filename, "regular", name)

    for alias, target in ALIASES.items():
        aliases[alias] = aliases[target]

    requested = set()
    for directory in ("src", "libnemo-private", "gresources"):
        for source in (ROOT / directory).rglob("*"):
            if source.suffix in (".c", ".h", ".xml", ".ui", ".glade"):
                requested.update(re.findall(r'"((?:xsi|xapp)-[a-z0-9-]+)"', source.read_text()))
    missing = sorted(name for name in requested if not name.endswith("-") and name not in aliases)
    if missing:
        raise RuntimeError("Missing UI fallbacks: " + ", ".join(missing))

    resource = ET.Element("gresources")
    group = ET.SubElement(resource, "gresource", prefix="/org/nemo/default-icons")
    for name, relative in sorted(aliases.items()):
        suffix = Path(relative).suffix
        entry = ET.SubElement(group, "file", alias=name + suffix, compressed="true")
        entry.text = relative if relative.startswith("../") else "default-icons/" + relative
    ET.indent(resource, space="  ")
    ET.ElementTree(resource).write(
        ROOT / "gresources/nemo-default-icons.gresource.xml",
        encoding="utf-8",
        xml_declaration=True,
    )
    (DEST / "origins.json").write_text(json.dumps(origins, indent=2, sort_keys=True) + "\n")
    print(f"Catalogued {len(origins)} source files, providing {len(aliases)} icon names")


if __name__ == "__main__":
    main()
