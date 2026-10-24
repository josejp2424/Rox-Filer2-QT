#pragma once

/*
 * ROX-Filer2-QT - ROX behaviour options (the "bind_*" family of the GTK3
 * build: single/double click, button 1 in a new window, double click on the
 * background resizes the window).
 *
 * Precedence: a value saved by the Qt Options dialog wins; otherwise the value
 * from the GTK3 build's own Options file
 *   $XDG_CONFIG_HOME/rox.sourceforge.net/ROX-Filer/Options
 * is used (read only), so the Qt filer starts out behaving like the GTK3 one on
 * the same account; otherwise the GTK3 built-in default.
 */

#include <QString>

namespace RoxOptions {

enum class ClickMode {
    Double = 0,     // select on click, open on double click
    Single = 1,     // open on a single click (GTK3 default)
    SingleDirs = 2  // folders open on a single click, files on a double click
};

ClickMode clickMode();
void setClickMode(ClickMode mode);

// Button 1 on a folder opens it in a new window instead of the same one.
bool newButton1();
void setNewButton1(bool enabled);

// Double click on the empty background resizes the window to fit its files.
bool dclickResizes();
void setDclickResizes(bool enabled);

// Read-only lookup in the GTK3 Options file; empty if absent. Exposed for tests.
QString gtkOptionValue(const QString &name, const QString &optionsFile = QString());
QString gtkOptionsPath();

} // namespace RoxOptions
