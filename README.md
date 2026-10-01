nemo-smpl
=========

[![Packages](https://github.com/smpl-os/nemo-smpl/actions/workflows/build-arch.yml/badge.svg?branch=main)](https://github.com/smpl-os/nemo-smpl/actions/workflows/build-arch.yml)

**nemo-smpl** is an enhanced fork of [Nemo](https://github.com/linuxmint/nemo), the file manager for the Cinnamon desktop environment. This fork is maintained for **smplOS** and ships features that upstream considers out-of-scope.

## Highlights

| Feature | Stock Nemo | nemo-smpl |
|---------|:----------:|:---------:|
| F3 Quick Preview (documents/text/image/media/hex/dir) | ❌ | ✅ |
| Native Markdown, EPUB, FB2 and basic MOBI previews | ❌ | ✅ |
| Camera RAW preview (DNG, ARW, CR2, NEF…) | ❌ | ✅ |
| Wayland video preview (sidebar + F3) | ❌ | ✅ |
| Timecode display (hh:mm:ss:ff) in F3 preview | ❌ | ✅ |
| Frame-by-frame stepping (< >) in F3 preview | ❌ | ✅ |
| Media keyboard shortcuts (play/mute) | ❌ | ✅ |
| Copy verification by default (SHA-256) | ❌ | ✅ |
| Preview Pane (Alt+F3) with GPS map | ❌ | ✅ |
| Disk Usage Overview (Pareto charts) | ❌ | ✅ |
| Archive browsing (ZIP/7z/TAR/ISO as folders) | ❌ | ✅ |
| Configurable keyboard shortcuts | ❌ | ✅ |
| Theme-aware filename colors and outline icons | ❌ | ✅ |
| Substring search | ❌ | ✅ |
| MTP device support (hardened) | ⚠️ | ✅ |
| smplOS live theming | ❌ | ✅ |
| Cover art directory icons | ❌ | ✅ |
| Per-pane location labels | ❌ | ✅ |

See [FEATURES.md](FEATURES.md) for full details and release notes.

Folder sizes in preview details are calculated recursively in the background
after a short selection delay, rather than showing the directory entry's own
size. The field shows **Calculating…** until ready, using the same totals as
Properties (including hidden contents, without following nested symbolic links
or descending into other filesystems). Moving away or hiding the details cancels
the request. Unreadable contents are reported as an incomplete total.

## Installation

### Arch Linux / smplOS

```bash
sudo pacman -S nemo-smpl
```

### Debian / Ubuntu

Download the `.deb` from [GitHub Releases](https://github.com/KonTy/nemo/releases):

```bash
sudo dpkg -i nemo_*.deb
sudo apt-get install -f
```

### Building from Source

```bash
git clone https://github.com/KonTy/nemo.git
cd nemo
meson setup build --prefix=/usr -Dtracker=false
ninja -C build
sudo ninja -C build install
```

See [INSTALLATION.md](INSTALLATION.md) for detailed instructions, build dependencies, and MTP setup.

Use normal patch-version bumps, not package-only test suffixes. Generate local
package versions from the build metadata rather than maintaining another version value.
About and `nemo --version` read the version of the installed package owning the
running executable (pacman, dpkg or RPM), including its package revision.
Unpackaged source builds are labelled explicitly; upgrading a running binary
adds a restart reminder instead of silently presenting it as the new code.

## File Transfer Safety

Hardened builds (`smpl_features=true`, the default) prioritize retaining data over
completing an unsafe operation. Copy verification is enabled by default across
copy entry points. Local transfers use checked synchronization and atomic
publication where supported; copied moves verify the destination before
reclaiming source space. Same-filesystem moves retain a synchronized rename path
and are not described as checksum-verified copies.

Unsupported destinations are refused rather than silently receiving weaker
protection. This includes remote destinations and unqualified network/FUSE
mounts. Remote sources can be **copied** to supported local storage; destructive
moves from remote sources are refused. Replacements retain the previous
destination in recovery storage, which can consume space until explicitly
removed. Read the operation's result for recovery locations and incomplete work.

**Transfer completion is not permission to unplug a device.** Wait for successful
completion, eject it, and wait for removal to finish. Keep files unchanged during
transfers and keep an independent backup of irreplaceable data. Checksums and
filesystem flushes cannot guarantee honest hardware, eliminate every concurrent
writer race, or certify that a remote server committed data to physical media.

See [transfer behavior and recovery limits](FEATURES.md#verify-after-copymove).

File Operations shows the transfer result and SHA-256 verification summary.
Use **Dock** to attach it to a resizable area at the bottom of Nemo, or **Undock**
to return it to a separate window. Nemo remembers that choice. Closing the
hosting Nemo window leaves transfers running in the background; reopening a
Nemo window restores their panel.

Technical notes and recovery locations are under **Details**, expanded
automatically for incomplete operations. **Enter**, **Escape**, or **Close**
dismisses the transfer UI without cancelling active transfers. In a docked
panel, these keys apply while a transfer control has focus.

To avoid successful-completion summaries, disable **Show completion results for
successful copies and moves** in **Preferences → Behavior**, or check **Don't
show successful transfers again** in a completion summary. Both change the same
setting. Errors, incomplete/cancelled transfers and unverified retained files
are still reported; verification itself is unchanged.

## Document Previews

F3 and the right preview pane share a native document viewer for **Markdown,
EPUB, plain UTF-8 FB2, basic MOBI, PDF and CBZ**. It provides document paging, zoom,
and F3 text search. Loading a sidebar preview never takes keyboard focus from
the file list: **Up/Down** continue selecting files. Use the scrollbar or
**Alt+PgUp/Alt+PgDn** to scroll a text/document preview without leaving the file
list; these bindings are configurable under **Keyboard Shortcuts → Preview**.
**PgUp/PgDn scroll the open document** in F3 or when the sidebar preview has
keyboard focus.
In F3, **Ctrl+Left / Ctrl+Right** switch files; both are configurable under
**Preferences → Keyboard Shortcuts → Quick Preview**. The file-navigation
button tooltips follow those settings. Plain Left/Right still seek in media.

Unrecognized regular files use the same paged **hex preview** in the sidebar as
in F3, without taking focus from the file list. Directories and special files
keep their information view rather than being read as byte streams.

Rendering uses open-source **md4c**, the separate stock **MuPDF `mutool`**
program, and **Poppler/Cairo**—not an embedded browser. Conversion/rendering
runs in a restricted Bubblewrap process with no network or access to the
original folder/home directory. The selected input is staged privately, with
size, memory, CPU and time limits. Closing or switching the preview cancels
the work. An unavailable sandbox or unsupported document produces an error;
Nemo does not fall back to running the converter unsandboxed.

There is **no DRM support**. MOBI support is limited to unencrypted,
uncompressed/PalmDOC books; HUFF/CDIC, KF8/AZW3, KFX and other unsupported
variants are not promised. Markdown uses safe HTML conversion and does not
load linked external images; EPUB/FB2 embedded images are supported. This is
a preview, not a full ebook editor or reader. FB2 files with DTDs or non-UTF-8
encoding are rejected explicitly.
FB2 previews preserve covers and chapter content without inserting an empty
opening page on older MuPDF releases.

Reflowable books (Markdown, EPUB, FB2 and supported MOBI) follow the current
theme's background and text colors, including theme changes while open.
PDFs and CBZ/comics keep their original page appearance. Embedded images are
not color-inverted. Reflowable books use controlled reader styles so publisher
CSS cannot force white pages; structural headings, lists, tables and emphasis
remain, but publisher-specific styling may be replaced.

Nemo remembers where you stopped reading. When a document is closed or
another file is previewed, its page and scroll position are saved, and the
next preview of that document reopens there—in both F3 and the preview pane.
Positions are stored per file in `~/.config/nemo/document-positions` and are
discarded automatically when the document changes on disk. Turn this off with
**Preferences → Preview → Remember the reading position in document previews**.

The divider between file panes—and the right preview pane's resize handle—is
a theme-colored **1-pixel line** that highlights on hover. Its invisible drag
target stays wider so it is still easy to resize.

## Browsing Archives and ISO Images

Press **Enter** or double-click a ZIP, 7z, TAR/compressed TAR, RAR or ISO image to
browse it like a folder. Open subfolders, preview individual files with **F3**
or the sidebar, and use **Copy/F5** or copy-and-paste to extract selected files
and folders to supported local storage. **Up/Backspace** from the archive root
returns to its containing folder and selects the archive.

List, icon and compact panes always show **`..` first** when the location has
an Up destination, including empty folders and nested archive roots. Enter or
double-click uses the same Up navigation. Sorting, reverse order and filename
filters do not hide or reorder it. It is navigation only: Select All, file
counts, clipboard, dragging, archive creation and file operations exclude it.
It is absent at the filesystem root, on the desktop, and in search, Recent and
Favorites views; search still exits using the existing Up/Backspace command.

Browsing is **read-only**: it does not add, modify or delete archive contents,
and does not extract the entire archive to a temporary directory. A native
virtual folder reads selected members on demand using libarchive, including
7z archives that require seeking. A dedicated libisofs reader preserves ISO
Rock Ridge paths, including relocated deep directories. Neither needs FUSE
mounts or a GVfs archive backend. Damaged archives and unsupported
formats/password protection produce an error. ISO support does not include
encrypted discs, UDF-only images, or compressed zisofs members.
**Open With** remains available for an external archive manager.

**Alt+F5** packs the selected files and folders into a new **7z** archive using
maximum LZMA2 compression. **Alt+Shift+F5** packs them and removes the originals
only after the archive has been completed and verified. The save dialog starts
in the other pane's folder when split view is open, otherwise in the current
folder; the destination and archive name can be changed before starting.
Both shortcuts are configurable under **Keyboard Shortcuts → File Operations**.
Selections must share a source folder, and names must be representable as UTF-8.
Selections containing multiple hard links to the same file are rejected.
Moves require supported local source storage; read-only archive members can be
copied into a new archive, but cannot be moved out by this command.

Selected folder names, their contents (including hidden files and empty folders),
and modification times are retained. Original ownership, permissions, ACLs and
extended attributes are not preserved. Maximum compression can require substantial
CPU, memory and temporary disk space. Compression and verification run in the
background, with progress and cancellation in File Operations.
Temporary compressed data is stored alongside the destination, so leave room
for both it and the finished archive.

Directory metadata is cached in a private temporary SQLite index, with a bounded
in-memory page cache. Listings create file-information objects only as requested,
and subsequent lookups reuse the index while the archive is unchanged. Opening
an archive no longer requires holding every member's full metadata in memory,
and there is no 100,000-entry archive limit. The first opening still scans archive
headers; compressed TAR and solid formats can require more work than ZIP.

Archive links cannot be read or followed. At most 16 nested archive levels are
supported, and unsafe or excessive member paths are rejected.
Compressed members support seeking by replaying decompression when necessary;
seeking in large or solid archives can therefore be slower than in local files.
With libarchive older than 3.7.5, opening later 7z members also replays preceding
members to avoid a solid-archive skipping bug; directory indexing remains lazy.

## Zen Mode

**Alt+Z** toggles Zen mode for the current window: only the file panes and their
breadcrumb/address bars remain. Menu and toolbar buttons, sidebar, tab strips,
status bar, right preview and docked transfer display are hidden. **Ctrl+L**
still edits the location, and pane/tab navigation shortcuts keep working.

Toggle Alt+Z again to restore the normal layout without changing your saved
visibility preferences. Transfers keep running; safety prompts and elevated-
privilege warnings remain available. Zen mode is per-window and is not saved
as a startup preference.

Change its shortcut under **Preferences → Keyboard Shortcuts → Window**.
**View → Zen Mode** and the file pane's background context menu also toggle it,
so the context menu can exit Zen mode even with its shortcut disabled.

## File Appearance

**Preferences → Views** has two independently configurable options, enabled by
default: **Use theme-aware file-type colors** and **Use outline-style file and
folder icons**. Filename colors distinguish folders, images, video, audio,
archives, documents, source/configuration files and scripts/programs in list,
icon and compact views. Colors adapt to the view background for readability.
On dark backgrounds, blue, red, yellow, purple and pink use a brighter palette;
green and teal stay unchanged. Neutral grey filenames and file details are
also lifted for easier scanning. Light backgrounds, selected/insensitive items
and high-contrast themes retain their previous behavior. Type-to-jump
highlighting still works.

Outline icons use the icon theme's symbolic artwork, with distinct icons for
Home, Desktop, Documents, Downloads, Music, Pictures and other standard folders.
Photo/video thumbnails, folder cover art and explicit custom icons are kept.
Changes to these preferences take effect without restarting Nemo.

## Remembering Folders and Saving Locations

**Preferences → Behavior → Remember folders, tabs and panes when closing Nemo**
is enabled by default. Opening Nemo without a location restores the **last closed
window**, including both panes, tab order, active tabs and the active pane.
Closing the final tab also remembers its folder. Search tabs reopen the folder
where the search started, not stale search results. Explicitly opening a folder
still opens that requested folder; turning this option off stops saving and
restoring the window. Remote folders require their connection to be available.

Right-click any directory in the breadcrumb bar to **Add to Favorites** or
**Add Bookmark** without navigating there. These act on the clicked directory,
not the current selection. Existing favorites/bookmarks are not duplicated.
Favorites appear in Nemo's Favorites collection; bookmarks appear in the
Bookmarks menu and Places sidebar.

Use the arrow beside **Favorites** in the Places sidebar to expand all saved
files and folders, sorted by name. The list updates when Favorites change and
remembers whether it is expanded. Clicking the arrow does not change your
current folder; clicking a saved folder opens it, while a saved file opens in
its usual application. Clicking the Favorites label still opens the collection.
The arrow sits beside the row's icon and label, without a separate sidebar
gutter. Desktop has a monitor symbol and Favorites a starred-folder symbol;
both use the current theme's foreground color.

**Show favorites first** controls sorting within each displayed folder; it does
not insert favorites into every directory. The sidebar is the global list.

**Ctrl+D** bookmarks the current pane's folder, and **Ctrl+Alt+B** adds it to
Favorites. Both are configurable under **Preferences → Keyboard Shortcuts →
Bookmarks**. The new Favorites shortcut leaves an existing assignment alone.

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| **F3** | Quick Preview (instant file viewer) |
| **Alt+F3** | Toggle Preview Pane |
| **Alt+PgUp / Alt+PgDn** | Scroll the text/document preview up / down without changing selection |
| **Ctrl+F3** | Toggle Split View |
| **Shift+Alt+F3** | Toggle Preview Pane details |
| **Ctrl+Shift+K** | Edit keyboard shortcuts |
| **Alt+Z** | Toggle Zen mode |
| **F5** | Copy dialog |
| **Alt+F5** | Copy selected files and folders into a new 7z archive |
| **Alt+Shift+F5** | Pack into a new verified 7z archive, then remove originals |
| **F6** | Move dialog |
| **Ctrl+Left / Ctrl+Right** | Previous / next visited folder |
| **Ctrl+Up** | Parent folder |
| **Ctrl+Down** | Open the selected folder in the current pane (does not launch files) |
| **Ctrl+D** | Bookmark the current folder |
| **Ctrl+Alt+B** | Add the current folder to Favorites |
| **Esc / Backspace in search results** | Return to the folder where the search started |
| **Ctrl+Alt+O** | Open the selected result's containing folder |
| **Ctrl+Alt+Shift+O** | Open its containing folder in the other pane, preserving search results |
| **Ctrl+H** | Show/hide hidden files |
| **Ctrl+[** / **Ctrl+]** | Shrink / grow Preview Pane |
| **Ctrl+M** | Mute / unmute preview audio |
| **Ctrl+Space** | Play / pause preview pane audio/video |
| | |
| **In F3 Quick Preview dialog:** | |
| **Space** | Play / pause |
| **M** | Mute / unmute |
| **< (comma)** | Step back one frame |
| **> (period)** | Step forward one frame |
| **F** | Toggle fullscreen |
| **I** | Toggle file information and GPS map |
| **Mouse wheel** | Zoom image at the pointer |
| **Left-button drag** | Pan a zoomed image |
| **PgUp / PgDn** | Scroll within the current document |
| **Ctrl+Left / Ctrl+Right** | Previous / next file (configurable) |
| **Left / Right in media** | Seek backward / forward |
| **Esc** | Dismiss |

Image **Fit** preserves the aspect ratio and fills the available preview area,
including small images and camera RAW files. Wheel zoom turns Fit off; check it
again to restore automatic fitting. The filename remains visible at the bottom.
The information panel shows available file/camera metadata; GPS maps use cached
OpenStreetMap tiles and need a network connection for uncached locations.

Backspace still edits text in search/location entries, and Escape still cancels
renaming, active type-to-jump, or an active filter first. Both containing-folder shortcuts are listed
under **Preferences → Keyboard Shortcuts → File Operations** and can be changed.
The other-pane action is also in the result's context menu and opens split view
automatically if needed.

Toolbar hover help shows the action description with its current shortcuts on
the next line. It updates immediately when shortcuts change in **Preferences →
Keyboard Shortcuts**. The Ctrl+arrow navigation bindings are in **Navigation**;
existing Alt+Left/Right/Up and Backspace bindings remain available. New defaults
do not replace chords already assigned in shortcut settings. Ctrl+arrows keep
their text-editing behavior while a location/search entry or rename field has focus.

### Type to jump

Typing in a folder uses consecutive, case-insensitive filename matching by
default. Matches at the beginning take priority: `cu` selects `curveycase.txt`
before `misscurve.txt`, while `curve2` can select `misscurve2.txt`. This is not a
fuzzy subsequence match, and it does not hide other files. Matching filename
segments are shown in **bold** while typing.

In typing mode, **Down** moves to the next match and **Up** to the previous
one. Configure **Next Matching Filename** and **Previous Matching Filename**
under **Preferences → Keyboard Shortcuts → Type to Jump**. Each action has one
shortcut; existing customizations of these two actions are kept. The former
Right/Left and Ctrl+G/Ctrl+Shift+G alternate actions are no longer used.
Escape, Tab, clicking elsewhere, or inactivity ends typing mode and
restores normal arrow-key navigation. Prefix-only and fuzzy-filter modes remain
available through the existing `interactive-search-mode` preference.

## Upstream Relationship

We actively track upstream [linuxmint/nemo](https://github.com/linuxmint/nemo) and cherry-pick fixes in both directions. The fork is currently 0 commits behind upstream master.

Open PRs to upstream:
- [#3726](https://github.com/linuxmint/nemo/pull/3726) — Page cache throttle
- [#3722](https://github.com/linuxmint/nemo/pull/3722) — Configurable keybindings
- [#3718](https://github.com/linuxmint/nemo/pull/3718) — Substring search

## Contributing

- **Bug reports / feature requests:** [GitHub Issues](https://github.com/KonTy/nemo/issues)
- **Development:** See [INSTALLATION.md](INSTALLATION.md) for build instructions
- **PRs:** Target the `main` branch

`main` is the development and release branch. Pushes to `main` run **Build and
Release Packages**, which bumps the version once and builds both Arch and Debian
packages from that same commit. Both package builds include LibRaw and run the
transfer-safety and image-preview regressions before uploading packages.
Arch releases include `SHA256SUMS-x86_64` for the pacman package and root filesystem
archive. The packages include the document renderer and archive-creation worker;
the native theme, archive, preview, and transfer release regressions must pass.
Compilation and the release gate run as an unprivileged container user so
unreadable-source tests exercise real permission failures instead of root bypasses.
The old `release` branch is retained for
history; it is no longer the publishing source. To publish manually, run **Build
and Release Packages** on `main`; dispatching **Build Debian Package** alone only
builds an artifact.

## License

nemo-smpl is free software released under the [GNU General Public License v2.0](COPYING) or later.

---

**nemo-smpl is maintained with ❤️ for the smplOS community.**
**Upstream Nemo is maintained by the Linux Mint team.**
