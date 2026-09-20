# Embedded default icons

These unmodified flat icons are compiled into Nemo's resource bundle. GTK uses
them as **ultimate fallbacks**: a usable icon supplied by the selected theme
still wins. They also work with no installed icon theme or usable `hicolor`
index, without creating symlinks or changing the user's theme preference.

The complete XSI symbolic set includes device names supplied dynamically by
GIO, not only the names written literally in Nemo's source. Standard symbolic
names are aliases of the same artwork. Adwaita's regular artwork supplies
recognizable folders, file types and places where Nemo needs full-color icons.

## Artwork and attribution

- `icons/symbolic/`: XApp Symbolic Icons **1.1.0**, from
  <https://github.com/xapp-project/xapp-symbolic-icons/tree/1.1.0>.
  Original filenames are preserved; symlinks are materialized.
  The upstream per-file copyright/attribution list and all referenced licenses
  are in `licenses/xsi/`. The set includes LGPL-3.0-only, GPL-3.0-only,
  CC-BY-SA-4.0, CC-BY-4.0, CC0-1.0, MIT and BSD-3-Clause artwork.
- `icons/regular/`: Adwaita Icon Theme **50.0** and **46.2** (AdwaitaLegacy),
  from <https://github.com/GNOME/adwaita-icon-theme>.
  Their authors and copyright/license notices are in `licenses/adwaita-50/`
  and `licenses/adwaita-46/`. Adwaita offers its artwork under
  CC-BY-SA-3.0 or LGPL-3.0; the CC-BY-SA-3.0 option is used here.

Nemo's own scalable application, sidebar, layout and progress icons are included
directly from `data/icons/`, under their existing repository license.

`origins.json` records the original installed or repository path and SHA-256 of
each unmodified source asset. Resource aliases only change lookup names, not artwork.
The notices are also installed with Nemo.

## Updating

With the corresponding XSI and Adwaita icon packages available, run from the
repository root:

```sh
python3 test/run-isolated-regression.py python3 utils/update-default-icons.py
```

Update the notices/provenance when changing upstream versions. The generator
updates `../nemo-default-icons.gresource.xml` and checks literal XSI/XApp UI
coverage. Normal builds require no installed icon packages and no network access.
