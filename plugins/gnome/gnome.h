#ifndef GNOMEPLUGIN_H
#define GNOMEPLUGIN_H

#include <QObject>
#include <QQueue>

#include "anykeepplugininterface.h"
#include "deintegrationinterface.h"
#include "notificationinterface.h"
#include "stickynotesintegrationinterface.h"

class QWindow;

namespace AnyKeep {

class PluginHostInterface;

class GnomePlugin : public QObject,
                    public PluginInterface,
                    DEIntegrationInterface,
                    public NotificationInterface,
                    public StickyNotesIntegrationInterface {
    Q_OBJECT
#include "gnome_plugin_metadata.inc"
    Q_INTERFACES(AnyKeep::PluginInterface AnyKeep::DEIntegrationInterface AnyKeep::NotificationInterface
                                                                          AnyKeep::StickyNotesIntegrationInterface)
public:
    explicit GnomePlugin(QObject *parent = 0);
    void setHost(PluginHostInterface *host) override;

    void notifyError(const QString &msg) override;

    void                        activateWindow(QWindow *window) override;
    WindowGeometryRestoreResult restoreWindowGeometry(QWindow *window, const QString &key) override;
    bool                        saveWindowGeometry(QWindow *window, const QString &key) override;
    bool                        removeWindowGeometry(const QString &key) override;
    QString                     takePendingWindowGeometryKey() override;
    void                        windowGeometryBridgeReady() override;

    bool  isStickyNotesAvailable() const override;
    bool  presentStickyNote(const QUuid &stickyId, const QRect &preferredGeometry) override;
    bool  dismissStickyNote(const QUuid &stickyId) override;
    QUuid stickyNoteIdForPresentation(const QString &presentationId) const override;

private slots:
    void askEnableShellExtension();

private:
    bool    isShellExtensionInstalled() const;
    bool    isShellExtensionEnabled() const;
    bool    enableShellExtension() const;
    bool    geometryExtensionAvailable();
    QString stickyNotesSettings() const;
    bool    setStickyNotesSettings(const QString &json) const;

    PluginHostInterface *host = nullptr;
    QQueue<QString>      pendingWindowGeometryKeys;
    bool                 shellExtensionEnabled = false;
    bool                 geometryBridgeReady   = false;
};

} // namespace AnyKeep

#endif // GNOMEPLUGIN_H
