#!/usr/bin/env python3
"""Ensure every literal symbolic UI icon and core file icon has embedded artwork."""

from pathlib import Path
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
resource = ET.parse(ROOT / "gresources/nemo-default-icons.gresource.xml")
names = {Path(entry.attrib["alias"]).stem for entry in resource.iter("file")}
requested = set()
for directory in ("src", "libnemo-private", "gresources"):
    for source in (ROOT / directory).rglob("*"):
        if source.suffix not in (".c", ".h", ".xml", ".ui", ".glade"):
            continue
        text = source.read_text()
        requested.update(re.findall(r'"((?:xsi|xapp)-[a-z0-9-]+)"', text))
        requested.update(re.findall(r'"([a-z][a-z0-9-]+-symbolic(?:-(?:ltr|rtl))?)"', text))
        if source.suffix in (".ui", ".glade"):
            xml = ET.fromstring(text)
            requested.update(
                entry.text for entry in xml.iter("property")
                if entry.attrib.get("name") in ("icon_name", "icon-name") and entry.text
            )
requested.update(re.findall(
    r'#define\s+NEMO_ICON_\w+\s+"([^"]+)"',
    (ROOT / "libnemo-private/nemo-icon-names.h").read_text(),
))
missing = sorted(name for name in requested if not name.endswith("-") and name not in names)
assert not missing, "Missing embedded UI icons: " + ", ".join(missing)
for entry in resource.iter("file"):
    assert (ROOT / "gresources" / entry.text).is_file(), entry.text
print(f"{len(requested)} UI icon references covered; {len(names)} embedded names")
