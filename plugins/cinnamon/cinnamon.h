#ifndef CINNAMONPLUGIN_H
#define CINNAMONPLUGIN_H

#include <QObject>
#include <QQueue>

#include "deintegrationinterface.h"
#include "notificationinterface.h"
#include "qtnoteplugininterface.h"
#include "stickynotesintegrationinterface.h"

class QWindow;

namespace QtNote {

class PluginHostInterface;

class CinnamonPlugin : public QObject,
                       public PluginInterface,
                       DEIntegrationInterface,
                       public NotificationInterface,
                       public StickyNotesIntegrationInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.rion-soft.QtNote.Cinnamon")
    Q_INTERFACES(QtNote::PluginInterface QtNote::DEIntegrationInterface QtNote::NotificationInterface
                                                                        QtNote::StickyNotesIntegrationInterface)
public:
    explicit CinnamonPlugin(QObject *parent = nullptr);

    int            metadataVersion() const override;
    PluginMetadata metadata() override;
    void           setHost(PluginHostInterface *host) override;

    void                        notifyError(const QString &message) override;
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
    void ensureAppletEnabled();

private:
    bool isAppletInstalled() const;
    bool isAppletEnabled() const;
    bool enableApplet() const;
    bool isDeskletInstalled() const;

    PluginHostInterface *host = nullptr;
    QQueue<QString>      pendingWindowGeometryKeys;
    bool                 geometryBridgeReady = false;
};

} // namespace QtNote

#endif // CINNAMONPLUGIN_H
