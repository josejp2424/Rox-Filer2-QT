#pragma once

#include "filemodel.h"

#include <QElapsedTimer>
#include <QModelIndex>
#include <QStringList>
#include <QWidget>

class QAbstractItemView;
class QAction;
class QActionGroup;
class QListView;
class QMenu;
class QStackedWidget;
class QTreeView;
class RoxIconProvider;

class FilePane final : public QWidget
{
    Q_OBJECT
public:
    /* Requests that need the main window (menus, dialogs, bookmarks...). */
    enum class HostAction {
        NewWindow,
        EditLocation,
        FocusFilter,
        Options,
        OpenTerminal,
        AddBookmark,
        ShowBookmarks,
        ConnectSmb,
        SharedFolders,
        CloseWindow,
        CloseAfterOpen,   // ROX: middle-click open closes the tab/window
        ResizeToFit,      // argument: "WxH" wanted size of the file view
        PairedWindows,    // ROX paired windows == the F3 split view here
        StatusMessage
    };
    Q_ENUM(HostAction)

    explicit FilePane(QWidget *parent = nullptr);
    ~FilePane() override;

    QString currentPath() const { return m_currentPath; }
    QString selectedPath() const;
    QStringList selectedPaths() const;
    bool canGoBack() const;
    bool canGoForward() const;
    bool iconMode() const { return m_iconMode; }

    void navigate(const QString &path, bool addHistory = true);
    void goBack();
    void goForward();
    void goUp();
    void refresh();
    void setFilterText(const QString &text);
    void setIconMode(bool icons);
    void focusFiles();

    // Selection operations always target the file view, never the filter or
    // the Places list. The current proxy determines the visible items.
    void selectAllFiles();
    void clearFileSelection();
    void invertSelection();
    void selectByName();
    void selectIf();   // ROX "Select If": expression language of GTK3 find.c

    // Resize Window (Ctrl+E / double click on the background): the size the
    // file view would need to show every item, clamped to the screen.
    QSize desiredViewSize() const;
    QSize currentViewSize() const;
    void requestResize();

    // Menu actions are forwarded to the ACTIVE pane. They must never use a
    // directory-as-selection fallback when no files are selected.
    void trashSelected();
    void deleteSelected();
    void renameSelected();
    void copyToClipboard(bool cut);
    void paste();
    bool isTrashDirectory() const { return inTrash(); }

    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);
    bool thumbnails() const;
    void setThumbnails(bool enabled);

signals:
    void currentPathChanged(const QString &path);
    void selectionPathChanged(const QString &path);
    void requestNewTab(const QString &path);
    void hostRequest(FilePane::HostAction action, const QString &argument);
    void viewSettingsChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    QAbstractItemView *currentView() const;
    QModelIndex sourceIndexForViewIndex(const QModelIndex &index) const;
    QModelIndex proxyRootIndex() const;
    void openIndex(const QModelIndex &index);
    // ROX run-item semantics (GTK3 run.c): AppDirs and executables RUN, mount
    // points mount, folders are entered, everything else opens in its MIME
    // application. `shift` is ROX "edit" (look inside / open as text / unmount),
    // `button` selects button-1 (same pane) or adjust/middle (new tab, and a
    // file or application closes the window after opening).
    void openItem(const QString &path, bool shift, Qt::MouseButton button);
    bool openFile(const QString &path);
    void openAsText(const QString &path);
    void openMountPoint(const QString &path, bool shift);
    bool isMountPoint(const QString &path) const;
    bool singleClickOpens(const QString &path) const;
    void showContextMenu(QWidget *view, const QPoint &point);
    void emitSelection();
    void updateRoots();
    bool searchByTyping(const QString &text, bool backspace = false);
    void status(const QString &text);
    bool inTrash() const;

    void createActions();
    void updateActionStates();
    void buildItemMenu(QMenu *menu, const QStringList &paths);
    void buildBackgroundMenu(QMenu *menu);
    void fillDisplayMenu(QMenu *menu);
    void fillSelectMenu(QMenu *menu);
    void fillNewMenu(QMenu *menu);
    void fillWindowMenu(QMenu *menu);

    void applyIconLayout();
    void queueIconLayout();
    void transferToPath(const QStringList &paths, bool move, bool duplicate = false);
    void applySort();
    void changeIconSize(int step);
    void setWith(RoxWith with);
    void autoSizeIfNeeded();
    void saveDisplaySettings();
    QString detailText(const QModelIndex &proxyIndex, RoxWith with) const;

    void shiftOpen(const QString &path);
    void newDirectory();
    void newBlankFile();
    void newFromTemplate(const QString &templatePath);
    void selectPathWhenLoaded(const QString &path);
    void trySelectPending();

    RoxFileModel *m_model = nullptr;
    RoxProxyModel *m_proxy = nullptr;
    RoxIconProvider *m_iconProvider = nullptr;
    RoxIconDelegate *m_iconDelegate = nullptr;
    QListView *m_icons = nullptr;
    QTreeView *m_details = nullptr;
    QStackedWidget *m_views = nullptr;
    QString m_currentPath;
    QStringList m_history;
    int m_historyPos = -1;
    bool m_iconMode = true;
    bool m_showHidden = false;
    bool m_autoSize = false;
    int m_iconSize = 48;
    bool m_reversed = false;
    bool m_iconLayoutQueued = false;
    QString m_hoverPath;
    QString m_pendingSelect;
    QString m_typeAheadText;
    QElapsedTimer m_typeAheadClock;

    // Persistent actions: they carry the keyboard shortcuts and are reused in
    // the context menus so the menu shows the real working shortcut.
    QAction *m_actCut = nullptr;
    QAction *m_actCopy = nullptr;
    QAction *m_actPaste = nullptr;
    QAction *m_actDuplicate = nullptr;
    QAction *m_actRename = nullptr;
    QAction *m_actTrash = nullptr;
    QAction *m_actDelete = nullptr;
    QAction *m_actProperties = nullptr;
    QAction *m_actSelectAll = nullptr;
    QAction *m_actShowHidden = nullptr;
    QAction *m_actNewDir = nullptr;
    QAction *m_actBigger = nullptr;
    QAction *m_actSmaller = nullptr;
    QAction *m_actHome = nullptr;
    QAction *m_actAutoSize = nullptr;
    QAction *m_actReversed = nullptr;
    QAction *m_actFilterDirs = nullptr;
    QAction *m_actThumbs = nullptr;
    QAction *m_actResize = nullptr;
    QActionGroup *m_sortGroup = nullptr;
};
