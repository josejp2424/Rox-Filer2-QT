#include "roxoptions.h"

#include <QFile>
#include <QHash>
#include <QSettings>
#include <QStandardPaths>
#include <QXmlStreamReader>

namespace RoxOptions {

namespace {

constexpr auto kClickKey = "behavior/clickMode";
constexpr auto kNewButton1Key = "behavior/newButton1";
constexpr auto kDclickKey = "behavior/dclickResizes";

QHash<QString, QString> loadGtkOptions(const QString &path)
{
    QHash<QString, QString> values;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return values;
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("Option")) {
            const QString name = xml.attributes().value(QLatin1String("name")).toString();
            const QString text = xml.readElementText().trimmed();
            if (!name.isEmpty())
                values.insert(name, text);
        }
    }
    return values;
}

bool gtkBool(const QString &name, bool fallback)
{
    const QString value = gtkOptionValue(name);
    if (value.isEmpty())
        return fallback;
    return value != QLatin1String("0");
}

} // namespace

QString gtkOptionsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/rox.sourceforge.net/ROX-Filer/Options");
}

QString gtkOptionValue(const QString &name, const QString &optionsFile)
{
    // The GTK3 file is tiny and only read when a value is not saved by Qt, but
    // do not re-parse it for every click.
    static QHash<QString, QHash<QString, QString>> cache;
    const QString path = optionsFile.isEmpty() ? gtkOptionsPath() : optionsFile;
    auto it = cache.find(path);
    if (it == cache.end())
        it = cache.insert(path, loadGtkOptions(path));
    return it->value(name);
}

ClickMode clickMode()
{
    QSettings settings;
    if (settings.contains(QLatin1String(kClickKey))) {
        const int value = settings.value(QLatin1String(kClickKey)).toInt();
        if (value >= 0 && value <= 2)
            return static_cast<ClickMode>(value);
    }
    // GTK3 semantics (filer.c): bind_single_click_dirs takes priority and makes
    // files need a double click; bind_single_click defaults to on.
    if (gtkBool(QStringLiteral("bind_single_click_dirs"), false))
        return ClickMode::SingleDirs;
    return gtkBool(QStringLiteral("bind_single_click"), true) ? ClickMode::Single : ClickMode::Double;
}

void setClickMode(ClickMode mode)
{
    QSettings().setValue(QLatin1String(kClickKey), static_cast<int>(mode));
}

bool newButton1()
{
    QSettings settings;
    if (settings.contains(QLatin1String(kNewButton1Key)))
        return settings.value(QLatin1String(kNewButton1Key)).toBool();
    return gtkBool(QStringLiteral("bind_new_button_1"), false);
}

void setNewButton1(bool enabled)
{
    QSettings().setValue(QLatin1String(kNewButton1Key), enabled);
}

bool dclickResizes()
{
    QSettings settings;
    if (settings.contains(QLatin1String(kDclickKey)))
        return settings.value(QLatin1String(kDclickKey)).toBool();
    return gtkBool(QStringLiteral("bind_dclick_resizes"), true);
}

void setDclickResizes(bool enabled)
{
    QSettings().setValue(QLatin1String(kDclickKey), enabled);
}

} // namespace RoxOptions
