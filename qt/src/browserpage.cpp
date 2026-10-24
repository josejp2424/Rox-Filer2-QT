#include "browserpage.h"
#include "filepane.h"

#include <QApplication>
#include <QSplitter>
#include <QVBoxLayout>

BrowserPage::BrowserPage(const QString &path, QWidget *parent)
    : QWidget(parent)
{
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_left = new FilePane(m_splitter);
    m_right = new FilePane(m_splitter);
    m_splitter->addWidget(m_left);
    m_splitter->addWidget(m_right);
    m_splitter->setChildrenCollapsible(false);
    m_right->hide();
    m_lastFocusedPane = m_left;
    // Use QApplication's typed sender; qApp is a QCoreApplication pointer
    // in some Qt6 headers and cannot be used with focusChanged directly.
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    connect(app, &QApplication::focusChanged, this,
            [this](QWidget *, QWidget *now) {
        if (!now) return;
        if (now == m_left || m_left->isAncestorOf(now))
            m_lastFocusedPane = m_left;
        else if (m_right->isVisible() &&
                 (now == m_right || m_right->isAncestorOf(now)))
            m_lastFocusedPane = m_right;
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_splitter);

    const auto wire = [this](FilePane *pane) {
        connect(pane, &FilePane::currentPathChanged, this, &BrowserPage::currentPathChanged);
        connect(pane, &FilePane::selectionPathChanged, this, &BrowserPage::selectionPathChanged);
        connect(pane, &FilePane::requestNewTab, this, &BrowserPage::requestNewTab);
        connect(pane, &FilePane::hostRequest, this, &BrowserPage::hostRequest);
        connect(pane, &FilePane::viewSettingsChanged, this, &BrowserPage::viewSettingsChanged);
    };
    wire(m_left);
    wire(m_right);

    m_left->navigate(path);
}

FilePane *BrowserPage::activePane() const
{
    QWidget *focus = QApplication::focusWidget();
    if (m_right->isVisible() && focus && (focus == m_right || m_right->isAncestorOf(focus)))
        return m_right;
    if (m_lastFocusedPane == m_right && m_right->isVisible())
        return m_right;
    return m_left;
}

FilePane *BrowserPage::otherPane() const
{
    if (!m_right->isVisible())
        return nullptr;
    return activePane() == m_right ? m_left : m_right;
}

QString BrowserPage::currentPath() const
{
    return activePane()->currentPath();
}

QString BrowserPage::selectedPath() const
{
    return activePane()->selectedPath();
}

bool BrowserPage::splitEnabled() const
{
    return m_right->isVisible();
}

void BrowserPage::navigateActive(const QString &path) { activePane()->navigate(path); }
void BrowserPage::goBack() { activePane()->goBack(); }
void BrowserPage::goForward() { activePane()->goForward(); }
void BrowserPage::goUp() { activePane()->goUp(); }
void BrowserPage::refresh() { activePane()->refresh(); }
void BrowserPage::setFilterText(const QString &text) { activePane()->setFilterText(text); }
void BrowserPage::setIconMode(bool icons) { activePane()->setIconMode(icons); }

void BrowserPage::toggleSplit()
{
    if (m_right->isVisible()) {
        m_right->hide();
        m_lastFocusedPane = m_left;
        return;
    }
    m_right->navigate(m_left->currentPath());
    m_right->setIconMode(m_left->iconMode());
    m_right->show();
    m_splitter->setSizes({1, 1});
}
