#include "trayiconutils.h"

#include "iconutils.h"
#include "notemanager.h"
#include "notestorage.h"

#include <QAction>
#include <QColor>
#include <QPainter>
#include <QPixmap>
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

QString TrayIconUtils::connectionSummary()
{
    QStringList lines;
    for (const auto &storage : NoteManager::instance()->prioritizedStorages(true)) {
        if (!storage)
            continue;
        const auto state = storage->connectivityState();
        if (state == NoteStorage::ConnectivityState::NotApplicable)
            continue;
        QString label;
        switch (state) {
        case NoteStorage::ConnectivityState::Online: label = QObject::tr("Online"); break;
        case NoteStorage::ConnectivityState::Offline: label = QObject::tr("Offline"); break;
        case NoteStorage::ConnectivityState::Connecting: label = QObject::tr("Reconnecting"); break;
        case NoteStorage::ConnectivityState::Error: label = QObject::tr("Needs attention"); break;
        case NoteStorage::ConnectivityState::NotApplicable: break;
        }
        lines.append(QObject::tr("%1: %2").arg(storage->name(), label));
    }
    return lines.join(QLatin1Char('\n'));
}

void TrayIconUtils::setupSystemTrayIcon(QSystemTrayIcon *trayIcon, QAction *statusAction)
{
    if (!trayIcon)
        return;

    // Keep the tray in sync with the same storage events used by the models.
    // A local offline cache must not be interpreted as an active XMPP session.
    const auto refresh = [trayIcon, statusAction] {
        const QString summary = connectionSummary();
        trayIcon->setToolTip(summary.isEmpty() ? QObject::tr("AnyKeep")
                                              : QObject::tr("AnyKeep\n%1").arg(summary));
        if (statusAction) {
            statusAction->setVisible(!summary.isEmpty());
            statusAction->setText(QString(summary).replace(QLatin1Char('\n'), QStringLiteral("; ")));
        }

        int priority = 0;
        QColor dot;
        for (const auto &storage : NoteManager::instance()->prioritizedStorages(true)) {
            if (!storage)
                continue;
            int p = 0;
            QColor color;
            switch (storage->connectivityState()) {
            case NoteStorage::ConnectivityState::Online: p = 1; color = QColor("#2c995a"); break;
            case NoteStorage::ConnectivityState::Connecting: p = 3; color = QColor("#b98a32"); break;
            case NoteStorage::ConnectivityState::Offline: p = 2; color = QColor("#858b95"); break;
            case NoteStorage::ConnectivityState::Error: p = 4; color = QColor("#c44a4a"); break;
            case NoteStorage::ConnectivityState::NotApplicable: break;
            }
            if (p > priority) {
                priority = p;
                dot = color;
            }
        }
        const auto baseIcon = themedTrayIcon();
        if (priority == 0) {
            trayIcon->setIcon(baseIcon);
            return;
        }
        QIcon composed;
        for (const int size : { 16, 22, 32, 48, 64 }) {
            QPixmap canvas = baseIcon.pixmap(size, size);
            if (canvas.isNull())
                continue;
            QPainter painter(&canvas);
            painter.setRenderHint(QPainter::Antialiasing);
            const qreal diameter = qMax(6.0, size * 0.36);
            const QRectF badge(size - diameter, size - diameter, diameter, diameter);
            painter.setPen(QPen(QColor("#ffffff"), qMax(1.0, size * 0.055)));
            painter.setBrush(dot);
            painter.drawEllipse(badge.adjusted(1, 1, -1, -1));
            painter.end();
            composed.addPixmap(canvas);
        }
        trayIcon->setIcon(composed.isNull() ? baseIcon : composed);
    };

    const auto manager = NoteManager::instance();
    QObject::connect(manager, &NoteManager::storageAdded, trayIcon, [refresh](const NoteStorage::Ptr &) { refresh(); });
    QObject::connect(manager, &NoteManager::storageRemoved, trayIcon, [refresh](const NoteStorage::Ptr &) { refresh(); });
    QObject::connect(manager, &NoteManager::storageChanged, trayIcon, [refresh](const NoteStorage::Ptr &) { refresh(); });
    QObject::connect(manager, &NoteManager::storageReady, trayIcon, [refresh](const NoteStorage::Ptr &) { refresh(); });
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, trayIcon, refresh);
#endif
    refresh();
}

} // namespace AnyKeep
