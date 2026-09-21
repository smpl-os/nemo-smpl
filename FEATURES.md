# nemo-smpl — Features & Release Notes

**Current Version:** v1.4.2  
**Release Date:** March 11, 2026

---

## Feature Tracking

This document tracks which features in nemo-smpl are upstream and which are exclusive.

### Upstream (Merged into linuxmint/nemo)

None yet — we're working on contributing suitable features upstream.

### Pending Upstream

These features are candidates for upstream contribution:

- [ ] **Page Cache Throttle** — PR [#3726](https://github.com/linuxmint/nemo/pull/3726)
- [ ] **Configurable Keybindings** — PR [#3722](https://github.com/linuxmint/nemo/pull/3722)
- [ ] **Substring Search** — PR [#3718](https://github.com/linuxmint/nemo/pull/3718)

---

## nemo-smpl Exclusive Features

These features are developed for nemo-smpl and are not expected to be accepted upstream.

### F3 Quick Preview

Double Commander-style instant file viewer:

- Text, image (including animated GIFs and camera RAW: DNG, ARW, CR2, NEF…), audio/video (GStreamer), and hex dump modes
- Directory analysis: F3 on a folder shows Pareto bar chart + ranked biggest-files list
- Paged text/hex viewer using `pread()` + LRU cache — handles multi-GB files with ~512 KB resident
- **Timecode display**: live `hh:mm:ss:ff` timestamp (monospace) with auto-detected framerate
- **Frame stepping**: `<` (comma) steps back one frame, `>` (period) steps forward — pauses playback automatically, like YouTube
- **Keyboard media controls**: Space = play/pause, M = mute/unmute, F = fullscreen, ← → = navigate files
- Escape to dismiss; singleton window reused across invocations
- Split view moved to Ctrl+F3
- Modular architecture: `NemoImageViewer`, `NemoPagedViewer`, `NemoPreviewUtils` shared between sidebar pane and quick preview

### Shared Directory Analyzer Widget

- Reusable `NemoDirAnalyzer` widget for directory-size Pareto analysis
- Used by both the Overview page (per-volume) and F3 Quick Preview (per-folder)
- Vertical bar chart + ranked list with clickable paths
- Background async scan with `"scan-finished"` signal

### Verify After Copy/Move

- Copy verification defaults to on and is a persistent preference, shared by the copy dialog, clipboard, drag-and-drop, and API operations. The copy dialog offers "Verify copied and existing files". Copied moves always verify; a native rename is not a checksum-verified copy.
- Verified streamed copies compute SHA-256 while reading the source, then flush and read back the unpublished destination to compare it. Copied moves additionally check the captured source before deleting it; verification and safe source retirement can require extra reads.
- Verified copies also compare existing regular files against the source with SHA-256. Stable matching files count as already present only after the required synchronization, without rewriting them or adding undo actions for them. Both source and destination must remain unchanged during comparison.
- Different existing contents are a conflict, not evidence of corruption: **Replace**, **Replace All**, rename, or skip. Replacement requires explicit approval, supported atomic exchange, and recovery storage for the displaced destination. **Replace All** still checks each existing file and does not rewrite verified matches. Unreadable or changing files are not treated as mismatches.
- If an existing regular source or target has no usable etag/modification-time token, a verified copy cannot establish stable comparison or replacement approval: both files are retained and the operation is reported unverifiable/incomplete, not corrupt. This also applies to known size differences; verification is never silently downgraded.
- Moves keep size, identity, and content/change checks around the transfer and source retirement. Copied moves capture the original in a controlled namespace before the final comparison and deletion; successful moves reclaim source space. Detected changes stop deletion and retain or safely restore the original.
- Mismatch, incomplete readback, or unavailable verification before publication never replaces an existing destination or deletes the source
- Copies and verification reads must cover known file sizes; premature end-of-stream is not accepted as a completed or verified transfer
- Staging and checked synchronization apply to ordinary same-filesystem copies too. The legacy `safe-cross-fs-copy` preference no longer disables these safeguards.
- Cross-filesystem copy-and-delete moves always verify regular files before deleting their source. Supported same-filesystem moves retain a native rename path with checked synchronization and recovery records; moving a directory can require inspecting and synchronizing its tree, but not copying every file's contents.
- Local stream copies keep the writer open through checked `fsync`, then publish using an atomic no-replace operation or guarded exchange and flush the affected directories. There is no stat-then-rename or destructive GIO-overwrite fallback.
- Errors after a namespace change are reported as incomplete with the actual destination/recovery state. An item may already have moved; Nemo does not blindly retry or roll back over a newer file.
- Destination roots and relevant local source parents are pinned and revalidated so a disappeared or replaced mount cannot silently redirect a queued move into its underlying directory.
- Directory-symlink traversal is refused in hardened transfers, including symlinks in root/ancestor paths. Use the actual directory location instead of a symlink alias.
- Unsupported destination filesystems/backends are refused before transfer mutation, including remote destinations, unqualified network/FUSE mounts, and volatile tmpfs destinations. Remote sources can be copied to supported local storage; remote-source moves are refused rather than deleting an original on unproven backend guarantees.
- Filesystem eligibility does not imply every operation is supported. In particular, replacement requires atomic exchange support; where it is unavailable (including some common removable-media filesystems), keep the existing file and choose a new destination name instead.
- Destructive Restore/Move operations from the virtual Trash backend are also refused when a safe paired payload/metadata operation cannot be established. Use a verified Copy from Trash to supported local storage instead; the original remains in Trash until you explicitly remove it.
- `posix_fadvise(DONTNEED)` requests cache eviction, but is not a physical-media read guarantee. Checked synchronization relies on the operating system, filesystem, and device honestly honoring their contracts.
- Cancel/Skip/Skip All retain unsafe items rather than disabling later verification
- Short staging/recovery names accommodate long final filenames; symbolic links are copied and compared without following their targets. A custom name for a copied directory applies only to its root, not to every child.
- Stream-copy staging files start private. Source permissions and timestamps are copied where supported; when a backend supplies no Unix permissions (or default target permissions are requested), a local stream-copy staging file conservatively keeps mode `0600`
- Pull-only remote sources can still copy to local storage through GIO's native backend. Its output stays in a private temporary folder and uses checked filesystem-wide `syncfs` with an error-tracking descriptor opened before the transfer; requested verification still requires a readable source. Separate backend processes receive Nemo-PID-qualified descriptor paths, not `/proc/self` paths. If a backend cannot access that protected namespace, the copy fails without switching to an unpinned public pathname.
- Copies and moves automatically merge existing destination folders without a folder-level skip/merge prompt. Every source child is processed and extra destination files are never deleted. Conflicting files still require a decision; merging a folder does not authorize overwriting its files. File-to-folder or folder-to-file replacement requires a different destination name rather than deleting existing data before the replacement is ready.
- Symbolic-link equality means matching link text, without following the target (including broken links or links to FIFOs). Existing-file recognition applies to copies only, not skipped moves or intentionally named duplicates.

#### Recovery and limits

- Recovery storage uses `.nemo-recovery-<UID>/<HH>/transaction-<UUID>` at the reported locations. The two-character bucket limits directory growth for large libraries. Use **Show Hidden Files** (`Ctrl+H`) to see it. Owner markers identify the controlled containers; do not edit those markers. `record-<phase>` files describe the transaction, with URI bytes base64-encoded so unusual filenames remain unambiguous. Depending on the operation, `payload` contains a displaced destination, `captured-source` a retained original, or `undone` data kept for guarded redo.
- Replaced destinations and redo/recovery payloads do not expire when undo history changes or Nemo exits. They consume disk space; remove them only after confirming they are no longer needed. Successful copied moves reclaim the source file's data. Completed owned metadata-only transactions are cleaned up; guarded undo allocates a fresh transaction when needed. Data-bearing or unknown entries are never garbage-collected, and there is no automatic recovery replay.
- Guarded file/symlink copy undo and supported same-filesystem move undo identify the entries created by the operation and refuse conflicts with newer data. They do not blindly overwrite a newer file or recursively delete unrelated folder contents.
- Undo records containing newly created folders/trees, cross-filesystem moves, or insufficient safety information are refused before mutation. A merge into existing directories can still be undone when its records contain only supported file operations; unrelated existing children remain untouched. A refusal detected before any undo mutation keeps history available and is not reported as user cancellation or a successful restore. Use the reported recovery data or a separate verified copy when manual restoration is needed.
- Results retain recovery locations and incomplete-operation reasons. A failure after publication or source retirement is not a promise that the original pathname still exists; consult the reported state before retrying.
- A recovery-initialization attempt that touches storage and then fails stops further initialization attempts for that namespace in the same job. Later retained files can repeat the originating error without having attempted another write. Resolve the storage problem and start a new operation. Failed or uncertain initialization directories are retained at the reported locations rather than swept automatically.
- Keep source and destination files unchanged during a transfer. Identity and checksum checks detect changes, but are not filesystem snapshots and cannot freeze an already-open concurrent writer after the final check.
- File-content verification does not certify preservation of all ownership, ACLs, extended attributes, timestamps, or hard-link relationships. Unsupported optional metadata is distinguished from fatal I/O and space errors.
- These safeguards apply to hardened `smpl_features=true` builds, not the upstream-comparison build with that option disabled. They are not a whole-job transaction or a guarantee against hardware failure. Keep independent backups.

### Copy/Move Completion Feedback

- The non-modal File Operations window keeps completed results until **Close**. Closing it clears completed summaries and hides the window; active transfers continue.
- Each new copy/move batch opens the window immediately, including queued or paused transfers and jobs that finish before the first UI update. A previous hidden result does not leave later batches invisible.
- Active work does not display 100% merely because all bytes have been submitted. Flushing and verification remain unfinished work; indeterminate progress clears stale percentages, including the status icon.
- Copy/move results distinguish success, incomplete/skipped work, failure and cancellation. SHA-256 file-content verification and symbolic-link target checks are reported separately. Atomic renames and empty folders are not described as checksum-verified.
- Newly copied files, verified existing files, and existing files retained without verification have separate counters. With verification off, ordinary retained-file conflicts can finish neutrally as **existing files retained (not verified)**; this is not content-confirmed success. Known differences, unexamined skipped folders, failures, and cancellation remain incomplete. Successful copy callbacks require every source item to be actually copied or verified already present.
- Fully successful copies clear the original source selection only if that view, location and selection stayed unchanged. F5/Copy To, same-process clipboard paste and internal drag-and-drop use explicit weak source tracking; changed selections, navigation, closed tabs, failures, unverified retention and external/unknown origins are left alone. Destination highlighting is preserved without overriding newer user selections.
- If you hide the window during an active batch, its completions send a desktop notification without reopening it. The status icon or notification opens the results; a fresh copy/move batch also opens the window again.
- Nemo retains the latest 50 text summaries in memory, with an explicit notice when older results are omitted. Results are not written to disk. A visible summary keeps Nemo running until closed; hidden results do not. If Nemo exits, the desktop notification is the remaining record, subject to the desktop's notification-retention settings. Reopening a result after exit requires a desktop notification backend that supports restarting application actions; freedesktop notification backends can only reopen it while Nemo is running.
- Transfer completion is separate from safe device removal. All removal entry points share per-operation feedback, and only successful completion of the removal request reports removal success.
- Device removal waits for users to finish or cancel queued, running, and paused operations; new transfers are refused while removal is in progress. This gate is deliberately conservative across devices.
- Removal does not offer forced unmount or automatically empty the device's trash. Use the explicit Empty Trash action separately, and never unplug based only on a transfer percentage.

### Per-Pane Location Labels

- Compact path label above each pane in dual-pane mode
- Tilde-shortened paths (e.g. `~/Documents`) or URIs for non-local locations
- Configurable via GSettings key `show-dual-pane-location-labels` (default: on)
- Preference checkbox under Views → Behavior

### Embedded Icon Defaults

- Flat XSI symbolic artwork and Adwaita file/place icons are built into Nemo, including dynamic device names and Nemo's own sidebar/layout/progress icons
- Missing or incomplete themes need no installed icon package or generated symlink cache; usable themed icons retain precedence
- Device selection checks actual lookup results rather than trusting an icon-cache name alone
- Artwork sources, hashes, and license/attribution notices are maintained in `gresources/default-icons/` and installed with Nemo

### MTP Empty Folders

- Successful empty phone folders remain ordinary empty folders, without an unlock warning or repeated two-second reloads
- Access guidance is driven by explicit load errors, not file counts or generic permission/libmtp messages
- Successful loads and navigation end pending access retries; both MTP URIs and their local GVfs aliases are recognized without synchronous device queries

### Preview Pane (Alt+F3)

- Live image/video preview with EXIF metadata display (works on both X11 and Wayland)
- GPS map display for geotagged photos (OpenStreetMap)
- Adjustable preview width (Ctrl+[ / Ctrl+])
- **Media keyboard shortcuts**: Ctrl+M = mute/unmute, Ctrl+Space = play/pause
- Toggle metadata details panel with Shift+Alt+F3
- All preview pane shortcuts are configurable via `org.nemo.keybindings`

### Disk Usage Overview

- Interactive Pareto charts for each mounted volume
- Deep directory scan with top offenders list
- Side-by-side layout (chart + list) with bookmark/anchor navigation
- Background cache with periodic refresh

### smplOS Live Theming

- Accent colors, backgrounds, and selection highlights update instantly via `theme-set`
- `GFileMonitor` watches `~/.config/smplos/nemo-theme.css` for live CSS reload
- All 15 smplOS themes ship pre-baked `nemo.css` files
- Compiled under `#ifdef SMPLOS` — upstream patches contain none of this code

### Archive Support

- **Browsing**: Double-click ZIP/7z/TAR archives to browse contents via FUSE
- **Creation**: Right-click "Compress to Archive" with progress feedback
- Supports ZIP, 7z, TAR, TAR.GZ, TAR.BZ2, TAR.XZ, RAR

### MTP Device Support

- Automatic udev rules prevent gphoto2 conflicts
- Retry logic for transient USB "device busy" errors
- Clear error messages: "Phone locked?", "Driver missing?", "Device busy"
- Covers Samsung, Google Pixel, HTC, LG, Sony, Motorola

### Command-Line Flags

- `--class` / `-c`: Set custom WM_CLASS at launch (for compositor floating rules)
- `--select` / `-s`: Open parent directory and highlight a specific file

### Other Enhancements

- **Configurable Keyboard Shortcuts** — edit all keybindings via preferences, including media controls (`toggle-mute`, `toggle-play`)
- **Substring Search** — match anywhere in filename, not just prefix
- **Tab-based Pane Splitting** — Tab to switch focus, Ctrl+N for new split pane
- **Copy Path** — right-click "Copy Path" to clipboard
- **Cover Art Directory Icons** — directories with `cover.jpg`/`cover.png` use them as folder icons
- **Performance Fixes** — USB copy throttling, memory leak corrections, use-after-free crash fixes

---

## Version History

### v1.4.2 (March 2026)

- **`#ifdef NEMO_SMPL` compile guards**: All parent-folder-entry code wrapped with compile-time guards for clean upstream separation — comment out `#define NEMO_SMPL 1` in `config.h.meson.in` to build without smplOS additions
- **Sidebar cross-section keyboard navigation**: Arrow keys in sidebar now cross section boundaries (e.g. from Bookmarks into Devices) instead of stopping at section edges
- **"..' parent folder entry**: Optional `..` row pinned at top of list view for navigating to parent directory — toggle via View menu or GSettings key `show-parent-folder-entry`
- Upstream sync: confirmed 0 commits behind upstream `linuxmint/nemo` master

### v1.4.1 (March 2026)

- **Media keyboard shortcuts**: Ctrl+M = mute/unmute preview audio, Ctrl+Space = play/pause preview pane, M/Space in F3 dialog
- **Timecode display**: live `hh:mm:ss:ff` timestamp in F3 Quick Preview with auto-detected framerate
- **Frame stepping**: `<` (comma) / `>` (period) step back/forward one frame in F3 preview
- **Sidebar focus on F9**: F9 toggle now selects first sidebar node (Overview) and clears file view selection on show; restores file view focus on hide
- Fixed 4 compiler warnings across 3 files
- Keybinding swap: Ctrl+M = mute (was create symlink), Ctrl+Shift+M = create symlink (was mute)

### v1.4.0 (March 2026)

- **Wayland-compatible video preview**: Replaced X11 overlay (`xvimagesink`) with `appsink` + Cairo rendering in both F3 Quick Preview and sidebar Preview Pane — video now works on X11 and Wayland equally
- **Camera RAW image support**: DNG, ARW, CR2, CR3, NEF, ORF, PEF, RAF, RW2, and 10+ more RAW formats rendered via libraw in both sidebar and F3 preview
- **New dependency**: optional `libraw` (≥0.20) for RAW image decoding; `gstreamer-app-1.0` for appsink
- Removed all X11-specific video code (`videooverlay.h`, `gdkx.h`, `GDK_WINDOW_XID`, `bus_sync_handler`)
- Video rendering uses mutex-protected `cairo_surface_t` frame buffer with aspect-ratio letterboxing
- Cleaned up debug logging from GStreamer pipeline

### v1.3.0 (March 2026)

- Renamed project from smplos-nemo to **nemo-smpl** across all docs, CI, and source
- Consolidated README.md and FEATURES.md — removed duplication, unified branding
- Upstream linuxmint CI no longer runs on release branch (no more stale artifacts)
- Cleaned up old GitHub releases (v1.0.x, preview-demo-assets)
- **F3 Quick Preview**: instant file viewer (text, image, media, hex, directory analysis)
- **Shared NemoDirAnalyzer widget**: directory-size Pareto analysis reused by Overview + F3
- **Verify after copy/move**: SHA-256 checksum checkbox in F5/F6 dialogs
- **Per-pane location labels**: compact path display above each pane in dual-pane mode
- **Cover art directory icons**: cherry-picked from upstream PR #3728
- **Keybinding changes**: F3 → Quick Preview, Ctrl+F3 → Split View
- Modular preview architecture: NemoImageViewer, NemoPagedViewer, NemoPreviewUtils
- Overview page refactored — ~350 lines of duplicated scan code removed
- Debian CI build fixed (LMDE 7 image)

---

## Contributing

### Feature Requests

[Open an issue](https://github.com/KonTy/nemo/issues) on the nemo-smpl repository.

### Development

1. Create a feature branch: `git checkout -b feature/my-feature`
2. Implement and test locally
3. Create a PR against the `release` branch
4. If suitable for upstream, we'll prepare a clean PR to [linuxmint/nemo](https://github.com/linuxmint/nemo)

---

**Maintained by:** smplOS Development Team
