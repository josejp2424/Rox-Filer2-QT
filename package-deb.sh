#!/usr/bin/env bash
# Generate a native Debian package from the CMake install manifest.
# No sudo, fakeroot or network; staged ownership is set by dpkg-deb.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${1:-$ROOT/build-qt}"
[[ "$BUILD" = /* ]] || BUILD="$PWD/$BUILD"
BUILD="$(cd -- "$BUILD" && pwd)"
[[ -f "$BUILD/CMakeCache.txt" ]] || { echo "Falta CMakeCache.txt en $BUILD" >&2; exit 1; }
command -v cmake >/dev/null || { echo 'Falta cmake.' >&2; exit 1; }
command -v dpkg-deb >/dev/null || { echo 'Falta dpkg-deb.' >&2; exit 1; }

ver="$(sed -n 's/^RoxFiler2QT_VERSION:STATIC=//p' "$BUILD/CMakeCache.txt" | head -1)"
# CMake may not cache PROJECT_VERSION under all versions: source is authoritative.
ver="${ver:-$(sed -n 's/.*project(RoxFiler2QT VERSION \([0-9.]*\).*/\1/p' "$ROOT/qt/CMakeLists.txt" | head -1)}"
[[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Versión inválida.' >&2; exit 1; }
arch="$(dpkg --print-architecture 2>/dev/null || true)"
[[ -n "$arch" ]] || { echo 'No se pudo determinar la arquitectura Debian.' >&2; exit 1; }
mkdir -p "$ROOT/output"
work="$(mktemp -d "${TMPDIR:-/tmp}/rox-filer2-qt-deb.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT
stage="$work/stage"
mkdir -p "$stage" "$work/debian"
DESTDIR="$stage" cmake --install "$BUILD" --prefix /usr

# A build may have an absolute install() DESTINATION: DESTDIR stages those files.
for path in "$stage/usr/bin/rox-filer2-qt" "$stage/usr/bin/rox-qt-drives" \
            "$stage/usr/bin/roxfiler-qt-root" "$stage/usr/lib/rox-filer2-qt/rox-qt-root-launcher"; do
    [[ -f "$path" ]] || { echo "Empaquetado incompleto: $path" >&2; exit 1; }
done
[[ -d "$stage/usr/share/rox-filer2-qt/translations" ]] || { echo 'Faltan traducciones en el paquete.' >&2; exit 1; }
count="$(find "$stage/usr/share/rox-filer2-qt/translations" -maxdepth 1 -name '*.qm' | wc -l)"
[[ "$count" -eq 11 ]] || { echo "Se esperaban 11 catálogos .qm: hay $count" >&2; exit 1; }

# Capture ELF dependencies: this uses the distro's shlibs metadata when present.
# For custom Qt installations not recorded by dpkg, --ignore-missing-info
# skips *only* unregistered private libs; they must exist on target at runtime.
qt_dir="$(sed -n 's/^Qt6_DIR:PATH=//p' "$BUILD/CMakeCache.txt" | head -1)"
private_qt=0
[[ "$qt_dir" == /opt/essora/qt6/* ]] && private_qt=1
cat > "$work/debian/control" <<EOF
Source: rox-filer2-qt
Section: utils
Priority: optional
Maintainer: Rox-Filer2-QT contributors
Standards-Version: 4.6.2

Package: rox-filer2-qt
Architecture: $arch
Description: Standalone Qt6 Rox-Filer2 file manager
EOF
elfs=()
for path in "$stage/usr/bin/rox-filer2-qt" "$stage/usr/bin/rox-qt-drives" "$stage/usr/lib/rox-filer2-qt/rox-qt-mount-helper" "$stage/usr/lib/rox-filer2-qt/rox-qt-root-launcher" "$stage/usr/lib/rox-filer2-qt/rox-qt-smb-helper"; do
    [[ -x "$path" ]] && elfs+=("$path")
done
[[ "${#elfs[@]}" -gt 0 ]] || { echo 'Sin binarios ELF.' >&2; exit 1; }
depends=''
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
    # dpkg-shlibdeps needs a Debian control file to determine the package name.
    opts=(-O --ignore-missing-info)
    if [[ $private_qt -eq 1 ]]; then
        for lib in /opt/essora/qt6/lib /opt/essora/qt6/lib64 /opt/essora/qt6/lib/x86_64-linux-gnu; do
            [[ -d "$lib" ]] && opts+=("-l$lib")
        done
    fi
    if out="$(cd "$work" && dpkg-shlibdeps "${opts[@]}" "${elfs[@]}")"; then
        depends="$(printf '%s\n' "$out" | sed -n 's/^shlibs:Depends=//p' | tail -1)"
    elif [[ $private_qt -eq 0 ]]; then
        echo 'No pude calcular dependencias del Qt del sistema; no genero un DEB incorrecto.' >&2
        exit 1
    else
        echo 'Aviso: no se pudieron resolver todas las librerías del Qt privado.' >&2
    fi
fi
if [[ -z "$depends" ]]; then
    if [[ $private_qt -eq 0 ]]; then
        echo 'No fue posible determinar dependencias dinámicas del Qt del sistema.' >&2
        exit 1
    fi
    # Qt privado is external; minimal distro runtime libs are required.
    depends='libc6, libstdc++6, libgcc-s1'
fi
if [[ $private_qt -eq 1 ]]; then
    echo 'AVISO: este .deb exige también el Qt privado de /opt/essora/qt6 en el equipo de destino.'
fi
install -d -m 0755 "$stage/usr/share/doc/rox-filer2-qt" "$stage/DEBIAN"
install -m 0644 "$ROOT/LICENSE" "$stage/usr/share/doc/rox-filer2-qt/copyright"
install -m 0644 "$ROOT/README.md" "$stage/usr/share/doc/rox-filer2-qt/README"
size="$(du -sk "$stage" | awk '{print $1}')"
cat > "$stage/DEBIAN/control" <<EOF
Package: rox-filer2-qt
Version: $ver
Section: utils
Priority: optional
Architecture: $arch
Maintainer: Rox-Filer2-QT contributors
Installed-Size: $size
Depends: $depends
Recommends: udisks2, pkexec, xdg-utils, shared-mime-info
Description: Rox-Filer2 modern Qt6 file manager
 Standalone Qt6 file manager for KDE, SonicDE and other desktops.
 Includes X11 desktop drive icons and a separate authorized root launcher.
EOF
(
  cd "$stage"
  # dpkg documents checksums relative to the package filesystem root.
  find usr etc -type f ! -path '*/DEBIAN/*' -print0 2>/dev/null | sort -z | xargs -0 -r md5sum
) > "$stage/DEBIAN/md5sums"
chmod 0644 "$stage/DEBIAN/control" "$stage/DEBIAN/md5sums"
output="$ROOT/output/rox-filer2-qt_${ver}_${arch}.deb"
dpkg-deb --build --root-owner-group "$stage" "$output"
dpkg-deb --info "$output" >/dev/null
dpkg-deb --contents "$output" > "$work/package-files.txt"
grep -q '/usr/bin/rox-filer2-qt$' "$work/package-files.txt" || { echo 'El DEB no contiene el gestor.' >&2; exit 1; }
echo "DEB generado: $output"
