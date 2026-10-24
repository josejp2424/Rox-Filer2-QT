# Changelog

## 1.0.7 — 2026-10-24 — Initial public release

First public source release of **Rox-Filer2-QT**, a standalone, modern Qt6
file manager based on the ROX-Filer2 lineage and developed for the Essora
project. This is the **first published changelog entry** for the Qt edition.

### Included in this release

- Qt6/C++20 browser with tabs, navigation, file search/selection and
  file-management actions; no GTK3 ROX frontend is compiled or overwritten.
- Copy/move progress with destination write tracking and final filesystem
  flushing before the transfer is reported complete.
- Image mounting for ISO, SFS/SquashFS and disk IMG formats; managed
  mounts can be displayed and unmounted through the interface.
- X11/XLibre desktop drive icons, with positioning and visibility preferences
  for KDE/SonicDE and other compatible desktops.
- Native UDisks2/DBus drive mounting and restricted Polkit helpers for
  authorized operations; Samba/CIFS share connection and user-share tools.
- Root launcher that carries over a sanitized snapshot of the user's Qt
  palette, style, font and icon theme.
- Eleven Qt translation catalogs, documentation, desktop launchers and icons.
- Portable CMake build with support for distribution Qt6 and the optional
  Essora Qt6 SDK under `/opt/essora/qt6`.
- Void Linux build wrapper (`build-void.sh`) and optional dependency installer
  for Debian/Devuan, Void and Arch; Debian `.deb` packaging when available.

### Known limitations

- Android MTP, camera PTP and Apple iOS handling are not yet fully integrated
  into the Qt UI. Installing their tools and development libraries does not
  add unimplemented features.
- Desktop drive icons require X11/XLibre; this release does not implement a
  Wayland desktop layer or the GTK3 ROX PuppyPin desktop.
- SVG/thumbnail support depends on Qt image plugins and available helpers.
- Some translated strings may fall back to English.

**Build requirement:** Qt6 >= 6.4 and its LinguistTools/lrelease. Testing on
hardware and multiple distributions remains necessary before deploying this
file manager for critical file operations.
