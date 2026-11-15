#!/bin/sh
set -eu

if [ "$#" -ne 1 ] || [ -e "$1" ]; then
    echo "Usage: build-projectm-packages.sh NEW_OUTPUT_DIRECTORY" >&2
    exit 1
fi

version=4.1.8
revision=4.1.8-1
arch=$(dpkg --print-architecture)
multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
mkdir -p "$1"
work=$(realpath "$1")
archive="$work/libprojectM-$version.tar.gz"
source="$work/libprojectM-$version"
runtime="$work/runtime"
devel="$work/devel"

curl --fail --location --silent --show-error \
    "https://github.com/projectM-visualizer/projectm/releases/download/v$version/libprojectM-$version.tar.gz" \
    --output "$archive"
echo "f72135e8b8e5fd807f908537255b41ac7ba4830309265e830cb756535487239e  $archive" | sha256sum --check
tar --extract --gzip --file "$archive" --directory "$work" --no-same-owner
cmake -S "$source" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    "-DCMAKE_INSTALL_LIBDIR=lib/$multiarch" \
    "-DCMAKE_C_FLAGS=-ffile-prefix-map=$work=." \
    "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=$work=." \
    -DBUILD_SHARED_LIBS=ON -DENABLE_PLAYLIST=OFF -DENABLE_SDL_UI=OFF \
    -DENABLE_SYSTEM_PROJECTM_EVAL=OFF -DBUILD_TESTING=OFF
cmake --build "$work/build" --parallel 2
DESTDIR="$runtime" cmake --install "$work/build" --component Runtime --strip
DESTDIR="$devel" cmake --install "$work/build" --component Devel
mkdir -p "$runtime/DEBIAN" "$devel/DEBIAN" "$devel/usr/lib/$multiarch" \
    "$runtime/usr/share/doc/libprojectm4/licenses" "$devel/usr/share/doc/libprojectm4-dev"
mv "$runtime/usr/lib/$multiarch/libprojectM-4.so" "$devel/usr/lib/$multiarch/"
cp "$source/LICENSE.txt" "$runtime/usr/share/doc/libprojectm4/copyright"
(
    cd "$source"
    find . -type f \( -iname 'license*' -o -iname 'copying*' \) \
        -exec cp --parents '{}' "$runtime/usr/share/doc/libprojectm4/licenses/" \;
)
ln -s ../libprojectm4/copyright "$devel/usr/share/doc/libprojectm4-dev/copyright"
dependencies=$(dpkg-shlibdeps -O -e "$runtime/usr/lib/$multiarch/libprojectM-4.so.$version")
cat > "$runtime/DEBIAN/control" <<EOF
Package: libprojectm4
Version: $revision
Architecture: $arch
Multi-Arch: same
Maintainer: smpl-os <smpl-os@users.noreply.github.com>
Section: libs
Priority: optional
Depends: ${dependencies#shlibs:Depends=}
Homepage: https://github.com/projectM-visualizer/projectm
Description: projectM 4 MilkDrop visualization engine for Nemo
 Unmodified upstream projectM $version, built from the checksum-pinned
 release source distributed alongside these packages.
EOF
echo "libprojectM-4 4 libprojectm4 (>= $revision)" > "$runtime/DEBIAN/shlibs"
echo "activate-noawait ldconfig" > "$runtime/DEBIAN/triggers"
cat > "$devel/DEBIAN/control" <<EOF
Package: libprojectm4-dev
Version: $revision
Architecture: $arch
Multi-Arch: same
Maintainer: smpl-os <smpl-os@users.noreply.github.com>
Section: libdevel
Priority: optional
Depends: libprojectm4 (= $revision), libopengl-dev
Homepage: https://github.com/projectM-visualizer/projectm
Description: projectM 4 development files for Nemo
 Headers and pkg-config/CMake metadata for the matching visualization engine.
EOF
dpkg-deb --build --root-owner-group "$runtime" "$work/libprojectm4_${revision}_${arch}.deb"
dpkg-deb --build --root-owner-group "$devel" "$work/libprojectm4-dev_${revision}_${arch}.deb"
