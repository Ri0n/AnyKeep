#ifndef BASEINTEGRATIONTRAY_H
#define BASEINTEGRATIONTRAY_H

#include <QPointer>

#include <QSystemTrayIcon>
#include <functional>

#include "anykeep.h"
#include "trayimpl.h"

class QMenu;
class QAction;
class QWidget;

namespace AnyKeep {

class PluginHostInterface;

class BaseIntegrationTray : public TrayImpl {
    Q_OBJECT

    friend class BaseIntegration;

    Main                 *anykeep;
    PluginHostInterface  *host;
    QSystemTrayIcon      *tray;
    QMenu                *contextMenu;
    QAction              *actQuit, *actNew, *actAbout, *actOptions, *actManager;
    QPointer<QWidget>     currentPopup;
    std::function<void()> notificationAction;

public:
    explicit BaseIntegrationTray(Main *anykeep, PluginHostInterface *host, QObject *parent = 0);
    ~BaseIntegrationTray();
    void showNotification(const QString &title, const QString &message, const QString &actionText,
                          std::function<void()> action, bool error);

signals:

public slots:

private slots:
    void showNoteList(QSystemTrayIcon::ActivationReason reason);
};

} // namespace AnyKeep

#endif // BASEINTEGRATIONTRAY_H
