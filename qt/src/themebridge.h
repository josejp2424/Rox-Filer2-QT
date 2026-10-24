#pragma once

// Only static colour values cross the privilege boundary; Qt never loads
// configuration files, stylesheets or code from the caller's home as root.
#include <QPalette>
#include <QColor>
#include <QString>
#include <QStringList>

namespace SonicThemeBridge {
inline constexpr QPalette::ColorRole roles[] = {
    QPalette::Window, QPalette::WindowText, QPalette::Base,
    QPalette::AlternateBase, QPalette::Text, QPalette::Button,
    QPalette::ButtonText, QPalette::BrightText, QPalette::Highlight,
    QPalette::HighlightedText, QPalette::ToolTipBase, QPalette::ToolTipText,
    QPalette::Link, QPalette::LinkVisited, QPalette::PlaceholderText,
    QPalette::Light, QPalette::Midlight, QPalette::Dark,
    QPalette::Mid, QPalette::Shadow
};
inline constexpr QPalette::ColorGroup groups[] = {
    QPalette::Active, QPalette::Inactive, QPalette::Disabled
};
inline constexpr int count = int(sizeof(roles) / sizeof(roles[0])) * 3;

inline QString encode(const QPalette &palette)
{
    QStringList result;
    result.reserve(count);
    for (const auto group : groups) {
        for (const auto role : roles) {
            const quint32 rgba = palette.color(group, role).rgba();
            result.push_back(QString::number(rgba, 16).rightJustified(8, QLatin1Char('0')));
        }
    }
    return result.join(QLatin1Char(','));
}

inline bool apply(QPalette &palette, const QString &value)
{
    if (value.size() != count * 8 + count - 1)
        return false;
    const QStringList colors = value.split(QLatin1Char(','));
    if (colors.size() != count)
        return false;
    int index = 0;
    for (const auto group : groups) {
        for (const auto role : roles) {
            const QString &token = colors.at(index++);
            if (token.size() != 8) return false;
            for (const QChar c : token) {
                const ushort x = c.unicode();
                if (!((x >= '0' && x <= '9') || (x >= 'a' && x <= 'f') || (x >= 'A' && x <= 'F')))
                    return false;
            }
            bool ok = false;
            const quint32 rgba = token.toUInt(&ok, 16);
            if (!ok) return false;
            palette.setColor(group, role, QColor::fromRgba(rgba));
        }
    }
    return true;
}
} // namespace SonicThemeBridge
