#include "findexpr.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <functional>
#include <vector>

struct FindInfo
{
    QByteArray fullPath;
    QByteArray leaf;
    struct stat st {};
    double now = 0;
};

struct FindExpr::Node
{
    std::function<bool(const FindInfo &)> test;
};

namespace {

using NodePtr = std::unique_ptr<FindExpr::Node>;
using Eval = std::function<double(const FindInfo &)>;

enum Flag { FlagAgo = 1, FlagHence = 2 };

class Parser
{
public:
    explicit Parser(const QByteArray &text) : m_text(text) {}

    NodePtr parseAll()
    {
        NodePtr node = parseExpression();
        skip();
        if (!node || m_pos != m_text.size())
            return nullptr;
        return node;
    }

private:
    QByteArray m_text;
    int m_pos = 0;

    char next() const { return m_pos < m_text.size() ? m_text.at(m_pos) : '\0'; }
    char at(int offset) const { return m_pos + offset < m_text.size() ? m_text.at(m_pos + offset) : '\0'; }
    void eat() { ++m_pos; }
    void skip() { while (next() == ' ' || next() == '\t') eat(); }

    // Same rule as find.c match(): case-insensitive word, not followed by a letter.
    bool match(const char *word)
    {
        const int len = int(qstrlen(word));
        if (m_text.mid(m_pos, len).compare(QByteArray(word), Qt::CaseInsensitive) != 0 ||
            m_pos + len > m_text.size())
            return false;
        const char after = at(len);
        if ((after >= 'a' && after <= 'z') || (after >= 'A' && after <= 'Z'))
            return false;
        m_pos += len;
        return true;
    }

    static NodePtr make(std::function<bool(const FindInfo &)> test)
    {
        auto node = std::make_unique<FindExpr::Node>();
        node->test = std::move(test);
        return node;
    }

    static std::shared_ptr<FindExpr::Node> share(NodePtr node) { return std::shared_ptr<FindExpr::Node>(std::move(node)); }

    NodePtr parseExpression()
    {
        NodePtr first = parseCase();
        if (!first)
            return nullptr;
        skip();
        if (next() != ',')
            return first;
        eat();
        NodePtr second = parseExpression();
        if (!second)
            return nullptr;
        auto a = share(std::move(first));
        auto b = share(std::move(second));
        return make([a, b](const FindInfo &i) { return a->test(i) || b->test(i); });
    }

    NodePtr parseCase()
    {
        NodePtr first = parseCondition();
        if (!first)
            return nullptr;
        skip();
        if (next() == '\0' || next() == ',' || next() == ')')
            return first;
        (void) match("And");
        NodePtr second = parseCase();
        if (!second)
            return nullptr;
        auto a = share(std::move(first));
        auto b = share(std::move(second));
        return make([a, b](const FindInfo &i) { return a->test(i) && b->test(i); });
    }

    NodePtr parseCondition()
    {
        skip();
        if (next() == '!' && at(1) != '=') {
            eat();
            return negate();
        }
        if (match("Not"))
            return negate();
        if (next() == '(') {
            eat();
            NodePtr sub = parseExpression();
            if (!sub)
                return nullptr;
            skip();
            if (next() != ')')
                return nullptr;
            eat();
            return sub;
        }
        if (next() == '\'') {
            eat();
            return parseGlob();
        }
        if (NodePtr dash = parseDash())
            return dash;
        if (NodePtr is = parseIs())
            return is;
        return parseComparison();
    }

    NodePtr negate()
    {
        NodePtr operand = parseCondition();
        if (!operand)
            return nullptr;
        auto a = share(std::move(operand));
        return make([a](const FindInfo &i) { return !a->test(i); });
    }

    NodePtr parseGlob()
    {
        QByteArray pattern;
        bool path = false;
        while (next() != '\'') {
            char c = next();
            if (c == '\0')
                return nullptr;
            eat();
            if (c == '\\' && next() == '\'') {
                c = next();
                eat();
            }
            if (c == '/')
                path = true;
            pattern.append(c);
        }
        eat();
        if (path)
            return make([pattern](const FindInfo &i) {
                return fnmatch(pattern.constData(), i.fullPath.constData(), FNM_PATHNAME) == 0;
            });
        return make([pattern](const FindInfo &i) {
            return fnmatch(pattern.constData(), i.leaf.constData(), 0) == 0;
        });
    }

    enum class Is { Dir, Reg, Lnk, Fifo, Sock, Chr, Blk, Dev, Door, Suid, Sgid, Sticky,
                    Readable, Writeable, Exec, Empty, Mine, Xattr };

    static NodePtr isTest(Is test)
    {
        return make([test](const FindInfo &i) {
            const mode_t mode = i.st.st_mode;
            switch (test) {
            case Is::Dir: return S_ISDIR(mode);
            case Is::Reg: return S_ISREG(mode);
            case Is::Lnk: return S_ISLNK(mode);
            case Is::Fifo: return S_ISFIFO(mode);
            case Is::Sock: return S_ISSOCK(mode);
            case Is::Chr: return S_ISCHR(mode);
            case Is::Blk: return S_ISBLK(mode);
            case Is::Dev: return S_ISCHR(mode) || S_ISBLK(mode);
            case Is::Door: return false; // no doors on Linux
            case Is::Suid: return (mode & S_ISUID) != 0;
            case Is::Sgid: return (mode & S_ISGID) != 0;
            case Is::Sticky: return (mode & S_ISVTX) != 0;
            case Is::Readable: return ::access(i.fullPath.constData(), R_OK) == 0;
            case Is::Writeable: return ::access(i.fullPath.constData(), W_OK) == 0;
            case Is::Exec: return ::access(i.fullPath.constData(), X_OK) == 0;
            case Is::Empty: return i.st.st_size == 0;
            case Is::Mine: return i.st.st_uid == ::geteuid();
            case Is::Xattr: return ::llistxattr(i.fullPath.constData(), nullptr, 0) > 0;
            }
            return false;
        });
    }

    NodePtr parseDash()
    {
        if (next() != '-')
            return nullptr;
        NodePtr result;
        int i = 1;
        while (at(i) && at(i) != ' ' && at(i) != '\t') {
            Is test;
            switch (at(i)) {
            case 'f': test = Is::Reg; break;
            case 'l': test = Is::Lnk; break;
            case 'd': test = Is::Dir; break;
            case 'b': test = Is::Blk; break;
            case 'c': test = Is::Chr; break;
            case 'D': test = Is::Dev; break;
            case 'p': test = Is::Fifo; break;
            case 'S': test = Is::Sock; break;
            case 'O': test = Is::Door; break;
            case 'u': test = Is::Suid; break;
            case 'g': test = Is::Sgid; break;
            case 'k': test = Is::Sticky; break;
            case 'r': test = Is::Readable; break;
            case 'w': test = Is::Writeable; break;
            case 'x': test = Is::Exec; break;
            case 'o': test = Is::Mine; break;
            case 'X': test = Is::Xattr; break;
            case 'z': test = Is::Empty; break;
            default: return nullptr;
            }
            ++i;
            NodePtr cond = isTest(test);
            if (result) {
                auto a = share(std::move(result));
                auto b = share(std::move(cond));
                result = make([a, b](const FindInfo &info) { return a->test(info) && b->test(info); });
            } else {
                result = std::move(cond);
            }
        }
        m_pos += i;
        return result;
    }

    NodePtr parseIs()
    {
        struct Word { const char *name; Is test; };
        static const Word words[] = {
            {"IsReg", Is::Reg}, {"IsLink", Is::Lnk}, {"IsDir", Is::Dir}, {"IsChar", Is::Chr},
            {"IsBlock", Is::Blk}, {"IsDev", Is::Dev}, {"IsPipe", Is::Fifo}, {"IsSocket", Is::Sock},
            {"IsDoor", Is::Door}, {"IsSUID", Is::Suid}, {"IsSGID", Is::Sgid}, {"IsSticky", Is::Sticky},
            {"IsReadable", Is::Readable}, {"IsWriteable", Is::Writeable}, {"IsExecutable", Is::Exec},
            {"IsEmpty", Is::Empty}, {"IsMine", Is::Mine}, {"HasXattr", Is::Xattr},
        };
        for (const Word &w : words)
            if (match(w.name))
                return isTest(w.test);
        return nullptr;
    }

    Eval parseVariable()
    {
        skip();
        if (match("atime")) return [](const FindInfo &i) { return double(i.st.st_atime); };
        if (match("ctime")) return [](const FindInfo &i) { return double(i.st.st_ctime); };
        if (match("mtime")) return [](const FindInfo &i) { return double(i.st.st_mtime); };
        if (match("size")) return [](const FindInfo &i) { return double(i.st.st_size); };
        if (match("inode")) return [](const FindInfo &i) { return double(i.st.st_ino); };
        if (match("nlinks")) return [](const FindInfo &i) { return double(i.st.st_nlink); };
        if (match("uid")) return [](const FindInfo &i) { return double(i.st.st_uid); };
        if (match("gid")) return [](const FindInfo &i) { return double(i.st.st_gid); };
        if (match("blocks")) return [](const FindInfo &i) { return double(i.st.st_blocks); };
        return nullptr;
    }

    Eval parseEval()
    {
        skip();
        // strtol(start, &end, 0): decimal, 0x hex and 0 octal like find.c.
        const QByteArray rest = m_text.mid(m_pos);
        char *end = nullptr;
        double value = double(strtol(rest.constData(), &end, 0));
        int flags = 0;
        if (end == rest.constData()) {
            if (match("Now")) {
                value = 0;
                flags |= FlagHence;
            } else {
                return parseVariable();
            }
        } else {
            m_pos += int(end - rest.constData());
        }
        skip();
        if (match("Bytes") || match("Byte")) {}
        else if (match("Kb") || match("K")) value *= 1 << 10;
        else if (match("Mb") || match("M")) value *= 1 << 20;
        else if (match("Gb") || match("G")) value *= 1 << 30;
        else if (match("Secs") || match("Sec")) {}
        else if (match("Mins") || match("Min")) value *= 60;
        else if (match("Hours") || match("Hour")) value *= 60 * 60;
        else if (match("Days") || match("Day")) value *= 60 * 60 * 24;
        else if (match("Weeks") || match("Week")) value *= 60 * 60 * 24 * 7;
        else if (match("Years") || match("Year")) value *= 60 * 60 * 24 * 7 * 365.25;
        skip();
        if (match("Ago")) flags |= FlagAgo;
        else if (match("Hence")) flags |= FlagHence;
        return [value, flags](const FindInfo &i) {
            if (flags & FlagAgo) return i.now - value;
            if (flags & FlagHence) return i.now + value;
            return value;
        };
    }

    NodePtr parseComparison()
    {
        skip();
        Eval first = parseEval();
        if (!first)
            return nullptr;
        skip();
        enum { LT, LE, EQ, NE, GE, GT } comp;
        if (next() == '=') { comp = EQ; eat(); }
        else if (next() == '>') { eat(); if (next() == '=') { eat(); comp = GE; } else comp = GT; }
        else if (next() == '<') { eat(); if (next() == '=') { eat(); comp = LE; } else comp = LT; }
        else if (next() == '!' && at(1) == '=') { eat(); eat(); comp = NE; }
        else if (match("After")) comp = GT;
        else if (match("Before")) comp = LT;
        else return nullptr;
        skip();
        Eval second = parseEval();
        if (!second)
            return nullptr;
        return make([first, second, comp](const FindInfo &i) {
            const double a = first(i);
            const double b = second(i);
            switch (comp) {
            case LT: return a < b;
            case LE: return a <= b;
            case EQ: return a == b;
            case NE: return a != b;
            case GE: return a >= b;
            case GT: return a > b;
            }
            return false;
        });
    }
};

} // namespace

FindExpr::FindExpr() = default;
FindExpr::~FindExpr() = default;

std::unique_ptr<FindExpr> FindExpr::parse(const QString &expression, QString *error)
{
    Parser parser(expression.toUtf8());
    NodePtr root = parser.parseAll();
    if (!root) {
        if (error)
            *error = QCoreApplication::translate("FindExpr", "The expression is not valid.");
        return nullptr;
    }
    std::unique_ptr<FindExpr> result(new FindExpr);
    result->m_root = std::move(root);
    return result;
}

bool FindExpr::matches(const QString &fullPath, qint64 now) const
{
    FindInfo info;
    info.fullPath = QFile::encodeName(fullPath);
    info.leaf = QFile::encodeName(QFileInfo(fullPath).fileName());
    if (::lstat(info.fullPath.constData(), &info.st) != 0)
        return false;
    info.now = now > 0 ? double(now) : double(QDateTime::currentSecsSinceEpoch());
    return m_root && m_root->test(info);
}
