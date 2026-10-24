#include "mimeapps.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QMimeDatabase>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

#include <algorithm>

namespace {

using Group = QHash<QString, QString>;
using IniData = QHash<QString, Group>;

/* Desktop Entry string unescaping (\s \n \t \r \\), applied before the
 * Exec quoting rules, as required by the specification. */
QString unescapeValue(const QString &value)
{
    QString out;
    out.reserve(value.size());
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c == QLatin1Char('\\') && i + 1 < value.size()) {
            const QChar n = value.at(++i);
            switch (n.unicode()) {
            case 's': out += QLatin1Char(' '); break;
            case 'n': out += QLatin1Char('\n'); break;
            case 't': out += QLatin1Char('\t'); break;
            case 'r': out += QLatin1Char('\r'); break;
            case '\\': out += QLatin1Char('\\'); break;
            default: out += QLatin1Char('\\'); out += n; break;
            }
        } else {
            out += c;
        }
    }
    return out;
}

/* Minimal INI reader for .desktop and mimeapps.list files. QSettings is not
 * usable here: it treats '/' in keys (every MIME type) as a group separator. */
IniData readIni(const QString &path)
{
    IniData data;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return data;
    QString group;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            group = line.mid(1, line.size() - 2);
            continue;
        }
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0 || group.isEmpty())
            continue;
        const QString key = line.left(eq).trimmed();
        Group &g = data[group];
        if (!g.contains(key))
            g.insert(key, line.mid(eq + 1).trimmed());
    }
    return data;
}

QStringList splitList(const QString &value)
{
    QStringList out;
    const QStringList parts = value.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &p : parts) {
        const QString t = p.trimmed();
        if (!t.isEmpty())
            out << t;
    }
    return out;
}

QString localized(const Group &g, const QString &key)
{
    const QString loc = QLocale::system().name(); // es_AR
    const QString lang = loc.section(QLatin1Char('_'), 0, 0);
    const QStringList candidates = {
        key + QLatin1Char('[') + loc + QLatin1Char(']'),
        key + QLatin1Char('[') + lang + QLatin1Char(']'),
        key
    };
    for (const QString &k : candidates) {
        const QString v = g.value(k);
        if (!v.isEmpty())
            return unescapeValue(v);
    }
    return QString();
}

bool boolValue(const Group &g, const QString &key, bool def = false)
{
    if (!g.contains(key))
        return def;
    const QString v = g.value(key).trimmed().toLower();
    return v == QLatin1String("true") || v == QLatin1String("1");
}

/* Exec tokenizer (Desktop Entry spec, "The Exec key"). */
QStringList splitExec(const QString &exec)
{
    QStringList args;
    QString cur;
    bool inQuotes = false;
    bool hasToken = false;
    for (int i = 0; i < exec.size(); ++i) {
        const QChar c = exec.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('\\') && i + 1 < exec.size()) {
                const QChar n = exec.at(i + 1);
                if (n == QLatin1Char('"') || n == QLatin1Char('`') ||
                    n == QLatin1Char('$') || n == QLatin1Char('\\')) {
                    cur += n;
                    ++i;
                    continue;
                }
                cur += c;
            } else if (c == QLatin1Char('"')) {
                inQuotes = false;
            } else {
                cur += c;
            }
            continue;
        }
        if (c == QLatin1Char('"')) {
            inQuotes = true;
            hasToken = true;
        } else if (c.isSpace()) {
            if (hasToken) {
                args << cur;
                cur.clear();
                hasToken = false;
            }
        } else if (c == QLatin1Char('\\') && i + 1 < exec.size()) {
            cur += exec.at(++i);
            hasToken = true;
        } else {
            cur += c;
            hasToken = true;
        }
    }
    if (hasToken)
        args << cur;
    return args;
}

bool programExists(const QString &program)
{
    if (program.isEmpty())
        return false;
    if (program.startsWith(QLatin1Char('/')))
        return QFileInfo(program).isExecutable();
    return !QStandardPaths::findExecutable(program).isEmpty();
}

QStringList applicationDirs()
{
    QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    return dirs;
}

QStringList mimeappsFiles()
{
    QStringList files;
    QStringList desktops;
    const QString current = qEnvironmentVariable("XDG_CURRENT_DESKTOP");
    for (const QString &d : current.split(QLatin1Char(':'), Qt::SkipEmptyParts))
        desktops << d.toLower();

    const QStringList configDirs = QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation);
    for (const QString &dir : configDirs) {
        for (const QString &d : desktops)
            files << dir + QLatin1Char('/') + d + QStringLiteral("-mimeapps.list");
        files << dir + QStringLiteral("/mimeapps.list");
    }
    for (const QString &dir : applicationDirs()) {
        for (const QString &d : desktops)
            files << dir + QLatin1Char('/') + d + QStringLiteral("-mimeapps.list");
        files << dir + QStringLiteral("/mimeapps.list");
        files << dir + QStringLiteral("/defaults.list");
    }
    return files;
}

struct Database
{
    QHash<QString, DesktopApp> apps;           // by id
    QHash<QString, QStringList> byMime;        // MimeType= index, sorted by name
    QList<IniData> mimeapps;                   // in priority order
    QHash<QString, qint64> stamps;             // dir/file -> mtime
};

QMutex g_mutex;
Database *g_db = nullptr;

qint64 stampOf(const QString &path)
{
    const QFileInfo fi(path);
    return fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : -1;
}

QHash<QString, qint64> currentStamps()
{
    QHash<QString, qint64> s;
    for (const QString &dir : applicationDirs())
        s.insert(dir, stampOf(dir));
    for (const QString &f : mimeappsFiles())
        s.insert(f, stampOf(f));
    return s;
}

Database *buildDatabase()
{
    auto *db = new Database;
    db->stamps = currentStamps();

    for (const QString &dir : applicationDirs()) {
        QDirIterator it(dir, {QStringLiteral("*.desktop")}, QDir::Files,
                        QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
        while (it.hasNext()) {
            const QString path = it.next();
            QString id = QDir(dir).relativeFilePath(path);
            id.replace(QLatin1Char('/'), QLatin1Char('-'));
            if (db->apps.contains(id))
                continue; // higher priority directory already provided it
            DesktopApp app = MimeApps::loadDesktopFile(path, id);
            if (app.hidden) {
                db->apps.insert(id, app); // still shadows lower directories
                continue;
            }
            db->apps.insert(id, app);
        }
    }

    for (auto it = db->apps.cbegin(); it != db->apps.cend(); ++it) {
        const DesktopApp &app = it.value();
        if (app.hidden || !app.isValid())
            continue;
        for (const QString &m : app.mimeTypes)
            db->byMime[m] << app.id;
    }
    for (auto it = db->byMime.begin(); it != db->byMime.end(); ++it) {
        std::sort(it.value().begin(), it.value().end(), [db](const QString &a, const QString &b) {
            return QString::localeAwareCompare(db->apps.value(a).name, db->apps.value(b).name) < 0;
        });
    }

    for (const QString &f : mimeappsFiles()) {
        if (QFileInfo::exists(f))
            db->mimeapps << readIni(f);
    }
    return db;
}

Database *database()
{
    // Caller holds g_mutex.
    if (g_db && g_db->stamps == currentStamps())
        return g_db;
    delete g_db;
    g_db = buildDatabase();
    return g_db;
}

bool appUsable(const DesktopApp &app)
{
    if (!app.isValid() || app.hidden)
        return false;
    if (!app.tryExec.isEmpty() && !programExists(app.tryExec))
        return false;
    const QStringList argv = splitExec(app.exec);
    return !argv.isEmpty() && programExists(argv.first());
}

QStringList mimeChain(const QString &mime)
{
    QStringList chain;
    if (mime.isEmpty())
        return chain;
    chain << mime;
    QMimeDatabase mdb;
    const QMimeType t = mdb.mimeTypeForName(mime);
    if (t.isValid()) {
        if (t.name() != mime)
            chain << t.name();
        for (const QString &a : t.aliases())
            if (!chain.contains(a)) chain << a;
        for (const QString &p : t.allAncestors()) {
            // Every type inherits octet-stream; hex editors for all files are noise.
            if (p == QLatin1String("application/octet-stream") && mime != p)
                continue;
            if (!chain.contains(p)) chain << p;
        }
    }
    return chain;
}

QString findExecutableArg(const QString &program)
{
    if (program.startsWith(QLatin1Char('/')))
        return program;
    const QString found = QStandardPaths::findExecutable(program);
    return found.isEmpty() ? program : found;
}

/* Expand field codes for one invocation. */
QStringList expandExec(const DesktopApp &app, const QStringList &files, bool *consumed)
{
    QStringList out;
    *consumed = false;
    const QStringList args = splitExec(app.exec);
    for (const QString &arg : args) {
        if (arg == QLatin1String("%F")) { out << files; *consumed = true; continue; }
        if (arg == QLatin1String("%U")) {
            for (const QString &f : files) out << QUrl::fromLocalFile(f).toString(QUrl::FullyEncoded);
            *consumed = true;
            continue;
        }
        if (arg == QLatin1String("%f")) { if (!files.isEmpty()) out << files.first(); *consumed = true; continue; }
        if (arg == QLatin1String("%u")) {
            if (!files.isEmpty()) out << QUrl::fromLocalFile(files.first()).toString(QUrl::FullyEncoded);
            *consumed = true;
            continue;
        }
        if (arg == QLatin1String("%i")) {
            if (!app.icon.isEmpty()) out << QStringLiteral("--icon") << app.icon;
            continue;
        }
        QString expanded;
        bool drop = false;
        for (int i = 0; i < arg.size(); ++i) {
            const QChar c = arg.at(i);
            if (c != QLatin1Char('%') || i + 1 >= arg.size()) { expanded += c; continue; }
            const QChar code = arg.at(++i);
            switch (code.unicode()) {
            case '%': expanded += QLatin1Char('%'); break;
            case 'f': if (!files.isEmpty()) expanded += files.first(); else drop = true; *consumed = true; break;
            case 'u': if (!files.isEmpty()) expanded += QUrl::fromLocalFile(files.first()).toString(QUrl::FullyEncoded); else drop = true; *consumed = true; break;
            case 'c': expanded += app.name; break;
            case 'k': expanded += app.path; break;
            default: break; // %d %D %n %N %v %m and unknown codes are removed
            }
        }
        if (!drop && !expanded.isEmpty())
            out << expanded;
    }
    return out;
}

bool execUsesSingleFile(const QString &exec)
{
    return exec.contains(QLatin1String("%f")) || exec.contains(QLatin1String("%u"));
}

bool execTakesFiles(const QString &exec)
{
    return execUsesSingleFile(exec) || exec.contains(QLatin1String("%F")) ||
           exec.contains(QLatin1String("%U"));
}

QString actionTargetFor(const QString &path)
{
    const QFileInfo fi(path);
    if (fi.isSymLink()) return QStringLiteral("links");
    if (fi.isDir()) return QStringLiteral("directories");
    return QStringLiteral("files");
}

bool tokenListContains(const QString &list, const QString &token)
{
    if (list.trimmed().isEmpty())
        return true;
    const QStringList values = list.split(QRegularExpression(QStringLiteral("[;,\\s]+")), Qt::SkipEmptyParts);
    for (const QString &v : values)
        if (v == token || v == QLatin1String("all") || v == QLatin1String("*"))
            return true;
    return false;
}

bool mimeListMatches(const QString &list, const QString &mime)
{
    if (list.trimmed().isEmpty())
        return true;
    const QStringList values = list.split(QRegularExpression(QStringLiteral("[;,\\s]+")), Qt::SkipEmptyParts);
    for (const QString &v : values) {
        if (v == QLatin1String("*") || v == QLatin1String("*/*") || v == mime)
            return true;
        if (v.endsWith(QLatin1String("/*")) && mime.startsWith(v.left(v.size() - 1)))
            return true;
    }
    return false;
}

} // namespace

QIcon DesktopApp::qicon() const
{
    if (icon.isEmpty())
        return QIcon::fromTheme(QStringLiteral("application-x-executable"));
    if (icon.startsWith(QLatin1Char('/')))
        return QIcon(icon);
    QIcon themed = QIcon::fromTheme(icon);
    if (!themed.isNull())
        return themed;
    const QString base = QFileInfo(icon).completeBaseName();
    themed = QIcon::fromTheme(base);
    if (!themed.isNull())
        return themed;
    for (const QString &ext : {QStringLiteral(".png"), QStringLiteral(".svg"), QStringLiteral(".xpm")}) {
        const QString pix = QStringLiteral("/usr/share/pixmaps/") + base + ext;
        if (QFileInfo::exists(pix))
            return QIcon(pix);
    }
    return QIcon::fromTheme(QStringLiteral("application-x-executable"));
}

namespace MimeApps {

QString mimeTypeForPath(const QString &path)
{
    const QFileInfo fi(path);
    if (fi.isDir())
        return QStringLiteral("inode/directory");
    QMimeDatabase db;
    return db.mimeTypeForFile(fi).name();
}

QString commonMimeType(const QStringList &paths)
{
    QString common;
    for (const QString &p : paths) {
        const QString m = mimeTypeForPath(p);
        if (common.isEmpty())
            common = m;
        else if (common != m)
            return QString();
    }
    return common;
}

DesktopApp loadDesktopFile(const QString &path, const QString &id)
{
    DesktopApp app;
    const IniData ini = readIni(path);
    const Group g = ini.value(QStringLiteral("Desktop Entry"));
    if (g.isEmpty())
        return app;
    const QString type = g.value(QStringLiteral("Type"));
    if (!type.isEmpty() && type != QLatin1String("Application"))
        return app;
    app.id = id.isEmpty() ? QFileInfo(path).fileName() : id;
    app.path = path;
    app.name = localized(g, QStringLiteral("Name"));
    if (app.name.isEmpty())
        app.name = QFileInfo(path).completeBaseName();
    app.icon = unescapeValue(g.value(QStringLiteral("Icon")));
    app.exec = unescapeValue(g.value(QStringLiteral("Exec")));
    app.tryExec = unescapeValue(g.value(QStringLiteral("TryExec")));
    app.mimeTypes = splitList(g.value(QStringLiteral("MimeType")));
    app.terminal = boolValue(g, QStringLiteral("Terminal"));
    app.noDisplay = boolValue(g, QStringLiteral("NoDisplay"));
    app.hidden = boolValue(g, QStringLiteral("Hidden"));

    const Group a = ini.value(QStringLiteral("X-ROX-Filer Action"));
    if (!a.isEmpty()) {
        app.actionMimeTypes = a.value(QStringLiteral("MimeTypes"));
        app.actionTargets = a.value(QStringLiteral("Targets"));
        app.actionSelection = a.value(QStringLiteral("Selection"));
        app.actionEnabled = !a.contains(QStringLiteral("Enabled")) || boolValue(a, QStringLiteral("Enabled"));
    }
    return app;
}

DesktopApp defaultAppForMime(const QString &mime)
{
    QMutexLocker lock(&g_mutex);
    Database *db = database();
    for (const QString &m : mimeChain(mime)) {
        for (const IniData &ini : std::as_const(db->mimeapps)) {
            const QStringList ids = splitList(ini.value(QStringLiteral("Default Applications")).value(m));
            for (const QString &id : ids) {
                const DesktopApp app = db->apps.value(id);
                if (appUsable(app))
                    return app;
            }
        }
    }
    return DesktopApp();
}

QList<DesktopApp> appsForMime(const QString &mime)
{
    QList<DesktopApp> result;
    if (mime.isEmpty())
        return result;

    const DesktopApp def = defaultAppForMime(mime);

    QMutexLocker lock(&g_mutex);
    Database *db = database();
    QSet<QString> seen;
    QSet<QString> removed;
    const QStringList chain = mimeChain(mime);
    for (const IniData &ini : std::as_const(db->mimeapps)) {
        for (const QString &m : chain)
            for (const QString &id : splitList(ini.value(QStringLiteral("Removed Associations")).value(m)))
                removed.insert(id);
    }

    auto add = [&](const QString &id) {
        if (id.isEmpty() || seen.contains(id) || removed.contains(id))
            return;
        const DesktopApp app = db->apps.value(id);
        if (!appUsable(app))
            return;
        seen.insert(id);
        result << app;
    };

    if (def.isValid()) {
        seen.insert(def.id);
        result << def;
    }
    for (const QString &m : chain) {
        for (const IniData &ini : std::as_const(db->mimeapps))
            for (const QString &id : splitList(ini.value(QStringLiteral("Added Associations")).value(m)))
                add(id);
        for (const QString &id : db->byMime.value(m))
            add(id);
    }
    return result;
}

QList<DesktopApp> allApps()
{
    QMutexLocker lock(&g_mutex);
    Database *db = database();
    QList<DesktopApp> result;
    for (auto it = db->apps.cbegin(); it != db->apps.cend(); ++it)
        if (appUsable(it.value()) && !it.value().noDisplay)
            result << it.value();
    std::sort(result.begin(), result.end(), [](const DesktopApp &a, const DesktopApp &b) {
        return QString::localeAwareCompare(a.name, b.name) < 0;
    });
    return result;
}

bool setDefaultApp(const QString &mime, const QString &desktopId, QString *error)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/mimeapps.list");

    QStringList lines;
    {
        QFile in(path);
        if (in.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!in.atEnd())
                lines << QString::fromUtf8(in.readLine()).remove(QLatin1Char('\n'));
        }
    }

    auto upsert = [&lines](const QString &group, const QString &key, const QString &value, bool prepend) {
        const QString header = QLatin1Char('[') + group + QLatin1Char(']');
        int start = lines.indexOf(header);
        if (start < 0) {
            if (!lines.isEmpty() && !lines.last().isEmpty())
                lines << QString();
            lines << header << key + QLatin1Char('=') + value + QLatin1Char(';');
            return;
        }
        int end = start + 1;
        while (end < lines.size() && !lines.at(end).startsWith(QLatin1Char('[')))
            ++end;
        for (int i = start + 1; i < end; ++i) {
            const QString line = lines.at(i);
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq > 0 && line.left(eq).trimmed() == key) {
                QStringList ids = splitList(line.mid(eq + 1));
                ids.removeAll(value);
                if (prepend) ids.prepend(value); else ids = {value};
                lines[i] = key + QLatin1Char('=') + ids.join(QLatin1Char(';')) + QLatin1Char(';');
                return;
            }
        }
        int insertAt = end;
        while (insertAt > start + 1 && lines.at(insertAt - 1).trimmed().isEmpty())
            --insertAt;
        lines.insert(insertAt, key + QLatin1Char('=') + value + QLatin1Char(';'));
    };

    upsert(QStringLiteral("Default Applications"), mime, desktopId, false);
    upsert(QStringLiteral("Added Associations"), mime, desktopId, true);

    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = out.errorString();
        return false;
    }
    out.write(lines.join(QLatin1Char('\n')).toUtf8());
    out.write("\n");
    if (!out.commit()) {
        if (error) *error = out.errorString();
        return false;
    }
    invalidateCache();
    return true;
}

DesktopApp createCustomApp(const QString &command, const QString &mime, QString *error)
{
    const QString trimmed = command.trimmed();
    const QStringList argv = splitExec(trimmed);
    if (argv.isEmpty()) {
        if (error) *error = QCoreApplication::translate("MimeApps", "The command is empty.");
        return DesktopApp();
    }
    if (!programExists(argv.first())) {
        if (error) *error = QCoreApplication::translate("MimeApps", "Program not found: %1").arg(argv.first());
        return DesktopApp();
    }
    QString exec = trimmed;
    if (!execTakesFiles(exec))
        exec += QStringLiteral(" %F");

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                        + QStringLiteral("/applications");
    QDir().mkpath(dir);
    const QByteArray hash = QCryptographicHash::hash(exec.toUtf8(), QCryptographicHash::Md5).toHex().left(10);
    const QString id = QStringLiteral("rox-filer2-custom-") + QString::fromLatin1(hash) + QStringLiteral(".desktop");
    const QString path = dir + QLatin1Char('/') + id;

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return DesktopApp();
    }
    QTextStream ts(&f);
    ts << "[Desktop Entry]\n"
       << "Type=Application\n"
       << "Name=" << QFileInfo(argv.first()).fileName() << "\n"
       << "Exec=" << exec << "\n"
       << "NoDisplay=true\n"
       << "MimeType=" << mime << ";\n";
    ts.flush();
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return DesktopApp();
    }
    invalidateCache();
    return loadDesktopFile(path, id);
}

QString findTerminal()
{
    const QStringList terminals = {
        QStringLiteral("x-terminal-emulator"), QStringLiteral("konsole"),
        QStringLiteral("xterm"), QStringLiteral("urxvt")
    };
    for (const QString &t : terminals)
        if (!QStandardPaths::findExecutable(t).isEmpty())
            return t;
    return QString();
}

QStringList wrapInTerminal(const QStringList &argv)
{
    const QString term = findTerminal();
    if (term.isEmpty())
        return QStringList();
    return QStringList{term, QStringLiteral("-e")} + argv;
}

bool launch(const DesktopApp &app, const QStringList &paths,
            const QString &workingDir, QString *error)
{
    if (!app.isValid()) {
        if (error) *error = QCoreApplication::translate("MimeApps", "Invalid application.");
        return false;
    }

    QList<QStringList> invocations;
    if (execUsesSingleFile(app.exec) && paths.size() > 1) {
        for (const QString &p : paths) {
            bool consumed = false;
            invocations << expandExec(app, {p}, &consumed);
        }
    } else {
        bool consumed = false;
        QStringList argv = expandExec(app, paths, &consumed);
        if (!consumed)
            argv << paths; // Exec without field codes: pass files anyway
        invocations << argv;
    }

    bool ok = true;
    for (QStringList argv : std::as_const(invocations)) {
        if (argv.isEmpty())
            continue;
        if (app.terminal) {
            argv = wrapInTerminal(argv);
            if (argv.isEmpty()) {
                if (error) *error = QCoreApplication::translate("MimeApps", "No terminal emulator was found.");
                return false;
            }
        }
        const QString program = findExecutableArg(argv.takeFirst());
        const QString wd = workingDir.isEmpty() ? QDir::homePath() : workingDir;
        if (!QProcess::startDetached(program, argv, wd)) {
            if (error) *error = QCoreApplication::translate("MimeApps", "Could not start %1.").arg(program);
            ok = false;
        }
    }
    return ok;
}

QString customActionsDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/rox-filer/actions");
}

QList<DesktopApp> customActionsFor(const QStringList &paths)
{
    QList<DesktopApp> result;
    if (paths.isEmpty())
        return result;
    QDir dir(customActionsDir());
    const QStringList files = dir.entryList({QStringLiteral("*.desktop")}, QDir::Files, QDir::Name);
    for (const QString &leaf : files) {
        DesktopApp a = loadDesktopFile(dir.filePath(leaf), leaf);
        if (!a.isValid() || !a.actionEnabled)
            continue;
        if (a.actionSelection == QLatin1String("single") && paths.size() != 1)
            continue;
        if (a.actionSelection == QLatin1String("multiple") && paths.size() < 2)
            continue;
        bool matches = true;
        for (const QString &p : paths) {
            if (!tokenListContains(a.actionTargets, actionTargetFor(p)) ||
                !mimeListMatches(a.actionMimeTypes, mimeTypeForPath(p))) {
                matches = false;
                break;
            }
        }
        if (matches)
            result << a;
    }
    return result;
}

void invalidateCache()
{
    QMutexLocker lock(&g_mutex);
    delete g_db;
    g_db = nullptr;
}

} // namespace MimeApps
