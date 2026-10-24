#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
bash -n build.sh build-void.sh package-deb.sh dependencies.sh qt/data/roxfiler-qt-root qt/data/qt-polkit-session qt/data/rox-qt
python3 - <<'PY'
from pathlib import Path
import xml.etree.ElementTree as ET
r=Path('qt')
assert not Path('ROX-Filer/src').exists(), 'unexpected GTK3 tree'
assert Path('CHANGELOG.md').exists() and 'Initial public release' in Path('CHANGELOG.md').read_text(), 'first-release changelog missing'
assert Path('build-void.sh').exists() and Path('.gitignore').exists(), 'GitHub/Void support missing'
assert len(list((r/'translations').glob('rox-filer2-qt_*.ts'))) == 11, 'translation count'
for f in (r/'translations').glob('*.ts'):ET.parse(f)
for f in (r/'data/polkit').glob('*.policy'):ET.parse(f)
for e in ET.parse(r/'data/rox-qt.qrc').iter('file'):
    assert (r/'data'/e.text).is_file(), 'missing qrc file: '+e.text
s=(r/'src/main.cpp').read_text()
assert '--desktop' in s and 'rox-qt-drives' in s
assert 'RoxRootPalette::applyChrome' in s
assert (r/'src/rootpalette.h').exists()
s=(r/'src/qt_drives.cpp').read_text()
assert 'm_force' in s and 'DesktopDriveController controller(force)' in s
s=(r/'CMakeLists.txt').read_text()
assert 'find_package(Qt6 6.4 REQUIRED' in s
assert 'VERSION 1.0.7' in s
assert 'src/rootpalette.h' in s
assert 'package-deb.sh' in Path('build.sh').read_text()
fileops=(r/'src/fileops.cpp').read_text()
filepane=(r/'src/filepane.cpp').read_text()
# 1.0.5: image mounting is a port of GTK3 image_mounter.c (imagemounter.cpp).
mounter=(r/'src/imagemounter.cpp').read_text()
assert 'namespace ImageMounter' not in fileops.replace('moved to imagemounter.cpp', '')
assert 'isPartitionableRaw(job.image)' in mounter, 'ISO/SFS must mount the whole loop device'
assert 'statePathForImage' in mounter and 'QCryptographicHash::Sha256' in mounter, 'GTK3 state files'
assert '"ro,nosuid,nodev"' in mounter and 'kHelperDirs' in mounter
assert 'readProcMounts' in mounter and 'atEnd()' not in mounter, 'procfs must be read with readAll()'
assert 'atEnd()' not in (r/'src/imagevolumes.cpp').read_text()
assert 'ImageMounter::managedMounts()' in (r/'src/mainwindow.cpp').read_text()
assert 'ImageMounter::managedMounts()' in (r/'src/qt_drives.cpp').read_text()
assert 'NotAuthorizedDismissed' in (r/'src/drivemount.cpp').read_text()
assert 'Retry with administrator authentication?' not in (r/'src/drivemount.cpp').read_text()
assert 'ImageMounter::canHandle(path)' in filepane and 'requestNewTab(mountPoint)' in filepane
# 1.0.6: like GTK3 action.c, a destination without Unix owners (FAT, exFAT,
# NTFS, SMB, FUSE) must not turn a good copy into an error when run as root.
assert 'destinationLacksOwnership(destDir)' in fileops
assert '--no-preserve=ownership' in fileops and '0x2011BAB0UL' in fileops and '0x4d44UL' in fileops
# 1.0.7: copy/move progress follows bytes confirmed on the device, and the
# job ends only after the destination filesystem has been flushed.
tp=(r/'src/transferprogress.cpp').read_text()
assert 'sync_file_range' in tp and 'syncfs' in tp and '/sys/dev/block/' in tp
assert 'FileJob::runTracked' in fileops and 'new TransferProgress(tracked)' in fileops
assert 'm_tracker->commandsFinished()' in fileops
cm=(r/'CMakeLists.txt').read_text()
assert cm.count('src/transferprogress.cpp') >= 5 and 'transfer_progress' in cm
for ts in (r/'translations').glob('*.ts'):
    t=ts.read_text(encoding='utf-8')
    for msg in ('Calculating size…', '%1 of %2', 'Remaining: %1',
                'Writing to the device. Do not remove it until this window closes.'):
        assert f'<source>{msg}</source>' in t, (ts.name, msg)

assert 'Qt6 6.11.2 EXACT' not in s
for term in ['Qt6::Core','Qt6::Gui','Qt6::Widgets','Qt6::DBus']:
    assert term in s
print('PASS: Qt-only source, 11 TS catalogs, Polkit XML, resources, desktop mode, Qt portability')
PY
for src in qt/src/rox_qt_mount_helper.c qt/src/rox_qt_root_launcher.c qt/src/rox_qt_smb_helper.c; do
   cc -std=c11 -Wall -Wextra -Wpedantic -fsyntax-only "$src"
done
echo 'PASS: all 3 native helpers parse cleanly.'
