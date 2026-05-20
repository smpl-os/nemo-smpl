# Upstream Sync Audit

**Generated:** 2026-07-18
**Baseline:** `upstream/master` = `932438fc` (post-6.7.4-unstable)
**Fork tip:** `release` = 18 commits ahead of `origin/release` (after 6.7.4 sync merge)

## Purpose

Answer the question: *"How do we stop deviating from upstream without losing
our features?"* This report classifies every fork modification into one of
four tiers so we know exactly what needs work.

---

## Numbers at a glance

| Metric | Value |
|---|---|
| Total diff vs upstream | **+15,755 / −454** |
| Files touched | **115** |
| Files with only-additions (safe) | ~71 |
| Files that modify upstream code (hot) | ~30 |
| Files with existing `NEMO_SMPL` guards | 5 |
| Total `NEMO_SMPL` sites | ~25 |

**Bottom line:** ~97% of our diff is pure additions. The friction lives in ~30
"hot" files, and most of that friction can be eliminated by wrapping our
changes in `NEMO_SMPL` guards or by adding runtime `GSettings` prefs.

---

## Tier definitions

- **Tier 1 — Bolt-on files (zero conflict).** New files that upstream doesn't
  know about. Sync is free forever. **No work needed.**
- **Tier 2 — Additive changes to upstream files.** Wrap in
  `#ifdef NEMO_SMPL … #endif`. Purely mechanical. Reduces conflict surface
  to zero when flag is off.
- **Tier 3 — Behavior changes to upstream code.** Needs a runtime
  `GSettings` pref so both upstream and our behavior can co-exist and the user
  chooses. This is the only tier that changes user-visible defaults.
- **Tier 4 — Bugfixes.** Should be submitted upstream and then deleted from
  our fork on the next sync. Reduces our diff permanently.

---

## Tier 1: bolt-on files (no work)

71 files where our diff is purely additions. Examples:

- `src/nemo-preview-pane.{c,h}` — F3 preview pane
- `src/nemo-quick-preview.{c,h}` — quick preview subsystem
- `src/nemo-paged-viewer.{c,h}` — multi-file media viewer
- `src/nemo-image-viewer.{c,h}`
- `src/nemo-overview.{c,h}` + `src/nemo-dir-analyzer.{c,h}` — overview page
- `src/nemo-keybindings.{c,h}` — configurable keybindings
- `src/nemo-archive-mounter.{c,h}` — archive browsing
- `libnemo-private/nemo-malloc-utils.{c,h}` — (from upstream sync)
- CI: `.github/workflows/build-{arch,debian}.yml`
- Docs: `FEATURES.md`, `INSTALLATION.md`, `Documents/media/SHOTLIST.md`
- Data: `data/70-disable-gphoto-for-mtp.rules`, custom actions in
  `files/usr/share/nemo/actions/`
- Support scripts: `support/*.sh`
- Full list: `git diff --numstat upstream/master...HEAD | awk '$2 == "0" && $1 != "0" {print $3}'`

**Status:** ✅ already safe. No action.

---

## Tier 2: additive hunks in upstream files → wrap in `NEMO_SMPL`

These files add code inside upstream files but don't remove upstream logic.
Wrap every added block in `#ifdef NEMO_SMPL … #endif` and the file becomes a
2-way clean merge target for future syncs.

### High priority (largest diffs)

| File | Δ | Current guards | Work |
|------|---|----------------|------|
| `src/nemo-window-menus.c` | +1001 / −20 | 5 sites ✓ (partial) | Wrap remaining menu registrations. **~2 hours.** |
| `src/nemo-window.c` | +706 / −5 | 0 | Preview-pane + split-pane hook points. Wrap ~10 sites. **~1 hour.** |
| `src/nemo-places-sidebar.c` | +415 / −20 | 0 | Cross-section keyboard nav (Alt+Shift+P/T), overview sidebar entry. **~1 hour.** |
| `src/nemo-window-slot.c` | +162 / −12 | 0 | Preview-pane hooks, overview navigation history. **~30 min.** |
| `src/nemo-statusbar.c` | +124 / −4 | 0 | Sidebar toggle button + dynamic tooltip. **~15 min.** |
| `src/nemo-application.c` | +211 / −0 | 0 | Overview app-registration. Already additive-only; still worth wrapping for future-proofing. **~15 min.** |

### Medium priority

| File | Δ | Notes |
|------|---|-------|
| `src/nemo-desktop-application.c` | +112 / −16 | Wayland / MTP integration hooks |
| `src/nemo-window-manage-views.c` | +192 / −2 | Overview navigation state |
| `src/nemo-main-application.c` | +276 / −13 | Note: 88/11 lines came from upstream 6.7 sync (`bc2579a3`, `409fced9`). Only the delta belongs to us. |
| `libnemo-private/nemo-file.c` | +246 / −3 | **Almost all is Tier 4** — see below |
| `libnemo-private/nemo-file-operations.c` | +334 / −15 | USB copy throttle is Tier 3; rest additive |

**Total effort estimate for Tier 2:** ~1 focused session (4-6 hours).

---

## Tier 3: behavior changes → runtime `GSettings` prefs

Adding a pref for each of these lets us pull upstream's version as an
alternative code path and let the user pick.

### 3a. Interactive search (this session — in progress)

- **Upstream:** `11df68cc` fzy filter view (active filter, hides non-matches,
  highlights matching chars).
- **Ours:** `84694965` substring type-ahead selection (selects & scrolls, no
  hiding).
- **Pre-both:** vanilla prefix-only match.
- **Pref:** `org.nemo.preferences.interactive-search-mode`
  enum: `prefix` | `substring` | `filter`. Default: `substring`.
- **Files touched:** `libnemo-private/nemo-icon-container.c`,
  `src/nemo-list-view.c`, plus all files added by `11df68cc`.

### 3b. Directory-listing responsiveness

- **Upstream:** `736b7ba0` view: hold-and-flush strategy (batches pending
  files, flushes on `done_loading_callback`, `UPDATE_INTERVAL_MIN=10`).
- **Ours:** `083f8b60` snappy tuning (`UPDATE_INTERVAL_MIN=50 MAX=200`,
  `g_idle_add` on first batch, `DIRECTORY_LOAD_ITEMS_PER_CALLBACK=500`).
- **Pref:** `org.nemo.preferences.directory-load-strategy`
  enum: `progressive` | `snappy` | `hold-and-flush`. Default: `snappy`.
- **Files touched:** `src/nemo-view.c`, `libnemo-private/nemo-directory-async.c`.
- **Effort:** medium. Requires factoring both strategies through a common
  hook. Follow-up commits `ec06509a`/`71b73d1f`/`65c9b890` become available
  once the pref exists.

### 3c. USB copy throttle

- **Ours:** `20d5664` + `a2cad67` throttle page cache during cross-device
  copies via incremental `sync_file_range`, `cd4f2776` prevents USB lockup.
- **Upstream:** does none of this.
- **Pref:** `org.nemo.preferences.usb-copy-throttle` boolean. Default: `true`.
- **Files touched:** `libnemo-private/nemo-file-operations.c` (+334 lines).
- **Note:** worth trying to upstream this as a PR — the underlying bug is
  real on any Linux system with slow removable media.

### 3d. Interactive-search prefix vs substring is inside 3a; nothing else here.

---

## Tier 4: bugfixes → submit upstream, then delete

### 4a. `nemo_file_mark_gone` use-after-free (issue #3712)

- **Ours:** `5af7a36c` + `c3f954fc` in `libnemo-private/nemo-file.c` (~14
  lines).
- **Status:** ✅ already merged upstream as `07f5a887` (by KonTy = us).
- **Action:** when we next merge a Nemo release containing `07f5a887`, drop
  our version. This removes almost all of the `+246/−3` diff in
  `nemo-file.c`.

### 4b. Bookmark-picker positioning fixes

- **Ours:** `9750ff47`, `066ab2ad`, `963fcbab`, `cdc1d003`, `baa8e573`,
  `9ec55823`, `e76027cf`, `213200d3`, `02a42a45`, `41f8a7d1`, `1088303f`,
  `3a6d1d0e`, `48e20e2e`, `85ec8660`, `9c777b18`, `2c46beac`, `87642117` —
  the long "make mnemonics visible" saga.
- **Status:** would probably be accepted upstream as a fix for the same GTK
  regression. Not in our fork's diff cleanly because it evolved through many
  attempts. Squash + PR is the right move if we care to reduce diff.
- **Action:** low priority; the code is stable now, just noisy in history.

### 4c. Sidebar Terminal launch (Terminal=true)

- **Ours:** `2496beac` "submenu black-on-black text, Terminal=true app launch
  (micro), unmounted volumes in Alt-F1/F2 picker".
- **Status:** the Terminal=true fix is a real bug (upstream just ignores the
  `Terminal=true` field for the sidebar); worth an upstream PR.
- **Action:** file upstream issue, split the commit.

---

## Prescribed order of work

1. **Now (this session):** Tier 3a — add
   `org.nemo.preferences.interactive-search-mode` and pull `11df68cc`.
2. **Next session (mechanical):** Tier 2 sweep. One PR per file, purely
   wrapping in `#ifdef NEMO_SMPL`. Zero behavior change.
3. **Follow-up:** Tier 4a — merge upstream release containing `07f5a887`,
   delete our version.
4. **Later:** Tier 3b (directory-load-strategy pref). This unlocks pulling
   the whole 6.7 view-loading commit group.
5. **Someday:** Tier 3c (USB throttle pref) — or upstream the throttle as a
   proper PR.

---

## Hygiene notes

- `git config rerere.enabled = true` ✅ (conflicts we resolve once won't
  need re-resolution on future syncs).
- `NEMO_SMPL` compile flag exists in `config.h.meson.in` ✅. Comment says
  "omit this line when contributing upstream" — this is exactly the pattern
  we want. It just needs consistent use.
- Consider adding a `meson_options.txt` boolean `smpl_features = true` that
  gates the `-DNEMO_SMPL` define, so building against upstream is a single
  meson flag flip.

## Commands used

```sh
# Diff surface
git diff --shortstat upstream/master...HEAD
git diff --numstat upstream/master...HEAD | sort -rn -k2

# Guard coverage
grep -rn NEMO_SMPL src/ libnemo-private/

# Per-file history
git log --oneline upstream/master..HEAD --no-merges -- src/nemo-view.c
```
