#!/usr/bin/env bash
# Rox-Filer2-QT: distro-specific build dependencies, device stacks and tools.
# Do not run package installation unless a mode is explicitly requested.
set -euo pipefail
usage() {
cat <<'TXT'
Rox-Filer2-QT 1.0.7 dependencies
  ./dependencies.sh --check            inspect tools, Qt6 and device stacks
  ./dependencies.sh --install-build    Qt6 toolchain + ALL available -dev libraries
  ./dependencies.sh --install-runtime  mount/network/mobile runtime tools
  ./dependencies.sh --install-full     build + development libs + runtime
  ./dependencies.sh --help

Installation modes require root. On Void use XBPS; on Debian/Devuan use APT;
on Arch use pacman. No sudo, Git or modification of Polkit rules.
Not every optional runtime is implemented in the Qt frontend yet (MTP/PTP/iOS).
TXT
}
exists() { command -v "$1" >/dev/null 2>&1; }
check_cmd() { if exists "$1"; then printf '  %-20s OK\n' "$1"; else printf '  %-20s not found\n' "$1"; fi; }
check() {
  echo '=== Compilers / Qt / packaging ==='
  local x
  for x in cmake c++ cc ninja make pkg-config lrelease lrelease-qt6 dpkg-deb xbps-install; do check_cmd "$x"; done
  for x in Qt6Core Qt6Gui Qt6Widgets Qt6DBus Qt6Svg libmtp libgphoto2 libimobiledevice smbclient fuse3 fuse; do
    if exists pkg-config && pkg-config --exists "$x" 2>/dev/null; then
      printf '  %-20s pkg-config %s\n' "$x" "$(pkg-config --modversion "$x")"
    else
      printf '  %-20s not in pkg-config search path (may be optional)\n' "$x"
    fi
  done
  echo '=== Drives, SMB, cameras, Android and iOS ==='
  for x in udisksctl lsblk mount umount pkexec dbus-send smbclient mount.cifs net \
      fusermount fusermount3 jmtpfs simple-mtpfs mtp-detect gphoto2 gphotofs \
      idevice_id ifuse usbmuxd ffmpegthumbnailer pdftoppm; do check_cmd "$x"; done
  echo '=== Qt SDK paths ==='
  for x in /opt/essora/qt6/lib/cmake/Qt6/Qt6Config.cmake \
      /opt/essora/qt6/lib64/cmake/Qt6/Qt6Config.cmake \
      /usr/lib/cmake/Qt6/Qt6Config.cmake /usr/lib64/cmake/Qt6/Qt6Config.cmake \
      /usr/lib/*/cmake/Qt6/Qt6Config.cmake; do
    [[ -f "$x" ]] && echo "  $x"
  done
  echo '=== Desktop ==='
  printf '  DISPLAY=%s WAYLAND_DISPLAY=%s\n' "${DISPLAY:-}" "${WAYLAND_DISPLAY:-}"
}
MODE="${1:---check}"
case "$MODE" in
    --help|-h) usage; exit 0 ;;
    --check) check; exit 0 ;;
    --install-build|--install-runtime|--install-full) ;;
    *) usage; exit 2 ;;
esac
[[ $# -eq 1 ]] || { usage; exit 2; }
[[ $EUID -eq 0 ]] || { echo 'ERROR: execute --install-* as root.' >&2; exit 1; }

if exists xbps-install; then distro=void
elif exists apt-get; then distro=debian
elif exists pacman; then distro=arch
else echo 'Unsupported package manager (XBPS/APT/pacman required).' >&2; exit 2; fi

# Mandatory packages: if any is unavailable, abort rather than suggesting
# compilation could succeed. Optional libs/tools are individually checked and
# omitted with explicit warnings when a repository does not provide them.
case "$distro" in
  debian)
    build=(build-essential cmake ninja-build pkg-config qt6-base-dev qt6-base-dev-tools qt6-tools-dev-tools qt6-l10n-tools)
    devel=(libqt6svg6-dev libmtp-dev libgphoto2-dev libimobiledevice-dev libsmbclient-dev
           libfuse3-dev libfuse-dev libusb-1.0-0-dev libplist-dev libusbmuxd-dev
           libglib2.0-dev libudev-dev libarchive-dev libmagic-dev libdbus-1-dev)
    runtime_core=(udisks2 dbus polkitd pkexec util-linux shared-mime-info xdg-utils desktop-file-utils)
    runtime_extra=(cifs-utils smbclient samba-common-bin samba fuse fuse3 libmtp-runtime
                   jmtpfs simple-mtpfs gphoto2 gphotofs ifuse libimobiledevice-utils
                   usbmuxd gvfs-backends gvfs-fuse qt6-image-formats-plugins
                   ffmpegthumbnailer poppler-utils avahi-daemon eject exfatprogs
                   ntfs-3g squashfs-tools squashfuse)
    ;;
  void)
    # Void names are NOT the Debian -dev names. libsmbclient headers come
    # from samba-devel, not a libsmbclient-dev package.
    build=(base-devel cmake ninja pkg-config qt6-base-devel qt6-tools qt6-tools-devel)
    devel=(qt6-svg-devel libmtp-devel libgphoto2-devel libimobiledevice-devel
           samba-devel fuse-devel fuse3-devel libusb-devel libplist-devel
           libusbmuxd-devel glib-devel eudev-libudev-devel libarchive-devel
           file-devel dbus-devel)
    runtime_core=(udisks2 dbus polkit util-linux shared-mime-info xdg-utils desktop-file-utils)
    runtime_extra=(samba cifs-utils fuse fuse3 libmtp jmtpfs simple-mtpfs
                   gphoto2 gphotofs ifuse libimobiledevice usbmuxd gvfs
                   qt6-imageformats qt6-wayland ffmpegthumbnailer poppler-utils
                   avahi eject exfatprogs ntfs-3g squashfs-tools squashfuse)
    ;;
  arch)
    build=(base-devel cmake ninja pkgconf qt6-base qt6-tools)
    devel=(qt6-svg libmtp libgphoto2 libimobiledevice smbclient fuse2 fuse3
           libusb libplist libusbmuxd glib2 libarchive file dbus)
    runtime_core=(udisks2 dbus polkit util-linux shared-mime-info xdg-utils desktop-file-utils)
    runtime_extra=(samba cifs-utils fuse2 fuse3 libmtp jmtpfs simple-mtpfs
                   gphoto2 gphotofs ifuse libimobiledevice usbmuxd gvfs
                   gvfs-mtp gvfs-gphoto2 qt6-imageformats qt6-wayland
                   ffmpegthumbnailer poppler avahi eject exfatprogs ntfs-3g squashfs-tools)
    ;;
esac

available() {
  local name="$1"
  case "$distro" in
    void)  xbps-query -R "$name" >/dev/null 2>&1 ;;
    debian) apt-cache show "$name" >/dev/null 2>&1 ;;
    arch) pacman -Si "$name" >/dev/null 2>&1 ;;
  esac
}
missing_optional=()
install_group() {
  local tag="$1" mandatory="$2"; shift 2
  local pkg
  local candidates=()
  for pkg in "$@"; do
    if available "$pkg"; then
      candidates+=("$pkg")
    elif [[ "$mandatory" == yes ]]; then
      echo "ERROR: missing required package '$pkg' ($distro/$tag); refresh your repo indexes." >&2
      exit 1
    else
      echo "WARNING: optional package not found in repositories: $pkg ($tag)" >&2
      missing_optional+=("$pkg")
    fi
  done
  if ((${#candidates[@]})); then
    echo "=== Installing $tag packages ($distro) ==="
    case "$distro" in
      void) xbps-install -y "${candidates[@]}" ;;
      debian) apt-get install -y --no-install-recommends "${candidates[@]}" ;;
      arch) pacman -S --needed --noconfirm "${candidates[@]}" ;;
    esac
  fi
}
if [[ "$MODE" != --install-runtime ]]; then
    install_group build yes "${build[@]}"
    install_group dev-libraries no "${devel[@]}"
fi
if [[ "$MODE" != --install-build ]]; then
    install_group runtime-base yes "${runtime_core[@]}"
    install_group runtime-optional no "${runtime_extra[@]}"
fi
if ((${#missing_optional[@]})); then
    printf '\nOptional packages not available (%s):\n' "$distro"
    printf '  %s\n' "${missing_optional[@]}"
fi
printf '\nDependencies step finished. Verify with ./dependencies.sh --check.\n'
