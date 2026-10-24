#include "xattrs.h"

#include <QFile>
#include <QStringDecoder>

#include <cerrno>
#include <cstring>
#include <sys/xattr.h>

namespace Xattr {

namespace {

void setError(QString *error, int err)
{
    if (error)
        *error = QString::fromLocal8Bit(std::strerror(err));
}

} // namespace

bool supported(const QString &path)
{
    const QByteArray p = QFile::encodeName(path);
    if (::listxattr(p.constData(), nullptr, 0) >= 0)
        return true;
    return errno != ENOTSUP && errno != ENOSYS;
}

QList<Attribute> list(const QString &path, QString *error)
{
    const QByteArray p = QFile::encodeName(path);
    QList<Attribute> result;
    ssize_t size = ::listxattr(p.constData(), nullptr, 0);
    if (size < 0) {
        setError(error, errno);
        return result;
    }
    if (size == 0)
        return result;
    QByteArray names(int(size), '\0');
    size = ::listxattr(p.constData(), names.data(), size_t(names.size()));
    if (size < 0) {
        setError(error, errno);
        return result;
    }
    names.resize(int(size));
    for (const QByteArray &name : names.split('\0')) {
        if (name.isEmpty())
            continue;
        QByteArray value;
        const ssize_t vsize = ::getxattr(p.constData(), name.constData(), nullptr, 0);
        if (vsize > 0) {
            value.resize(int(vsize));
            const ssize_t got = ::getxattr(p.constData(), name.constData(), value.data(), size_t(value.size()));
            if (got >= 0)
                value.resize(int(got));
            else
                value.clear();
        }
        result.append({name, value});
    }
    return result;
}

bool set(const QString &path, const QByteArray &name, const QByteArray &value, QString *error)
{
    const QByteArray p = QFile::encodeName(path);
    if (::setxattr(p.constData(), name.constData(), value.constData(), size_t(value.size()), 0) == 0)
        return true;
    setError(error, errno);
    return false;
}

bool remove(const QString &path, const QByteArray &name, QString *error)
{
    const QByteArray p = QFile::encodeName(path);
    if (::removexattr(p.constData(), name.constData()) == 0)
        return true;
    setError(error, errno);
    return false;
}

bool isText(const QByteArray &value)
{
    if (value.contains('\0'))
        return false;
    QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    const QString text = decoder(value);
    return !decoder.hasError();
}

} // namespace Xattr
