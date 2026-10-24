#include "pathbar.h"

#include <QDir>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QStackedLayout>
#include <QToolButton>

PathBar::PathBar(QWidget *parent)
    : QWidget(parent)
{
    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);

    m_crumbWidget = new QWidget(this);
    m_crumbLayout = new QHBoxLayout(m_crumbWidget);
    m_crumbLayout->setContentsMargins(4, 0, 4, 0);
    m_crumbLayout->setSpacing(0);

    m_editor = new QLineEdit(this);
    m_editor->setClearButtonEnabled(true);

    m_stack->addWidget(m_crumbWidget);
    m_stack->addWidget(m_editor);
    m_stack->setCurrentWidget(m_crumbWidget);

    connect(m_editor, &QLineEdit::returnPressed, this, [this] {
        const QString requested = QDir::cleanPath(m_editor->text().trimmed());
        if (!requested.isEmpty())
            emit pathActivated(requested);
        finishEdit();
    });
    connect(m_editor, &QLineEdit::editingFinished, this, [this] {
        if (m_stack->currentWidget() == m_editor)
            finishEdit();
    });
}

void PathBar::setPath(const QString &path)
{
    const QString cleaned = QDir::cleanPath(path);
    if (cleaned == m_path)
        return;
    m_path = cleaned;
    m_editor->setText(m_path);
    rebuildBreadcrumbs();
}

void PathBar::setBreadcrumbMode(bool enabled)
{
    m_breadcrumbMode = enabled;
    if (enabled) {
        rebuildBreadcrumbs();
        m_stack->setCurrentWidget(m_crumbWidget);
    } else {
        m_editor->setText(m_path);
        m_stack->setCurrentWidget(m_editor);
    }
}

void PathBar::beginEdit()
{
    m_editor->setText(m_path);
    m_stack->setCurrentWidget(m_editor);
    m_editor->setFocus(Qt::ShortcutFocusReason);
    m_editor->selectAll();
}

void PathBar::finishEdit()
{
    if (m_breadcrumbMode)
        m_stack->setCurrentWidget(m_crumbWidget);
    else
        m_stack->setCurrentWidget(m_editor);
}

void PathBar::rebuildBreadcrumbs()
{
    while (QLayoutItem *item = m_crumbLayout->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }

    QString current = QDir::rootPath();
    auto *root = new QToolButton(m_crumbWidget);
    root->setText(QStringLiteral("/"));
    root->setAutoRaise(true);
    root->setToolTip(QDir::rootPath());
    connect(root, &QToolButton::clicked, this, [this] { emit pathActivated(QDir::rootPath()); });
    m_crumbLayout->addWidget(root);

    const QString relative = QDir::root().relativeFilePath(m_path);
    if (relative != QStringLiteral(".")) {
        const QStringList parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            current = QDir(current).filePath(part);
            auto *separator = new QToolButton(m_crumbWidget);
            separator->setText(QStringLiteral("›"));
            separator->setAutoRaise(true);
            separator->setEnabled(false);
            m_crumbLayout->addWidget(separator);

            auto *button = new QToolButton(m_crumbWidget);
            button->setText(part);
            button->setAutoRaise(true);
            button->setToolTip(current);
            const QString target = current;
            connect(button, &QToolButton::clicked, this, [this, target] { emit pathActivated(target); });
            m_crumbLayout->addWidget(button);
        }
    }
    m_crumbLayout->addStretch(1);
}
