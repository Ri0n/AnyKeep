/*
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "freedesktopnotifier.h"

#include <QtGlobal>

#ifdef ANYKEEP_DBUS_AVAILABLE
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QStringList>
#include <QVariantMap>
#endif

namespace AnyKeep {

bool FreedesktopNotifier::notifyError(const QString &summary, const QString &body)
{
#ifdef ANYKEEP_DBUS_AVAILABLE
    auto bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;

    QVariantMap hints;
    hints.insert(QLatin1String("urgency"), uchar(2));

    auto message = QDBusMessage::createMethodCall(
        QLatin1String("org.freedesktop.Notifications"), QLatin1String("/org/freedesktop/Notifications"),
        QLatin1String("org.freedesktop.Notifications"), QLatin1String("Notify"));
    message << QLatin1String("AnyKeep") // app_name
            << uint(0)                 // replaces_id
            << QLatin1String("anykeep") // app_icon
            << summary                 // summary
            << body                    // body
            << QStringList()           // actions
            << hints                   // hints
            << 5000;                   // expire_timeout

    QDBusReply<uint> reply = bus.call(message, QDBus::Block, 1500);
    return reply.isValid();
#else
    Q_UNUSED(summary)
    Q_UNUSED(body)
    return false;
#endif
}

} // namespace AnyKeep
