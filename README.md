nemo-smpl
=========

[![Build](https://github.com/KonTy/nemo/actions/workflows/build-arch.yml/badge.svg)](https://github.com/KonTy/nemo/actions/workflows/build-arch.yml)
[![Debian](https://github.com/KonTy/nemo/actions/workflows/build-debian.yml/badge.svg)](https://github.com/KonTy/nemo/actions/workflows/build-debian.yml)

**nemo-smpl** is an enhanced fork of [Nemo](https://github.com/linuxmint/nemo), the file manager for the Cinnamon desktop environment. This fork is maintained for **smplOS** and ships features that upstream considers out-of-scope.

## Highlights

| Feature | Stock Nemo | nemo-smpl |
|---------|:----------:|:---------:|
| F3 Quick Preview (text/image/media/hex/dir) | ❌ | ✅ |
| Camera RAW preview (DNG, ARW, CR2, NEF…) | ❌ | ✅ |
| Wayland video preview (sidebar + F3) | ❌ | ✅ |
| Timecode display (hh:mm:ss:ff) in F3 preview | ❌ | ✅ |
| Frame-by-frame stepping (< >) in F3 preview | ❌ | ✅ |
| Media keyboard shortcuts (play/mute) | ❌ | ✅ |
| Copy verification by default (SHA-256) | ❌ | ✅ |
| Preview Pane (Alt+F3) with GPS map | ❌ | ✅ |
| Disk Usage Overview (Pareto charts) | ❌ | ✅ |
| Archive browsing (ZIP/7z/TAR as folders) | ❌ | ✅ |
| Configurable keyboard shortcuts | ❌ | ✅ |
| Substring search | ❌ | ✅ |
| MTP device support (hardened) | ⚠️ | ✅ |
| smplOS live theming | ❌ | ✅ |
| Cover art directory icons | ❌ | ✅ |
| Per-pane location labels | ❌ | ✅ |

See [FEATURES.md](FEATURES.md) for full details and release notes.

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

The File Operations window shows the transfer result and SHA-256 verification
summary. Technical notes and recovery locations are under **Details**, expanded
automatically for incomplete operations. **Enter**, **Escape**, or **Close**
dismisses the window without cancelling active transfers.

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| **F3** | Quick Preview (instant file viewer) |
| **Alt+F3** | Toggle Preview Pane |
| **Ctrl+F3** | Toggle Split View |
| **Shift+Alt+F3** | Toggle Preview Pane details |
| **Ctrl+Shift+K** | Edit keyboard shortcuts |
| **F5** | Copy dialog |
| **F6** | Move dialog |
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
| **←** / **→** | Previous / next file |
| **Esc** | Dismiss |

## Upstream Relationship

We actively track upstream [linuxmint/nemo](https://github.com/linuxmint/nemo) and cherry-pick fixes in both directions. The fork is currently 0 commits behind upstream master.

Open PRs to upstream:
- [#3726](https://github.com/linuxmint/nemo/pull/3726) — Page cache throttle
- [#3722](https://github.com/linuxmint/nemo/pull/3722) — Configurable keybindings
- [#3718](https://github.com/linuxmint/nemo/pull/3718) — Substring search

## Contributing

- **Bug reports / feature requests:** [GitHub Issues](https://github.com/KonTy/nemo/issues)
- **Development:** See [INSTALLATION.md](INSTALLATION.md) for build instructions
- **PRs:** Target the `release` branch

## License

nemo-smpl is free software released under the [GNU General Public License v2.0](COPYING) or later.

---

**nemo-smpl is maintained with ❤️ for the smplOS community.**
**Upstream Nemo is maintained by the Linux Mint team.**
