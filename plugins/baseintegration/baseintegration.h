#ifndef TRAYICON_H
#define TRAYICON_H

#include <QHash>
#include <QObject>
#include <QPointer>

#include "anykeepplugininterface.h"
#include "deintegrationinterface.h"
#include "globalshortcutsinterface.h"
#include "notificationinterface.h"
#include "stickynotesintegrationinterface.h"
#include "trayinterface.h"

class QxtGlobalShortcut;

class QWindow;

namespace AnyKeep {

class Main;
class StickyNoteWindow;
class StickyNotesManager;

class BaseIntegration : public QObject,
                        public PluginInterface,
                        public DEIntegrationInterface,
                        public TrayInterface,
                        public NotificationInterface,
                        public GlobalShortcutsInterface,
                        public StickyNotesIntegrationInterface,
                        public StickyNotesHostInterface {
    Q_OBJECT
#include "baseintegration_plugin_metadata.inc"
    Q_INTERFACES(AnyKeep::PluginInterface AnyKeep::DEIntegrationInterface AnyKeep::TrayInterface
                     AnyKeep::GlobalShortcutsInterface AnyKeep::NotificationInterface
                         AnyKeep::StickyNotesIntegrationInterface AnyKeep::StickyNotesHostInterface)
public:
    explicit BaseIntegration(QObject *parent = 0);
    ~BaseIntegration() override;
    void setHost(PluginHostInterface *host) override;

    void      activateWindow(QWindow *window) override;
    TrayImpl *initTray(Main *anykeep) override;
    void      notifyError(const QString &message) override;

    bool registerGlobalShortcut(const QString &id, const QKeySequence &key, QAction *action) override;
    bool updateGlobalShortcut(const QString &id, const QKeySequence &key) override;
    void setGlobalShortcutEnabled(const QString &id, bool enabled = true) override;

    void  initializeStickyNotes(StickyNotesServiceInterface *service) override;
    bool  stickyNotesRequireApplicationAutostart() const override { return true; }
    bool  isStickyNotesAvailable() const override;
    bool  presentStickyNote(const QUuid &stickyId, const QRect &preferredGeometry) override;
    bool  dismissStickyNote(const QUuid &stickyId) override;
    QUuid stickyNoteIdForPresentation(const QString &presentationId) const override;

public slots:

private:
    PluginHostInterface                     *host;
    TrayImpl                                *tray;
    QHash<QString, QxtGlobalShortcut *>      _shortcuts;
    QHash<QUuid, QPointer<StickyNoteWindow>> stickyWindows;
    StickyNotesServiceInterface             *stickyNotes = nullptr;
    bool                                     isWayland   = false;
};

} // namespace AnyKeep

#endif // TRAYICON_H
