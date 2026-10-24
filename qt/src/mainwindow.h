#pragma once

#include "filepane.h"

#include <QMainWindow>
#include <QList>
#include <QStringList>

class QAction;
class BrowserPage;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QSplitter;
class QTabWidget;
class PathBar;

class MainWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const QStringList &initialPaths = {}, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct DeviceInfo {
        QString device;
        QString label;
        QString mountPoint;
        QString fsType;
        QString type;
        QString model;
        qint64 size = 0;
        bool removable = false;
        bool image = false;
        bool protectedImage = false;
        QString imagePath;
    };

    BrowserPage *currentPage() const;
    void buildActions();
    void buildUi();
    void buildPlaces();
    void buildMenus();
    void rebuildBookmarks();
    void rebuildDevices();
    QList<DeviceInfo> enumerateBlockDevices() const;
    QString mountPointForDevice(const QString &device) const;
    void addTab(const QString &path, bool makeCurrent = true);
    void closeTab(int index);
    void closeOtherTabs();
    void closeTabsToRight();
    void duplicateCurrentTab();
    void openNewWindow();
    void syncUiFromCurrentPage();
    void updatePreview(const QString &path);
    void updateStatusBar(const QString &path);
    void launchTerminal();
    void openDevice(const QString &device, const QString &mountPoint);
    void mountDevice(const QString &device, bool openAfterMount = true);
    void unmountDevice(const QString &device);
    void connectSmb();
    void disconnectSmb();
    void showSharedFolders();
    void addCurrentBookmark();
    void addBookmark(const QString &path);
    void handleHostRequest(FilePane::HostAction action, const QString &argument);
    void editBookmarks();
    void showHelp();
    void showAbout();
    void showOptions();
    void copyOrMoveToOtherPane(bool move);

    QSplitter *m_rootSplitter = nullptr;
    QListWidget *m_places = nullptr;
    QString m_imageSnapshot;
    QTabWidget *m_tabs = nullptr;
    PathBar *m_pathBar = nullptr;
    QLineEdit *m_filter = nullptr;
    QMenu *m_bookmarksMenu = nullptr;
    QWidget *m_preview = nullptr;
    QLabel *m_previewImage = nullptr;
    QLabel *m_previewInfo = nullptr;

    QAction *m_back = nullptr;
    QAction *m_forward = nullptr;
    QAction *m_up = nullptr;
    QAction *m_refresh = nullptr;
    QAction *m_home = nullptr;
    QAction *m_newTab = nullptr;
    QAction *m_closeTab = nullptr;
    QAction *m_split = nullptr;
    QAction *m_icons = nullptr;
    QAction *m_details = nullptr;
    QAction *m_previewToggle = nullptr;
    QAction *m_sidebarToggle = nullptr;
    QAction *m_terminal = nullptr;
};
