#!/usr/bin/env bash
# Builds the Linux x86_64 release of Zombiewood: configures + builds the linux-release preset, runs ctest, and
# produces
#   Zombiewood-Linux-x86_64-<version>.AppImage   (+ .sha256)
#   Zombiewood-Linux-x86_64-<version>.tar.gz     (+ .sha256)   portable folder: data lives next to the binary
#
# Same rules as tools/package.ps1: the packages NEVER contain game data (the script fails if it finds any), and
# they ship LICENSE.txt, licenses/, readme.txt and README.md. SDL2 and its dependencies are bundled; the system
# OpenGL ES / EGL (Mesa or the vendor driver), glibc and the X11/Wayland base libraries are NOT.
#
# Usage: tools/package-linux.sh [--clean] [--skip-build] [--skip-tests] [--dist-dir DIR] [--build-dir DIR]
#   --clean       delete the build directory first (true from-scratch build; needs network for FetchContent)
#   --skip-build  package what is already in the build directory
#   --skip-tests  do not run ctest
# Defaults: build dir = $HOME/zw/build/linux-release (the preset's binaryDir), dist dir = $HOME/zw/dist.
# Requirements: see docs/BUILD.md (Linux). linuxdeploy is downloaded once into <dist>/tools/.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$HOME/zw/build/linux-release"
dist_dir="$HOME/zw/dist"
clean=0; skip_build=0; skip_tests=0
while [ $# -gt 0 ]; do
  case "$1" in
    --clean) clean=1 ;;
    --skip-build) skip_build=1 ;;
    --skip-tests) skip_tests=1 ;;
    --dist-dir) dist_dir="$2"; shift ;;
    --build-dir) build_dir="$2"; shift ;;
    -h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

# Version from CMakeLists (project(zombiewood VERSION x.y.z)), like package.ps1.
version="$(tr -d '\r' < "$root/CMakeLists.txt" | sed -n 's/^project(zombiewood VERSION \([0-9.]*\).*/\1/p')"
[ -n "$version" ] || { echo "cannot read the version from CMakeLists.txt" >&2; exit 1; }
name="Zombiewood-Linux-x86_64"
appimage="$dist_dir/$name-$version.AppImage"
tarball="$dist_dir/$name-$version.tar.gz"
appdir="$dist_dir/AppDir"
stage="$dist_dir/$name"
pkg_src="$root/tools/package"

# ---- 1. Build ----
if [ "$skip_build" -eq 0 ]; then
  if [ "$clean" -eq 1 ] && [ -d "$build_dir" ]; then
    echo "== Deleting $build_dir"
    rm -rf "$build_dir"
  fi
  echo "== Configuring (preset linux-release)"
  (cd "$root" && cmake --preset linux-release -B "$build_dir" -DZW_STATIC_CXX_RUNTIME=ON)
  echo "== Building"
  cmake --build "$build_dir"
  if [ "$skip_tests" -eq 0 ]; then
    echo "== Tests (ctest)"
    ctest --test-dir "$build_dir" --output-on-failure
  fi
fi
exe="$build_dir/src/frontend/zombiewood"
icon="$build_dir/src/frontend/zombiewood.png"
[ -x "$exe" ] || { echo "missing $exe (build first)" >&2; exit 1; }
[ -f "$icon" ] || { echo "missing $icon (build first)" >&2; exit 1; }

# ---- 2. Tools ----
mkdir -p "$dist_dir/tools"
linuxdeploy="$dist_dir/tools/linuxdeploy-x86_64.AppImage"
if [ ! -x "$linuxdeploy" ]; then
  echo "== Downloading linuxdeploy"
  curl -fL --retry 3 -o "$linuxdeploy.part" \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
  mv "$linuxdeploy.part" "$linuxdeploy"
  chmod +x "$linuxdeploy"
fi
export APPIMAGE_EXTRACT_AND_RUN=1   # works without FUSE (containers, WSL)
# linuxdeploy aborts if PATH has entries it cannot stat (under WSL the inherited Windows PATH has some):
# keep only the directories that are really accessible.
clean_path=""
IFS=':' read -r -a _path_entries <<< "$PATH"
for p in "${_path_entries[@]}"; do
  case "$p" in /mnt/[a-z]/*) continue ;; esac
  if [ -d "$p" ] && [ -r "$p" ] && [ -x "$p" ]; then clean_path="${clean_path:+$clean_path:}$p"; fi
done
export PATH="$clean_path"

# Refuse to wipe a staging folder someone played from (like package.ps1).
for d in "$stage" "$appdir"; do
  for u in data saves saves_backup zombiewood.json; do
    if [ -e "$d/$u" ] || [ -e "$d/usr/bin/$u" ]; then
      echo "$d contains user data ($u). Move it elsewhere or delete it by hand; it will not be overwritten." >&2
      exit 1
    fi
  done
done
rm -rf "$appdir" "$stage" "$appimage" "$tarball" "$appimage.sha256" "$tarball.sha256"

# ---- 3. Documentation and licenses shared by both packages ----
docs="$dist_dir/docs-stage"
rm -rf "$docs"; mkdir -p "$docs/licenses"
sed "s/@VERSION@/$version/g" "$pkg_src/readme-linux.txt" > "$docs/readme.txt"
cp "$root/README.md" "$docs/README.md"
cp "$root/docs/LINUX.md" "$docs/LINUX.md"
cp "$root/LICENSE" "$docs/LICENSE.txt"
copy_license() {  # <name> <file...>: first existing file wins
  local n="$1"; shift
  for f in "$@"; do
    if [ -f "$f" ]; then cp "$f" "$docs/licenses/$n.txt"; return; fi
  done
  echo "warning: no license file found for $n" >&2
}
copy_license sdl2 /usr/share/doc/libsdl2-2.0-0/copyright /usr/share/licenses/SDL2/LICENSE.txt /usr/share/licenses/sdl2/LICENSE.txt
copy_license zlib /usr/share/doc/zlib1g/copyright /usr/share/licenses/zlib/LICENSE
copy_license nlohmann-json /usr/share/doc/nlohmann-json3-dev/copyright /usr/share/licenses/nlohmann-json/LICENSE.MIT
copy_license boost /usr/share/doc/libboost-dev/copyright /usr/share/doc/libboost1.83-dev/copyright /usr/share/licenses/boost/LICENSE_1_0.txt
copy_license imgui "$build_dir/_deps/imgui_src-src/LICENSE.txt"
copy_license dynarmic "$build_dir/_deps/dynarmic-src/LICENSE.txt" "$build_dir/_deps/dynarmic-src/LICENSE"
echo 'SDL_GameControllerDB (mdqinc) - zlib license: https://github.com/mdqinc/SDL_GameControllerDB/blob/master/LICENSE' \
  > "$docs/licenses/gamecontrollerdb.txt"

# ---- 4. AppDir + AppImage (linuxdeploy bundles SDL2 and its dependencies; GL/EGL/glibc stay on the host) ----
echo "== Building the AppDir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/doc/zombiewood"
cat > "$dist_dir/zombiewood.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Zombiewood
Comment=Native port runtime for Zombiewood 1.5.3 (bring your own APK)
Exec=zombiewood
Icon=zombiewood
Categories=Game;ActionGame;
Terminal=false
StartupWMClass=zombiewood
EOF
# libGLESv2/libEGL/libGLdispatch (libglvnd) must come from the host, next to its GPU driver: never bundle them.
# Both linuxdeploy runs need the excludes: the --output run deploys dependencies again.
ld_excludes=(--exclude-library 'libGLESv2*' --exclude-library 'libEGL*' --exclude-library 'libGLdispatch*'
             --exclude-library 'libGL.so*' --exclude-library 'libOpenGL*')
"$linuxdeploy" --appdir "$appdir" -e "$exe" -d "$dist_dir/zombiewood.desktop" -i "$icon" \
  "${ld_excludes[@]}" > "$dist_dir/linuxdeploy.log" 2>&1 \
  || { tail -30 "$dist_dir/linuxdeploy.log" >&2; exit 1; }
cp "$pkg_src/gamecontrollerdb-linux.txt" "$appdir/usr/bin/gamecontrollerdb.txt"
cp -r "$docs/." "$appdir/usr/share/doc/zombiewood/"

check_no_game_data() {  # never ship game files
  local bad
  bad="$(find "$1" -type f \( -iname '*.apk' -o -iname '*.obb' -o -iname '*.mp4' -o -iname '*.bar' -o -iname '*.dex' \
          -o -iname 'libZombieWood.so' -o -iname 'libgenerator.so' \) -print)"
  # Shared objects are only allowed in the bundled-library folder.
  bad="$bad$(find "$1" -type f -name '*.so' ! -path '*/lib/*' -print)"
  if [ -n "$bad" ]; then echo "The package contains game data or stray libraries:" >&2; echo "$bad" >&2; exit 1; fi
}
check_no_game_data "$appdir"

echo "== Building the AppImage"
(cd "$dist_dir" && LINUXDEPLOY_OUTPUT_VERSION="$version" LDAI_OUTPUT="$appimage" OUTPUT="$appimage" \
   "$linuxdeploy" --appdir "$appdir" "${ld_excludes[@]}" --output appimage >> "$dist_dir/linuxdeploy.log" 2>&1) \
  || { tail -30 "$dist_dir/linuxdeploy.log" >&2; exit 1; }
[ -f "$appimage" ] || { echo "linuxdeploy did not produce $appimage (see $dist_dir/linuxdeploy.log)" >&2; exit 1; }
chmod +x "$appimage"
# The build fails if a system library that must stay on the host ended up in the bundle.
host_only="$(find "$appdir/usr/lib" -maxdepth 1 \( -name 'libGLESv2.so*' -o -name 'libEGL.so*' -o -name 'libGLdispatch.so*' \
              -o -name 'libGL.so*' -o -name 'libOpenGL.so*' -o -name 'libc.so*' -o -name 'libm.so*' -o -name 'libstdc++.so*' \) -print)"
if [ -n "$host_only" ]; then
  rm -f "$appimage"
  echo "The AppImage bundles system GL/EGL or libc libraries; they must stay on the host:" >&2; echo "$host_only" >&2; exit 1
fi

# ---- 5. Portable folder + tar.gz (binary with RUNPATH $ORIGIN/lib, same bundled libraries) ----
echo "== Building $stage"
mkdir -p "$stage/lib"
cp "$exe" "$stage/zombiewood"
strip "$stage/zombiewood"   # like linuxdeploy does; the dynamic symbols (-rdynamic) used by the crash report remain
cp -a "$appdir/usr/lib/." "$stage/lib/"
cp "$pkg_src/gamecontrollerdb-linux.txt" "$stage/gamecontrollerdb.txt"
cp "$icon" "$stage/zombiewood.png"
cp -r "$docs/." "$stage/"
# Licenses of the bundled libraries, as collected by linuxdeploy from the distribution.
if [ -d "$appdir/usr/share/doc" ]; then
  mkdir -p "$stage/licenses/bundled-libs"
  for d in "$appdir/usr/share/doc"/*/; do
    n="$(basename "$d")"
    [ "$n" = zombiewood ] && continue
    [ -f "$d/copyright" ] && cp "$d/copyright" "$stage/licenses/bundled-libs/$n.txt"
  done
fi
check_no_game_data "$stage"
tar -C "$dist_dir" --owner=0 --group=0 -czf "$tarball" "$name"

# ---- 6. Checksums and summary ----
for f in "$appimage" "$tarball"; do
  (cd "$dist_dir" && sha256sum "$(basename "$f")" > "$f.sha256")
done
rm -rf "$docs" "$dist_dir/zombiewood.desktop"

echo
echo "Folder   : $stage"
(cd "$stage" && find . -type f -printf '  %10s  %P\n' | sort -k2)
for f in "$appimage" "$tarball"; do
  printf '%-9s: %s  (%s bytes)\n' "Package" "$f" "$(stat -c %s "$f")"
  printf '%-9s: %s\n' "SHA-256" "$(cut -d' ' -f1 "$f.sha256")"
done
