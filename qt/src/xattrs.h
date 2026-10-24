#pragma once

/*
 * ROX-Filer2-QT - extended attributes backend (GTK3 xattr.c equivalent,
 * Linux getxattr/setxattr family; follows symbolic links like ROX does).
 */

#include <QByteArray>
#include <QList>
#include <QString>

namespace Xattr {

struct Attribute
{
    QByteArray name;
    QByteArray value;
};

// True if the filesystem of `path` answers extended-attribute calls at all.
bool supported(const QString &path);

QList<Attribute> list(const QString &path, QString *error = nullptr);
bool set(const QString &path, const QByteArray &name, const QByteArray &value, QString *error = nullptr);
bool remove(const QString &path, const QByteArray &name, QString *error = nullptr);

// Text editing: only valid UTF-8 without NUL is shown and edited as text.
bool isText(const QByteArray &value);

} // namespace Xattr
