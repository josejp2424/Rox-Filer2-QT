#pragma once

#include <QWidget>
#include <QString>

class QHBoxLayout;
class QLineEdit;
class QStackedLayout;

class PathBar final : public QWidget
{
    Q_OBJECT
public:
    explicit PathBar(QWidget *parent = nullptr);
    QString path() const { return m_path; }
    void setPath(const QString &path);
    void beginEdit();
    void setBreadcrumbMode(bool enabled);
    bool breadcrumbMode() const { return m_breadcrumbMode; }

signals:
    void pathActivated(const QString &path);

private:
    void rebuildBreadcrumbs();
    void finishEdit();

    QString m_path;
    QWidget *m_crumbWidget = nullptr;
    QHBoxLayout *m_crumbLayout = nullptr;
    QLineEdit *m_editor = nullptr;
    QStackedLayout *m_stack = nullptr;
    bool m_breadcrumbMode = true;
};
