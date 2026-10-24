#pragma once

/*
 * ROX-Filer2-QT - "Select If" expressions, ported from GTK3 find.c.
 *
 * Grammar (same as ROX; keywords are case-insensitive):
 *   expression := case [ "," expression ]            "," is OR
 *   case       := condition [ ["And"] case ]          juxtaposition is AND
 *   condition  := "!" condition | "Not" condition | "(" expression ")"
 *               | 'glob' | -flags | IsDir ... | HasXattr | comparison
 *   'glob'     := fnmatch on the leaf name; if it contains "/" on the full path
 *   -flags     := -f -l -d -b -c -D -p -S -O -u -g -k -r -w -x -o -X -z (ANDed)
 *   comparison := value (= > >= < <= != After Before) value
 *   value      := number [unit] [Ago|Hence] | Now | atime ctime mtime size inode
 *                 nlinks uid gid blocks
 *   unit       := Bytes Kb K Mb M Gb G Sec Min Hour Day Week Year
 * Examples:  size > 1M    'ptr*' and IsDir    mtime after 1 week ago
 * The ROX system(...) and prune forms are NOT supported: selecting files must
 * never run shell commands.
 */

#include <QString>

#include <memory>

class FindExpr
{
public:
    ~FindExpr();
    FindExpr(const FindExpr &) = delete;
    FindExpr &operator=(const FindExpr &) = delete;

    // Returns nullptr (and fills *error) if the expression does not parse
    // completely.
    static std::unique_ptr<FindExpr> parse(const QString &expression, QString *error = nullptr);

    // Evaluated on lstat() of fullPath, like GTK3 (a symlink is a link).
    // `now` is injectable for tests; 0 means the current time.
    bool matches(const QString &fullPath, qint64 now = 0) const;

    struct Node;
private:
    FindExpr();
    std::unique_ptr<Node> m_root;
};
