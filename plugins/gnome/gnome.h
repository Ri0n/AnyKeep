#ifndef GNOMEPLUGIN_H
#define GNOMEPLUGIN_H

#include <QObject>
#include <QQueue>

#include "deintegrationinterface.h"
#include "notificationinterface.h"
#include "qtnoteplugininterface.h"
#include "stickynotesintegrationinterface.h"

class QWindow;

namespace QtNote {

class PluginHostInterface;

class GnomePlugin : public QObject,
                    public PluginInterface,
                    DEIntegrationInterface,
                    public NotificationInterface,
                    public StickyNotesIntegrationInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.rion-soft.QtNote.Gnome")
    Q_INTERFACES(QtNote::PluginInterface QtNote::DEIntegrationInterface QtNote::NotificationInterface
                                                                        QtNote::StickyNotesIntegrationInterface)
public:
    explicit GnomePlugin(QObject *parent = 0);

    int                    metadataVersion() const override;
    virtual PluginMetadata metadata() override;
    void                   setHost(PluginHostInterface *host) override;

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

} // namespace QtNote

#endif // GNOMEPLUGIN_H
