#include "xattrs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Use the home/work dir rather than /tmp (tmpfs may reject user.* attributes).
    QTemporaryDir tmp(QDir::currentPath() + "/xattr-XXXXXX");
    CHECK(tmp.isValid());
    const QString file = tmp.path() + "/f";
    QFile f(file);
    CHECK(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    if (!Xattr::supported(file)) {
        std::printf("xattrs: filesystem has no extended attributes, skipped\n");
        return 0;
    }
    QString error;
    CHECK(Xattr::list(file, &error).isEmpty());
    CHECK(Xattr::set(file, "user.rox.test", "hello", &error));
    CHECK(Xattr::set(file, "user.rox.bin", QByteArray("a\0b", 3), &error));
    const auto attrs = Xattr::list(file, &error);
    CHECK(attrs.size() == 2);
    QByteArray text, bin;
    for (const auto &a : attrs) {
        if (a.name == "user.rox.test") text = a.value;
        if (a.name == "user.rox.bin") bin = a.value;
    }
    CHECK(text == "hello");
    CHECK(bin.size() == 3);
    CHECK(Xattr::isText(text));
    CHECK(!Xattr::isText(bin));
    CHECK(!Xattr::isText(QByteArray("\xff\xfe", 2)));
    CHECK(Xattr::isText(QByteArray("ñandú")));
    CHECK(Xattr::set(file, "user.rox.test", "changed", &error));
    CHECK(Xattr::list(file).first().value == "changed" || Xattr::list(file).last().value == "changed");
    CHECK(Xattr::remove(file, "user.rox.test", &error));
    CHECK(Xattr::list(file).size() == 1);
    CHECK(!Xattr::remove(file, "user.rox.missing", &error));
    CHECK(!error.isEmpty());
    CHECK(!Xattr::set(QStringLiteral("/nonexistent/file"), "user.x", "y", &error));

    if (failures == 0) std::printf("xattrs: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
