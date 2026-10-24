#pragma once

/*
 * ROX-Filer2-QT - ROX "run item" semantics, ported from the GTK3 build
 * (diritem.c / run.c) without any GTK or GIO dependency.
 *
 * What the GTK3 filer does when you click an item is NOT "open with the MIME
 * default application":
 *   - a directory holding an executable AppRun is an application and RUNS;
 *   - a regular file with an execute bit whose MIME type derives from
 *     application/x-executable (scripts, binaries) RUNS, in the current folder;
 *   - a .desktop file launches the application it describes;
 *   - a directory that is listed in /etc/fstab (or is a mount point) is a
 *     "mount point": opening it mounts it first; Shift unmounts it;
 *   - everything else opens in the default application for its MIME type, and
 *     Shift opens it as text.
 * This module holds the detection rules and the process spawning; FilePane
 * decides what to do with the result.
 */

#include <QString>

namespace RoxOpen {

/* Same rule as diritem.c examine_dir(): the directory is not world-writable,
 * AppRun exists, belongs to the same owner as the directory and has an execute
 * bit. Mount points are never applications. */
bool isAppDir(const QString &path);

/* Same rule as run.c EXECUTABLE_FILE(): execute bit + executable MIME type
 * (ELF, shell, python, perl, ...), or a .desktop launcher. */
bool isExecutableFile(const QString &path);
bool isDesktopFile(const QString &path);

/* Mount points: mounted now (from /proc/self/mountinfo) and/or listed in
 * /etc/fstab. "/" is never reported. */
bool isMounted(const QString &path);
bool isFstabMountPoint(const QString &path);

/* Spawns detached. On failure returns false and fills *error. */
bool runAppDir(const QString &path, QString *error);
bool runExecutable(const QString &path, const QString &workingDir, QString *error);
bool runDesktopFile(const QString &path, const QString &workingDir, QString *error);

/* Exposed for tests: parse the escaped path fields of mountinfo/fstab. */
QString unescapeMountField(const QString &field);

} // namespace RoxOpen
