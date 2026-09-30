# Installation Guide — nemo-smpl

This guide covers installation across different distributions and methods.

## 📦 Quick Start by Distribution

### Arch Linux / smplOS

**Via official repositories:**
```bash
sudo pacman -S nemo-smpl
```

**Via AUR (Arch User Repository):**
```bash
yay -S nemo-smpl
# or
paru -S nemo-smpl
```

The AUR package automatically:
- ✅ Deploys MTP udev rules
- ✅ Reloads udev configuration on install
- ✅ Installs all required dependencies (gvfs-mtp, gstreamer, libexif, etc.)

### Debian / Ubuntu

**From GitHub Releases (Recommended):**

1. Download latest `.deb` from [GitHub Releases](https://github.com/KonTy/nemo/releases)
   ```bash
   # Example for v1.4.0:
   wget https://github.com/KonTy/nemo/releases/download/v1.4.0/nemo-smpl_1.4.0-1_amd64.deb
   ```

2. Install the package:
   ```bash
   sudo dpkg -i nemo-smpl_1.4.0-1_amd64.deb
   sudo apt-get install -f  # Install missing dependencies if needed
   ```

**Build from source on Debian/Ubuntu:**
```bash
git clone https://github.com/KonTy/nemo.git
cd nemo
debuild -us -uc  # Requires devscripts, debhelper
```

### Other Linux Distributions

**Build from source:**
```bash
git clone https://github.com/KonTy/nemo.git
cd nemo

# Install build dependencies (adjust for your distro)
# Ubuntu: sudo apt-get install -y meson ninja-build [...dev packages...]
# Fedora: sudo dnf install -y meson ninja-build [...dev packages...]

meson build -Dsmplos_branding=true
ninja -C build
sudo ninja -C build install
```

**Dependencies by distribution:**

<details>
<summary><b>Ubuntu/Debian</b></summary>

```bash
sudo apt-get install -y \
  build-essential meson ninja-build pkgconf \
  libgtk-3-dev libglib2.0-dev libjson-glib-dev \
  libx11-dev libxapp-dev libcinnamon-desktop-dev \
  libexif-dev libraw-dev libexempi-dev libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev libgsf-1-dev \
  libmd4c-dev libmd4c-html0-dev libpoppler-glib-dev mupdf-tools bubblewrap \
  gobject-introspection libgirepository1.0-dev \
  intltool itstool gtk-doc-tools
```

</details>

<details>
<summary><b>Fedora/RHEL</b></summary>

```bash
sudo dnf install -y \
  meson ninja-build pkgconfig \
  gtk3-devel glib2-devel json-glib-devel \
  libX11-devel xapp-devel cinnamon-desktop-devel \
  libexif-devel LibRaw-devel exempi-devel gstreamer1-devel \
  md4c-devel poppler-glib-devel mupdf bubblewrap \
  gstreamer1-plugins-base-devel libgsf-devel \
  gobject-introspection-devel intltool itstool \
  gtk-doc
```

</details>

<details>
<summary><b>Arch Linux</b></summary>

```bash
sudo pacman -S --needed \
  base-devel meson ninja pkgconf \
  gtk3 glib2 json-glib libx11 xapp cinnamon-desktop \
  libexif libraw exempi gstreamer gst-plugins-base-libs \
  md4c poppler-glib mupdf-tools bubblewrap \
  pango gobject-introspection libgsf intltool itstool
```

</details>

---

## Native document preview dependencies

Document previews are enabled by default in smpl builds. They require
`md4c-html` (0.4.8 or newer), `poppler-glib` (21.12 or newer), the stock
`mutool` executable from `mupdf-tools` (`mupdf` on Fedora), and `bubblewrap`. Distribution packages
install these dependencies; MuPDF remains a separate program and is not linked
into Nemo. No proprietary reader, web service, or DRM component is used.

The renderer is installed under the configured Meson `libexecdir`; packaging
must include `nemo-document-renderer` as well as the main executable. Bubblewrap
must be permitted to create its restricted namespaces. If system policy blocks
that sandbox, the preview reports the error rather than bypassing isolation.
Use `-Ddocument_preview=false` for a build without this optional feature.

Markdown, EPUB, plain UTF-8 FB2 without DTDs, basic unencrypted/PalmDOC MOBI, PDF and CBZ are the
supported preview formats. Advanced Kindle variants, DRM and external Markdown
images are not supported. Native conversion and rendered-page limits may reject
oversized or unusually complex documents.

## 📱 Android/Phone File Transfer (MTP Setup)

All installation methods automatically configure MTP support. After installation:

### Initial Setup

1. **Connect your Android phone via USB cable**

2. **On your phone:**
   - Unlock the screen
   - Go to Settings → Developer Options (or search for "Developer Options")
   - Scroll down and enable **USB Debugging** OR **File Transfer** mode
   - When the "Allow USB debugging?" prompt appears, tap **Allow**
   - Select **File Transfer** mode (not Charging)

3. **On your computer:**
   - Open nemo
   - Your phone should appear in the sidebar as "Android Device" or "SAMSUNG Android" (device name)
   - Click to mount it
   - Browse files like a regular folder

### Features

- ✅ Drag & drop files to/from phone
- ✅ Shows device battery percentage
- ✅ Automatic retry if connection briefly drops
- ✅ Works with: Samsung, Google Pixel, HTC, LG, Sony, Motorola, and most Android devices
- ✅ No gphoto2 conflicts (camera app won't interfere)

### Troubleshooting

| Problem | Solution |
|---------|----------|
| Phone not showing in sidebar | Unlock phone screen (MTP requires unlocked for security) |
| "Unable to open MTP device" error | Run `mtp-detect` to verify device is recognized; check `pacman -S libmtp` |
| Connection drops frequently | Try different USB cable or port; may be hardware issue |
| Only showing camera/photos, not all files | Switch phone to "File Transfer" mode, not "PTP" or "Charging" |

**Advanced troubleshooting:**
```bash
# Check device detection
mtp-detect

# Manually mount if sidebar button doesn't work
gio mount mtp://[usb:001,007]/

# Force udev rule reload (shouldn't be needed)
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb
```

---

## 🛠️ Configuration & Customization

### Keyboard Shortcuts

1. Open nemo
2. Press **Ctrl+Shift+K** to open shortcuts editor
3. Customize any action to your preferred key combination
4. Changes are saved instantly

**Common shortcuts:**
| Shortcut | Action |
|----------|--------|
| **Alt+F3** | Toggle Preview Pane on/off |
| **Ctrl+Shift+K** | Edit keyboard shortcuts |
| **Ctrl+H** | Show/hide hidden files |
| **Ctrl+1, 2, 3, 4** | Change view mode (icons, list, compact, detailed) |

### Preview Pane

- **Toggle on/off:** Alt+F3
- **Adjust width:** Ctrl+[ (shrink) / Ctrl+] (grow)
- **Toggle metadata:** Shift+Alt+F3

### Theme & Appearance

nemo-smpl integrates with your system theme:
```bash
# Use dark theme
gsettings set org.cinnamon.desktop.interface gtk-application-prefer-dark-theme true

# Reset to system default
gsettings reset org.cinnamon.desktop.interface gtk-application-prefer-dark-theme
```

---

## 🚀 Building & Contributing

### Development Build

```bash
git clone https://github.com/KonTy/nemo.git
cd nemo

# Configure with debug symbols
meson build -Dbuildtype=debug -Dsmplos_branding=true
ninja -C build

# Run directly (no install needed)
./build/src/nemo --help
./build/src/nemo ~/  # Open home directory
```

### Code Style

nemo-smpl follows GNOME coding standards:
- 2-space indentation (not tabs)
- CamelCase for types, snake_case for variables/functions
- Use GObject patterns for classes
- Write comments for non-obvious logic

### Testing Changes

```bash
# Build
meson compile -C build

# Run
./build/src/nemo /path/to/test/directory

# Check for errors
ninja -C build test
```

---

## 📚 Additional Resources

- **Bug Reports:** [GitHub Issues](https://github.com/KonTy/nemo/issues)
- **Feature Requests:** [GitHub Discussions](https://github.com/KonTy/nemo/discussions)
- **Original Nemo:** [linuxmint/nemo](https://github.com/linuxmint/nemo)
- **smplos Distribution:** [GitHub](https://github.com/KonTy/smplos)
