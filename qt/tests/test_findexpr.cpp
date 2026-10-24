// Offscreen checks for the ROX "Select If" expression language (findexpr.cpp).

#include "findexpr.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>
#include <sys/stat.h>

static int failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++failures;                                                               \
        }                                                                             \
    } while (0)

static void write(const QString &path, const QByteArray &data, mode_t mode = 0644)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) { f.write(data); f.close(); }
    ::chmod(QFile::encodeName(path).constData(), mode);
}

static bool m(const QString &expr, const QString &path, bool *parsed = nullptr)
{
    QString error;
    auto e = FindExpr::parse(expr, &error);
    if (parsed) *parsed = e != nullptr;
    return e && e->matches(path);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString d = tmp.path();
    write(d + "/photo.png", QByteArray(2 * 1024 * 1024, 'x'));
    write(d + "/note.txt", "hi");
    write(d + "/empty", "");
    write(d + "/run.sh", "#!/bin/sh\n", 0755);
    QDir(d).mkpath("sub");
    QFile::link(d + "/note.txt", d + "/link");
    // old.txt: modified 3 weeks ago
    write(d + "/old.txt", "old");
    QFile old(d + "/old.txt");
    old.open(QIODevice::ReadWrite);
    old.setFileTime(QDateTime::currentDateTime().addDays(-21), QFileDevice::FileModificationTime);
    old.close();

    bool ok = false;
    // globs
    CHECK(m("'*.png'", d + "/photo.png"));
    CHECK(!m("'*.png'", d + "/note.txt"));
    CHECK(m("'*.PNG'", d + "/photo.png") == false);          // fnmatch is case-sensitive
    CHECK(m("'" + d + "/*.txt'", d + "/note.txt"));            // path glob (contains '/')
    // sizes and units
    CHECK(m("size > 1M", d + "/photo.png"));
    CHECK(!m("size > 3M", d + "/photo.png"));
    CHECK(m("size >= 2 Mb", d + "/photo.png"));
    CHECK(m("size = 2", d + "/note.txt"));
    CHECK(m("size != 2", d + "/photo.png"));
    CHECK(m("size < 1K", d + "/note.txt"));
    // is-tests, dash flags, words
    CHECK(m("IsDir", d + "/sub"));
    CHECK(!m("IsDir", d + "/note.txt"));
    CHECK(m("-d", d + "/sub"));
    CHECK(m("-f", d + "/note.txt"));
    CHECK(m("-l", d + "/link"));                                // lstat: a link is a link
    CHECK(!m("-f", d + "/link"));
    CHECK(m("-fx", d + "/run.sh"));                             // flags are ANDed
    CHECK(!m("-fx", d + "/note.txt"));
    CHECK(m("IsEmpty", d + "/empty"));
    CHECK(m("-z", d + "/empty"));
    CHECK(m("IsMine", d + "/note.txt"));
    CHECK(m("isreg", d + "/note.txt"));                         // case-insensitive
    // boolean structure
    CHECK(m("'*.txt' and size < 10", d + "/note.txt"));
    CHECK(m("'*.txt' size < 10", d + "/note.txt"));             // "And" is optional
    CHECK(!m("'*.txt' and size > 10", d + "/note.txt"));
    CHECK(m("'*.png', '*.txt'", d + "/note.txt"));              // comma is OR
    CHECK(m("not '*.png'", d + "/note.txt"));
    CHECK(m("! '*.png'", d + "/note.txt"));
    CHECK(!m("not '*.txt'", d + "/note.txt"));
    CHECK(m("('*.png', '*.txt') and not IsDir", d + "/note.txt"));
    CHECK(!m("('*.png', '*.txt') and IsDir", d + "/note.txt"));
    // time
    CHECK(m("mtime before 1 week ago", d + "/old.txt"));
    CHECK(!m("mtime before 1 week ago", d + "/note.txt"));
    CHECK(m("mtime after 1 week ago", d + "/note.txt"));
    CHECK(m("mtime <= now", d + "/note.txt"));
    CHECK(m("mtime > 1 hour ago", d + "/note.txt"));
    CHECK(m("mtime > 1 year ago", d + "/old.txt"));
    // escapes
    write(d + "/it's.txt", "q");
    CHECK(m("'it\\'s.txt'", d + "/it's.txt"));
    // errors: must not parse, and must not match anything
    m("size >", d + "/note.txt", &ok);            CHECK(!ok);
    m("'unterminated", d + "/note.txt", &ok);     CHECK(!ok);
    m("(IsDir", d + "/sub", &ok);                 CHECK(!ok);
    m("IsDir)", d + "/sub", &ok);                 CHECK(!ok);
    m("bogus", d + "/sub", &ok);                  CHECK(!ok);
    m("-q", d + "/sub", &ok);                     CHECK(!ok);
    m("", d + "/sub", &ok);                       CHECK(!ok);
    // system() / prune are intentionally unsupported (selecting must not run commands)
    m("system(true)", d + "/sub", &ok);           CHECK(!ok);
    m("prune", d + "/sub", &ok);                  CHECK(!ok);
    // missing file never matches
    CHECK(!m("'*'", d + "/nope"));

    if (failures == 0) std::printf("findexpr: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
