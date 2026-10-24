#pragma once

/*
 * ROX-Filer2-QT - dialogs behind the classic context menu entries.
 * A QObject subclass only so that tr() uses the "RoxDialogs" context.
 */

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

class QWidget;

class RoxDialogs final : public QObject
{
    Q_OBJECT
public:
    /* Open With... / Set Default Application... */
    static void chooseApplication(QWidget *parent, const QStringList &paths, bool setDefaultOnly);

    static void showProperties(QWidget *parent, const QStringList &paths);
    static void showCount(QWidget *parent, const QStringList &paths);
    static void changePermissions(QWidget *parent, const QStringList &paths, std::function<void()> done);
    static void addFileAction(QWidget *parent, const QStringList &paths);
    static void shareFolder(QWidget *parent, const QString &path,
                            std::function<void(const QString &)> status);
    static void shellCommand(QWidget *parent, const QString &dir, const QStringList &selected,
                             std::function<void()> done);
    /* Extended attributes... (GTK3 FILE_XATTRS): list, add, edit and remove. */
    static void extendedAttributes(QWidget *parent, const QString &path);
};
