#include <KGlobalAccel>
#include <KGlobalShortcutInfo>
#include <KNotification>
#include <KWaylandExtras>
#include <KWindowSystem>
#include <kwindowsystem_version.h>

#include <QAction>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QPointer>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>
#include <QWindow>
#include <QtPlugin>

#include "kdeintegration.h"
#include "kdeintegrationtray.h"
#include "pluginhostinterface.h"
#include "settingscontroller.h"
#include "sonnetspellcheckprovider.h"

namespace AnyKeep {

Q_LOGGING_CATEGORY(logKdeIntegration, "anykeep.kdeintegration")

static constexpr auto      anyKeepServiceName = "com.github.ri0n.AnyKeep";
static const QLatin1String stickyPlasmoidId("com.github.ri0n.anykeep.sticky");
static const QLatin1String stickyPresentationsGroup("kdeintegration/stickyPresentations");
static const QLatin1String useSonnetSetting("kdeintegration/useSonnet");

//------------------------------------------------------------
// KDEIntegration
//------------------------------------------------------------
KDEIntegration::KDEIntegration(QObject *parent) : QObject(parent) {}

// The KWin script talks back to this process by the service name. Until the
// name is ours, a started script finds no AnyKeep and gives up.
static bool ownsAnyKeepService()
{
    auto *bus = QDBusConnection::sessionBus().interface();
    if (!bus)
        return false;
    const QDBusReply<QString> owner = bus->serviceOwner(QLatin1String(anyKeepServiceName));
    return owner.isValid() && owner.value() == QDBusConnection::sessionBus().baseService();
}

bool KDEIntegration::ensureWaylandGeometryScript()
{
    if (!waylandWindowTags())
        return false;
    if (_waylandGeometryScriptAvailable)
        return true;
    // A second window can ask again after unloadScript() reports the old
    // script absent but before its deferred destruction has completed.
    if (_waylandGeometryReloadScheduled)
        return false;
    if (!ownsAnyKeepService())
        return false;

#ifdef ANYKEEP_DEVEL_KWIN_SCRIPT
    const QString scriptPath = QStringLiteral(ANYKEEP_DEVEL_KWIN_SCRIPT);
#else
    const QString scriptPath
        = QStandardPaths::locate(QStandardPaths::GenericDataLocation,
                                 QStringLiteral("kwin/scripts/anykeepwindowgeometry/contents/code/main.js"));
#endif
    if (scriptPath.isEmpty()) {
        qCWarning(logKdeIntegration) << "AnyKeep KWin geometry script was not found";
        return false;
    }

    QDBusInterface scripting(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"),
                             QStringLiteral("org.kde.kwin.Scripting"));
    const QString  pluginName = QStringLiteral("anykeepwindowgeometry");
    // KWin unloads script objects via deleteLater(). Defer a reload when an
    // old script is present so a reused ID cannot point to the dying object.
    const QDBusReply<bool> loaded = scripting.call(QStringLiteral("isScriptLoaded"), pluginName);
    if (!loaded.isValid()) {
        qCWarning(logKdeIntegration) << "Cannot query AnyKeep KWin script state:" << loaded.error().message();
        return false;
    }
    if (loaded.value()) {
        const auto unload = scripting.call(QStringLiteral("unloadScript"), pluginName);
        if (unload.type() == QDBusMessage::ErrorMessage) {
            qCWarning(logKdeIntegration) << "Failed to unload old AnyKeep KWin script:" << unload.errorMessage();
            return false;
        }
        _waylandGeometryReloadScheduled = true;
        QTimer::singleShot(250, this, [this] {
            _waylandGeometryReloadScheduled = false;
            ensureWaylandGeometryScript();
        });
        qCInfo(logKdeIntegration) << "Waiting for the previous KWin script object to be destroyed";
        return false;
    }

    const QDBusReply<int> scriptId = scripting.call(QStringLiteral("loadScript"), scriptPath, pluginName);
    if (!scriptId.isValid() || scriptId.value() < 0) {
        qCWarning(logKdeIntegration) << "Failed to load AnyKeep KWin geometry script:" << scriptId.error().message();
        return false;
    }
    const QString  objectPath = QStringLiteral("/Scripting/Script%1").arg(scriptId.value());
    QDBusInterface script(QStringLiteral("org.kde.KWin"), objectPath, QStringLiteral("org.kde.kwin.Script"));
    const auto     runReply         = script.call(QStringLiteral("run"));
    _waylandGeometryScriptAvailable = runReply.type() != QDBusMessage::ErrorMessage;
    if (!_waylandGeometryScriptAvailable)
        qCWarning(logKdeIntegration) << "Failed to run AnyKeep KWin geometry script" << objectPath << ":"
                                     << runReply.errorMessage();
    else
        qCInfo(logKdeIntegration) << "Requested direct KWin geometry script run:" << objectPath;
    return _waylandGeometryScriptAvailable;
}

void KDEIntegration::setHost(PluginHostInterface *)
{
    if (!waylandWindowTags())
        return;
    // Start the KWin script once AnyKeep owns its service name, before the
    // first note window opens: the script reads all note window states once
    // and must have them by then to place a window before it is painted.
    auto      *watcher = new QDBusServiceWatcher(QLatin1String(anyKeepServiceName), QDBusConnection::sessionBus(),
                                                 QDBusServiceWatcher::WatchForRegistration, this);
    const auto start   = [this, watcher] {
        if (ensureWaylandGeometryScript())
            watcher->deleteLater();
    };
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, watcher, start);
    // The name may already be registered when the plugin is set up.
    QTimer::singleShot(0, watcher, start);
}

std::shared_ptr<SpellCheckProvider> KDEIntegration::spellCheckProvider()
{
    QSettings settings;
    if (!settings.value(useSonnetSetting, true).toBool())
        return {};
    auto provider = std::make_shared<SonnetSpellCheckProvider>();
    if (!provider->isValid()) {
        qCWarning(logKdeIntegration) << "Sonnet is enabled but no usable dictionaries were found";
        return {};
    }
    qCInfo(logKdeIntegration) << "Sonnet languages:" << provider->languages();
    return provider;
}

QUrl KDEIntegration::settingsComponent() const { return QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml")); }

SettingsController *KDEIntegration::createSettingsController(QObject *parent)
{
    SettingsController::Field sonnet;
    sonnet.key             = QStringLiteral("useSonnet");
    sonnet.label           = tr("Use Sonnet for spell checking");
    sonnet.description     = tr("The spell checker selection is applied after restarting AnyKeep.");
    sonnet.type            = SettingsController::Boolean;
    sonnet.value           = true;
    sonnet.restartRequired = true;
    return new PersistentSettingsController(QStringLiteral("kdeintegration"), { sonnet }, parent);
}
TrayImpl *KDEIntegration::initTray(Main *anykeep) { return new KDEIntegrationTray(anykeep, this); }

void KDEIntegration::notifyError(const QString &msg)
{
    KNotification::event(KNotification::Error, tr("Error"), msg, QPixmap(":/svg/anykeep"));
}

void KDEIntegration::notify(const QString &title, const QString &message, const QString &actionText,
                            std::function<void()> action)
{
    auto *notification = KNotification::event(KNotification::Notification, title, message, QPixmap(":/svg/anykeep"),
                                              KNotification::CloseOnTimeout);
    if (!actionText.isEmpty() && action) {
        auto *notificationAction = notification->addAction(actionText);
        connect(notificationAction, &KNotificationAction::activated, notification,
                [action = std::move(action)]() { action(); });
    }
}

void KDEIntegration::activateWindow(QWindow *window)
{
    QPointer<QWindow> guarded(window);
    QTimer::singleShot(100, this, [guarded]() {
        if (!guarded)
            return;
        guarded->showNormal();
        guarded->raise();
        if (KWindowSystem::isPlatformWayland())
            KWindowSystem::updateStartupId(guarded);
        guarded->requestActivate();
        KWindowSystem::activateWindow(guarded);
    });
}

// Note windows are placed on Wayland by the companion KWin script, which
// finds a window's geometry by the xdg-toplevel tag carrying its window id.
// Without tags (KF6 < 6.22) a window cannot be told apart from the others,
// so it is only sized by AnyKeep and placed by KWin. On X11 the window
// positions itself.
bool KDEIntegration::waylandWindowTags()
{
#if KWINDOWSYSTEM_VERSION >= QT_VERSION_CHECK(6, 22, 0)
    return KWindowSystem::isPlatformWayland();
#else
    return false;
#endif
}

WindowGeometryRestoreResult KDEIntegration::restoreWindowGeometry(QWindow *window, const QString &key)
{
    if (!window || key.isEmpty() || !waylandWindowTags() || !ensureWaylandGeometryScript())
        return WindowGeometryRestoreResult::Unsupported;
    setWindowIdentity(window, key);
    return WindowGeometryRestoreResult::Pending;
}

void KDEIntegration::setWindowIdentity(QWindow *window, const QString &key)
{
#if KWINDOWSYSTEM_VERSION >= QT_VERSION_CHECK(6, 22, 0)
    // Set before the window is shown, the tag is known to KWin as soon as
    // the window appears, and the script places it before it is painted.
    if (window && waylandWindowTags())
        KWaylandExtras::setXdgToplevelTag(window, key);
#else
    Q_UNUSED(window)
    Q_UNUSED(key)
#endif
}

bool KDEIntegration::isStickyNotesAvailable() const
{
    return !QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                      QLatin1String("plasma/plasmoids/") + stickyPlasmoidId
                                          + QLatin1String("/metadata.json"),
                                      QStandardPaths::LocateFile)
                .isEmpty();
}

bool KDEIntegration::evaluatePlasmaScript(const QString &script, QString *output) const
{
    QDBusInterface      plasmaShell(QStringLiteral("org.kde.plasmashell"), QStringLiteral("/PlasmaShell"),
                                    QStringLiteral("org.kde.PlasmaShell"));
    QDBusReply<QString> reply = plasmaShell.call(QStringLiteral("evaluateScript"), script);
    if (!reply.isValid()) {
        qCWarning(logKdeIntegration) << "Failed to evaluate Plasma script:" << reply.error().message();
        return false;
    }
    if (output)
        *output = reply.value();
    if (reply.value().startsWith(QLatin1String("ERROR:"))) {
        qCWarning(logKdeIntegration).noquote() << "Plasma script failed:" << reply.value();
        return false;
    }
    return true;
}

bool KDEIntegration::presentStickyNote(const QUuid &stickyId, const QRect &preferredGeometry)
{
    if (stickyId.isNull() || !isStickyNotesAvailable())
        return false;

    const QString idText = stickyId.toString(QUuid::WithoutBraces);
    int           screen = 0;
    QPoint        position(100, 100);
    QSize         size(300, 220);
    if (preferredGeometry.isValid()) {
        if (auto *targetScreen = QGuiApplication::screenAt(preferredGeometry.center())) {
            screen   = QGuiApplication::screens().indexOf(targetScreen);
            position = preferredGeometry.topLeft() - targetScreen->geometry().topLeft();
        }
        size = preferredGeometry.size().expandedTo(QSize(220, 140));
    }

    const QString script = QStringLiteral(R"JS(
try {
    var stickyId = "%1";
    var pluginId = "%2";
    var targetScreen = %3;
    var allDesktops = desktops();
    var desktop = null;
    for (var i = 0; i < allDesktops.length; ++i) {
        if (allDesktops[i].screen === targetScreen) {
            desktop = allDesktops[i];
            break;
        }
    }
    if (!desktop && allDesktops.length)
        desktop = allDesktops[0];
    if (!desktop)
        throw new Error("No Plasma desktop containment found");

    var widgets = desktop.widgets();
    var found = false;
    for (var j = 0; j < widgets.length; ++j) {
        if (widgets[j].type !== pluginId && widgets[j].pluginName !== pluginId)
            continue;
        widgets[j].currentConfigGroup = ["General"];
        if (widgets[j].readConfig("stickyId", "") === stickyId) {
            print("OK:" + widgets[j].id);
            found = true;
            break;
        }
    }

    if (!found) {
        var widget = desktop.addWidget(pluginId);
        if (!widget)
            throw new Error("Failed to create sticky note plasmoid");
        widget.currentConfigGroup = ["General"];
        widget.writeConfig("stickyId", stickyId);
        widget.geometry = new QRectF(%4, %5, %6, %7);
        print("OK:" + widget.id);
    }
} catch (error) {
    print("ERROR:" + error);
}
)JS")
                               .arg(idText, stickyPlasmoidId)
                               .arg(screen)
                               .arg(qMax(0, position.x()))
                               .arg(qMax(0, position.y()))
                               .arg(size.width())
                               .arg(size.height());
    QString       output;
    if (!evaluatePlasmaScript(script, &output))
        return false;
    const QString presentationId = output.trimmed().section(QLatin1Char(':'), 1, 1);
    if (presentationId.isEmpty()) {
        qCWarning(logKdeIntegration).noquote() << "Plasma did not return the sticky widget id:" << output;
        return false;
    }
    QSettings settings;
    settings.beginGroup(stickyPresentationsGroup);
    settings.setValue(presentationId, idText);
    return true;
}

bool KDEIntegration::dismissStickyNote(const QUuid &stickyId)
{
    if (stickyId.isNull())
        return false;
    QSettings settings;
    settings.beginGroup(stickyPresentationsGroup);
    QString       presentationId;
    const QString idText = stickyId.toString(QUuid::WithoutBraces);
    for (const auto &key : settings.childKeys()) {
        if (settings.value(key).toString() == idText) {
            presentationId = key;
            break;
        }
    }
    const QString script  = QStringLiteral(R"JS(
try {
    var stickyId = "%1";
    var pluginId = "%2";
    var presentationId = "%3";
    var allDesktops = desktops();
    for (var i = 0; i < allDesktops.length; ++i) {
        var widgets = allDesktops[i].widgets();
        for (var j = widgets.length - 1; j >= 0; --j) {
            if (widgets[j].type !== pluginId && widgets[j].pluginName !== pluginId)
                continue;
            widgets[j].currentConfigGroup = ["General"];
            if ((presentationId && String(widgets[j].id) === presentationId)
                || widgets[j].readConfig("stickyId", "") === stickyId)
                widgets[j].remove();
        }
    }
    print("OK");
} catch (error) {
    print("ERROR:" + error);
}
)JS")
                                .arg(idText, stickyPlasmoidId, presentationId);
    const bool    removed = evaluatePlasmaScript(script);
    if (removed && !presentationId.isEmpty())
        settings.remove(presentationId);
    return removed;
}

QUuid KDEIntegration::stickyNoteIdForPresentation(const QString &presentationId) const
{
    QSettings settings;
    settings.beginGroup(stickyPresentationsGroup);
    return QUuid(settings.value(presentationId).toString());
}

bool KDEIntegration::saveWindowGeometry(QWindow *, const QString &)
{
    // The KWin script stores what KWin knows: the window's real position.
    return waylandWindowTags() && _waylandGeometryScriptAvailable;
}

bool KDEIntegration::removeWindowGeometry(const QString &) { return false; }

namespace {
    QString shortcutConflictDescription(const QKeySequence &key, const QAction *ownAction)
    {
        if (key.isEmpty())
            return {};
        const auto  conflicts = KGlobalAccel::globalShortcutsByKey(key);
        QStringList owners;
        for (const auto &info : conflicts) {
            if (ownAction && info.componentUniqueName() == QCoreApplication::applicationName()
                && info.uniqueName() == ownAction->objectName()) {
                continue;
            }
            const QString component = info.componentFriendlyName();
            const QString action    = info.friendlyName();
            owners.append(action.isEmpty() ? component : QStringLiteral("%1 — %2").arg(component, action));
        }
        owners.removeDuplicates();
        return owners.join(QStringLiteral(", "));
    }

    bool updateKdeGlobalShortcut(QAction *action, const QKeySequence &key)
    {
        const QList<QKeySequence> shortcuts = key.isEmpty() ? QList<QKeySequence>() : QList<QKeySequence> { key };
        auto                     *accel     = KGlobalAccel::self();
        // AnyKeep's shortcut editor is an explicit user request.  Autoloading would
        // restore the previously saved KDE value and ignore the newly supplied
        // sequence. Keep the application default intact and replace only the
        // active user shortcut without autoloading the old value.
        const bool activeSet = accel->setShortcut(action, shortcuts, KGlobalAccel::NoAutoloading);
        return activeSet && accel->shortcut(action) == shortcuts;
    }
}

bool KDEIntegration::registerGlobalShortcut(const QString &id, const QKeySequence &key, QAction *action)
{
    _lastGlobalShortcutError.clear();
    QAction *act = _shortcuts.value(id);
    if (!act) {
        act = new QAction(action->text(), this);
        act->setObjectName(id);
        _shortcuts.insert(id, act);
    }
    const QString conflict = shortcutConflictDescription(key, act);
    if (!conflict.isEmpty()) {
        _lastGlobalShortcutError
            = tr("The shortcut %1 is already used by %2.").arg(key.toString(QKeySequence::NativeText), conflict);
        return false;
    }
    // Do not call isGlobalShortcutAvailable() here.  KGlobalAccel reports an
    // action's already registered shortcut as unavailable as well, so an
    // update can conflict with AnyKeep itself.  globalShortcutsByKey() above
    // gives us the actual owners and filters this action; setShortcut() is the
    // authoritative check for reserved combinations that have no owner.
    const bool registered = KGlobalAccel::setGlobalShortcut(act, key);
    if (!registered) {
        _lastGlobalShortcutError = tr("KDE rejected the shortcut %1.").arg(key.toString(QKeySequence::NativeText));
        qCWarning(logKdeIntegration).noquote()
            << "registerGlobalShortcut failed"
            << "id=" << id << "text=" << action->text() << "key=" << key.toString(QKeySequence::NativeText)
            << "platform=" << QGuiApplication::platformName();
    }
    connect(act, SIGNAL(triggered()), action, SLOT(trigger()), Qt::UniqueConnection);
    return registered;
}

bool KDEIntegration::updateGlobalShortcut(const QString &id, const QKeySequence &key)
{
    _lastGlobalShortcutError.clear();
    auto act = _shortcuts.value(id);
    if (!act) {
        qCWarning(logKdeIntegration) << "updateGlobalShortcut: no action for id" << id
                                     << "key=" << key.toString(QKeySequence::NativeText);
        return false;
    }
    const QString conflict = shortcutConflictDescription(key, act);
    if (!conflict.isEmpty()) {
        _lastGlobalShortcutError
            = tr("The shortcut %1 is already used by %2.").arg(key.toString(QKeySequence::NativeText), conflict);
        return false;
    }
    // Do not call isGlobalShortcutAvailable() here.  KGlobalAccel reports an
    // action's already registered shortcut as unavailable as well, so an
    // update can conflict with AnyKeep itself.  globalShortcutsByKey() above
    // gives us the actual owners and filters this action; setShortcut() is the
    // authoritative check for reserved combinations that have no owner.
    const bool registered = updateKdeGlobalShortcut(act, key);
    if (!registered) {
        _lastGlobalShortcutError = tr("KDE rejected the shortcut %1.").arg(key.toString(QKeySequence::NativeText));
        qCWarning(logKdeIntegration).noquote() << "updateGlobalShortcut failed"
                                               << "id=" << id << "key=" << key.toString(QKeySequence::NativeText);
    }
    return registered;
}

QString KDEIntegration::lastGlobalShortcutError() const { return _lastGlobalShortcutError; }

void KDEIntegration::setGlobalShortcutEnabled(const QString &id, bool enabled)
{
    auto act = _shortcuts.value(id);
    if (!act) {
        qCWarning(logKdeIntegration) << "setGlobalShortcutEnabled: no action for id" << id << "enabled=" << enabled;
        return;
    }
    act->setEnabled(enabled);
}

} // namespace AnyKeep
