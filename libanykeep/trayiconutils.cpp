#include "trayiconutils.h"

#include "iconutils.h"

#include <QColor>
#include <QGuiApplication>
#include <QIcon>
#include <QSystemTrayIcon>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif

namespace AnyKeep {

namespace {
    constexpr auto TrayIconName         = "anykeep";
    constexpr auto SymbolicTrayIconName = "anykeep-symbolic";
    constexpr auto ColorTrayIconPath    = ":/icons/trayicon";
    constexpr auto SymbolicTrayIconPath = ":/icons/trayicon-symbolic";

#ifdef Q_OS_WIN
    QIcon windowsTrayIcon()
    {
        const QColor color = IconUtils::isDarkColorScheme() ? Qt::white : Qt::black;
        auto         icon  = IconUtils::tintedSymbolicIcon(QLatin1String(SymbolicTrayIconPath), color);

        return icon.isNull() ? QIcon(ColorTrayIconPath) : icon;
    }
#endif
} // namespace

QString TrayIconUtils::themedTrayIconName() { return QLatin1String(SymbolicTrayIconName); }

QIcon TrayIconUtils::themedTrayIcon()
{
#ifdef Q_OS_WIN
    return windowsTrayIcon();
#else
    return QIcon::fromTheme(SymbolicTrayIconName, QIcon::fromTheme(TrayIconName, QIcon(ColorTrayIconPath)));
#endif
}

void TrayIconUtils::setupSystemTrayIcon(QSystemTrayIcon *trayIcon)
{
    if (!trayIcon)
        return;

    trayIcon->setIcon(themedTrayIcon());

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, trayIcon,
                     [trayIcon] { trayIcon->setIcon(themedTrayIcon()); });
#endif
}

} // namespace AnyKeep
