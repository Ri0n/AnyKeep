#ifndef KDEINTEGRATION_H
#define KDEINTEGRATION_H

#include <QObject>
#include <QQueue>

#include "actionnotificationinterface.h"
#include "deintegrationinterface.h"
#include "globalshortcutsinterface.h"
#include "notificationinterface.h"
#include "qtnoteplugininterface.h"
#include "settingsproviderinterface.h"
#include "spellcheckproviderinterface.h"
#include "stickynotesintegrationinterface.h"
#include "trayinterface.h"

class KAction;

class QWindow;

namespace QtNote {

class PluginHostInterface;

class KDEIntegration : public QObject,
                       public PluginInterface,
                       public NotificationInterface,
                       public ActionNotificationInterface,
                       public TrayInterface,
                       public DEIntegrationInterface,
                       public GlobalShortcutsInterface,
                       public StickyNotesIntegrationInterface,
                       public SpellCheckProviderInterface,
                       public SettingsProviderInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.rion-soft.QtNote.KdeTrayIcon")
    Q_INTERFACES(
        QtNote::PluginInterface QtNote::TrayInterface QtNote::DEIntegrationInterface QtNote::GlobalShortcutsInterface
            QtNote::NotificationInterface QtNote::ActionNotificationInterface QtNote::StickyNotesIntegrationInterface
                QtNote::SpellCheckProviderInterface QtNote::SettingsProviderInterface)
public:
    explicit KDEIntegration(QObject *parent = 0);

    int                                 metadataVersion() const override;
    virtual PluginMetadata              metadata() override;
    void                                setHost(PluginHostInterface *host) override;
    std::shared_ptr<SpellCheckProvider> spellCheckProvider() override;
    QUrl                                settingsComponent() const override;
    SettingsController                 *createSettingsController(QObject *parent) override;

    TrayImpl                   *initTray(Main *qtnote) override;
    void                        notifyError(const QString &msg) override;
    void                        notify(const QString &title, const QString &message, const QString &actionText,
                                       std::function<void()> action) override;
    void                        activateWindow(QWindow *window) override;
    WindowGeometryRestoreResult restoreWindowGeometry(QWindow *window, const QString &key) override;
    bool                        saveWindowGeometry(QWindow *window, const QString &key) override;
    bool                        removeWindowGeometry(const QString &key) override;
    QString                     takePendingWindowGeometryKey() override;

    bool  isStickyNotesAvailable() const override;
    bool  presentStickyNote(const QUuid &stickyId, const QRect &preferredGeometry) override;
    bool  dismissStickyNote(const QUuid &stickyId) override;
    QUuid stickyNoteIdForPresentation(const QString &presentationId) const override;

    bool registerGlobalShortcut(const QString &id, const QKeySequence &key, QAction *action) override;
    bool updateGlobalShortcut(const QString &id, const QKeySequence &key) override;
    void setGlobalShortcutEnabled(const QString &id, bool enabled = true) override;

signals:

public slots:

private:
    bool ensureWaylandGeometryScript();
    bool evaluatePlasmaScript(const QString &script, QString *output = nullptr) const;

    QHash<QString, QAction *> _shortcuts;
    QQueue<QString>           _pendingWindowGeometryKeys;
    bool                      _waylandGeometryScriptAvailable = false;
};

} // namespace QtNote

#endif // KDEINTEGRATION_H
