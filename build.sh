#!/usr/bin/env bash
# Portable CMake build: system Qt6, Essora Qt6, or another selected Qt6 SDK.
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-qt}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
QT_MODE="${QT_MODE:-auto}"             # auto | system | essora
QT_PREFIX="${QT_PREFIX:-}"             # optional explicit Qt SDK location
PACKAGE_DEB="${PACKAGE_DEB:-auto}"      # auto | 0 | 1

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
command -v cmake >/dev/null 2>&1 || fail 'Falta cmake; consultá ./dependencies.sh --check.'
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail 'JOBS debe ser un entero mayor que cero.'
[[ "$PACKAGE_DEB" == auto || "$PACKAGE_DEB" == 0 || "$PACKAGE_DEB" == 1 ]] || fail 'PACKAGE_DEB: auto, 0 o 1.'
[[ "$QT_MODE" == auto || "$QT_MODE" == system || "$QT_MODE" == essora ]] || fail 'QT_MODE: auto, system o essora.'

# An explicitly supplied QT_PREFIX always wins; QT_MODE=system conflicts with
# a private explicit prefix, so reject it rather than silently linking wrong Qt.
if [[ -n "$QT_PREFIX" && "$QT_MODE" == system && "$QT_PREFIX" != /usr ]]; then
    fail 'QT_MODE=system no puede usarse con QT_PREFIX distinto de /usr.'
fi
if [[ -n "$QT_PREFIX" && "$QT_MODE" == essora && "$QT_PREFIX" != /opt/essora/qt6 ]]; then
    fail 'QT_MODE=essora requiere QT_PREFIX=/opt/essora/qt6 o sin QT_PREFIX.'
fi
if [[ -z "$QT_PREFIX" ]]; then
    case "$QT_MODE" in
        essora) QT_PREFIX=/opt/essora/qt6 ;;
        system) QT_PREFIX=/usr ;;
        auto)
            # Keep Essora's historic default, but do not use its Qt on Void.
            if command -v xbps-install >/dev/null 2>&1; then
                QT_PREFIX=/usr
            elif [[ -f /opt/essora/qt6/lib/cmake/Qt6/Qt6Config.cmake ||
                    -f /opt/essora/qt6/lib64/cmake/Qt6/Qt6Config.cmake ]]; then
                QT_PREFIX=/opt/essora/qt6
            else
                QT_PREFIX=/usr
            fi ;;
    esac
fi

# Locate a real Qt6 CMake config, not merely an installed Qt runtime.
qt_config=''
if [[ "$QT_PREFIX" == /usr ]]; then
    for candidate in /usr/lib/cmake/Qt6/Qt6Config.cmake \
        /usr/lib64/cmake/Qt6/Qt6Config.cmake \
        /usr/lib/*/cmake/Qt6/Qt6Config.cmake \
        /usr/lib/qt6/lib/cmake/Qt6/Qt6Config.cmake; do
        if [[ -f "$candidate" ]]; then qt_config="$candidate"; break; fi
    done
else
    for candidate in "$QT_PREFIX/lib/cmake/Qt6/Qt6Config.cmake" \
        "$QT_PREFIX/lib64/cmake/Qt6/Qt6Config.cmake" \
        "$QT_PREFIX/lib/"*/cmake/Qt6/Qt6Config.cmake; do
        if [[ -f "$candidate" ]]; then qt_config="$candidate"; break; fi
    done
fi
[[ -n "$qt_config" ]] || fail "No se encontró Qt6Config.cmake en $QT_PREFIX. Instalá los paquetes Qt6 -dev/-devel (ver ./dependencies.sh --check)."
qt_dir="$(dirname "$qt_config")"

# Avoid reusing Qt6_DIR and compiler paths cached from an older SDK.
# Do not delete a directory unless it is recognized as our own build directory.
mkdir -p -- "$BUILD_DIR"
BUILD_DIR="$(cd -- "$BUILD_DIR" && pwd)"
selected="$qt_dir"
if [[ -f "$BUILD_DIR/.selected-qt" && "$(cat "$BUILD_DIR/.selected-qt")" != "$selected" ]]; then
    [[ -f "$BUILD_DIR/CMakeCache.txt" ]] || fail "Hay datos ajenos en $BUILD_DIR; no se limpiará automáticamente."
    printf 'Cambio de Qt detectado; limpiando archivos de CMake anteriores...\n'
    cmake -E rm -f "$BUILD_DIR/CMakeCache.txt"
    cmake -E rm -rf "$BUILD_DIR/CMakeFiles"
fi
printf '%s\n' "$selected" > "$BUILD_DIR/.selected-qt"
cmake_args=(-S "$ROOT/qt" -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
    -DROX_QT_PREFER_ESSORA=OFF "-DQt6_DIR=$qt_dir"
    "-DCMAKE_PREFIX_PATH=$QT_PREFIX")
if [[ "${WITH_TESTS:-0}" == 1 ]]; then cmake_args+=(-DROX_QT_BUILD_TESTS=ON); else cmake_args+=(-DROX_QT_BUILD_TESTS=OFF); fi
if [[ "${INSTALL_ROX_COMMAND:-0}" == 1 ]]; then cmake_args+=(-DROX_QT_INSTALL_ROX_COMMAND=ON); else cmake_args+=(-DROX_QT_INSTALL_ROX_COMMAND=OFF); fi
printf 'Usando Qt6: %s\n' "$qt_dir"
cmake "${cmake_args[@]}" "$@"
cmake --build "$BUILD_DIR" --parallel "$JOBS"
printf 'Binarios: %s/rox-filer2-qt y %s/rox-qt-drives\n' "$BUILD_DIR" "$BUILD_DIR"
if [[ "${WITH_TESTS:-0}" == 1 ]]; then
    QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD_DIR" --output-on-failure
fi
# Only Debian-family hosts build a .deb automatically; never require dpkg on Void.
if [[ "$PACKAGE_DEB" == 1 ]]; then
    command -v dpkg-deb >/dev/null 2>&1 || fail 'PACKAGE_DEB=1 requiere dpkg-deb.'
    "$ROOT/package-deb.sh" "$BUILD_DIR"
elif [[ "$PACKAGE_DEB" == auto && -x /usr/bin/dpkg-deb && -f /etc/debian_version ]]; then
    "$ROOT/package-deb.sh" "$BUILD_DIR"
fi
printf 'Instalación opcional (como root): cmake --install %s\n' "$BUILD_DIR"
