#ifndef TRAYICON_H
#define TRAYICON_H

#include <QHash>
#include <QObject>
#include <QPointer>

#include "deintegrationinterface.h"
#include "globalshortcutsinterface.h"
#include "notificationinterface.h"
#include "qtnoteplugininterface.h"
#include "stickynotesintegrationinterface.h"
#include "trayinterface.h"

class QxtGlobalShortcut;

class QWindow;

namespace QtNote {

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
    Q_PLUGIN_METADATA(IID "com.rion-soft.QtNote.BaseIntegration")
    Q_INTERFACES(
        QtNote::PluginInterface QtNote::DEIntegrationInterface QtNote::TrayInterface QtNote::GlobalShortcutsInterface
            QtNote::NotificationInterface QtNote::StickyNotesIntegrationInterface QtNote::StickyNotesHostInterface)
public:
    explicit BaseIntegration(QObject *parent = 0);
    ~BaseIntegration() override;

    int            metadataVersion() const;
    PluginMetadata metadata();
    void           setHost(PluginHostInterface *host);

    void      activateWindow(QWindow *window);
    TrayImpl *initTray(Main *qtnote);
    void      notifyError(const QString &message);

    bool registerGlobalShortcut(const QString &id, const QKeySequence &key, QAction *action);
    bool updateGlobalShortcut(const QString &id, const QKeySequence &key);
    void setGlobalShortcutEnabled(const QString &id, bool enabled = true);

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

} // namespace QtNote

#endif // TRAYICON_H
