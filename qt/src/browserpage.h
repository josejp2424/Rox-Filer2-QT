#pragma once

#include "filepane.h"

#include <QWidget>
#include <QPointer>

class QSplitter;

class BrowserPage final : public QWidget
{
    Q_OBJECT
public:
    explicit BrowserPage(const QString &path, QWidget *parent = nullptr);

    FilePane *activePane() const;
    FilePane *otherPane() const;
    QString currentPath() const;
    QString selectedPath() const;
    bool splitEnabled() const;

    void navigateActive(const QString &path);
    void goBack();
    void goForward();
    void goUp();
    void refresh();
    void setFilterText(const QString &text);
    void setIconMode(bool icons);
    void toggleSplit();

signals:
    void currentPathChanged(const QString &path);
    void selectionPathChanged(const QString &path);
    void requestNewTab(const QString &path);
    void hostRequest(FilePane::HostAction action, const QString &argument);
    void viewSettingsChanged();

private:
    QSplitter *m_splitter = nullptr;
    FilePane *m_left = nullptr;
    FilePane *m_right = nullptr;
    // Focus moves from the file view into a menu when selecting an action.
    // Keep the last focused pane so menu commands still target the right pane.
    QPointer<FilePane> m_lastFocusedPane;
};
