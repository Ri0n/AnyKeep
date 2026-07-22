#include <QGuiApplication>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>
#include <QtPlugin>

#include "cinnamon.h"
#include "freedesktopnotifier.h"
#include "qtnote_config.h"

namespace QtNote {

static const QLatin1String pluginId("cinnamon_de");
static const QLatin1String appletId("qtnote@ri0n.github.io");
static const QLatin1String deskletId("qtnote-sticky@ri0n.github.io");
static const QLatin1String stickyPresentationsGroup("cinnamonintegration/stickyPresentations");

static QString runGSettings(const QStringList &arguments, bool *ok = nullptr)
{
    const QString executable = QStandardPaths::findExecutable(QLatin1String("gsettings"));
    if (executable.isEmpty()) {
        if (ok)
            *ok = false;
        return {};
    }

    QProcess process;
    process.start(executable, arguments);
    if (!process.waitForFinished(3000)) {
        if (ok)
            *ok = false;
        return {};
    }

    const bool succeeded = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    if (ok)
        *ok = succeeded;
    if (!succeeded) {
        qWarning().noquote() << "Cinnamon integration: gsettings failed:" << arguments.join(QLatin1Char(' '))
                             << QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
    }
    return succeeded ? QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed() : QString();
}

static QStringList parseGSettingsStringList(const QString &value)
{
    QStringList        result;
    QRegularExpression expression(QLatin1String("'((?:\\\\.|[^'])*)'"));
    auto               matchIterator = expression.globalMatch(value);
    while (matchIterator.hasNext()) {
        QString item = matchIterator.next().captured(1);
        item.replace(QLatin1String("\\'"), QLatin1String("'"));
        item.replace(QLatin1String("\\\\"), QLatin1String("\\"));
        result.append(item);
    }
    return result;
}

static QString serializeGSettingsStringList(const QStringList &values)
{
    QStringList quoted;
    for (QString value : values) {
        value.replace(QLatin1String("\\"), QLatin1String("\\\\"));
        value.replace(QLatin1String("'"), QLatin1String("\\'"));
        quoted.append(QLatin1Char('\'') + value + QLatin1Char('\''));
    }
    return QLatin1Char('[') + quoted.join(QLatin1String(", ")) + QLatin1Char(']');
}

CinnamonPlugin::CinnamonPlugin(QObject *parent) : QObject(parent) { }

int CinnamonPlugin::metadataVersion() const { return MetadataVersion; }

PluginMetadata CinnamonPlugin::metadata()
{
    PluginMetadata md;
    md.id          = pluginId;
    md.icon        = QIcon(":/icons/qtnote");
    md.name        = "Cinnamon Integration";
    md.description = tr("Integration with Cinnamon desktop features");
    md.author      = "Sergei Ilinykh <rion4ik@gmail.com>";
    md.version     = 0x01000000;
    md.minVersion  = 0x020300;
    md.maxVersion  = QTNOTE_VERSION;
    md.homepage    = QUrl("http://ri0n.github.io/QtNote");
    md.extra.insert("de", QStringList() << "cinnamon" << "x-cinnamon");
    md.extra.insert("externalTray", true);
    return md;
}

void CinnamonPlugin::setHost(PluginHostInterface *host)
{
    this->host = host;
    QTimer::singleShot(0, this, &CinnamonPlugin::ensureAppletEnabled);
}

bool CinnamonPlugin::isAppletInstalled() const
{
    return !QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                      QLatin1String("cinnamon/applets/") + appletId + QLatin1String("/metadata.json"),
                                      QStandardPaths::LocateFile)
                .isEmpty();
}

bool CinnamonPlugin::isDeskletInstalled() const
{
    return !QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                      QLatin1String("cinnamon/desklets/") + deskletId + QLatin1String("/metadata.json"),
                                      QStandardPaths::LocateFile)
                .isEmpty();
}

bool CinnamonPlugin::isAppletEnabled() const
{
    bool              ok      = false;
    const QStringList applets = parseGSettingsStringList(
        runGSettings({ QLatin1String("get"), QLatin1String("org.cinnamon"), QLatin1String("enabled-applets") }, &ok));
    if (!ok)
        return false;

    for (const QString &definition : applets) {
        if (definition.split(QLatin1Char(':')).value(3) == appletId)
            return true;
    }
    return false;
}

bool CinnamonPlugin::enableApplet() const
{
    bool        ok      = false;
    QStringList applets = parseGSettingsStringList(
        runGSettings({ QLatin1String("get"), QLatin1String("org.cinnamon"), QLatin1String("enabled-applets") }, &ok));
    if (!ok)
        return false;

    const QStringList panels = parseGSettingsStringList(
        runGSettings({ QLatin1String("get"), QLatin1String("org.cinnamon"), QLatin1String("panels-enabled") }, &ok));
    if (!ok || panels.isEmpty())
        return false;

    const QString panelNumber = panels.first().section(QLatin1Char(':'), 0, 0);
    const QString panelId     = QLatin1String("panel") + panelNumber;
    for (QString &definition : applets) {
        QStringList fields = definition.split(QLatin1Char(':'));
        if (fields.value(0) == panelId && fields.value(1) == QLatin1String("right")) {
            fields[2]  = QString::number(fields.value(2).toInt() + 1);
            definition = fields.join(QLatin1Char(':'));
        }
    }

    const int instanceId
        = runGSettings({ QLatin1String("get"), QLatin1String("org.cinnamon"), QLatin1String("next-applet-id") }, &ok)
              .toInt();
    if (!ok)
        return false;

    applets.append(QStringLiteral("%1:right:0:%2:%3").arg(panelId).arg(appletId).arg(instanceId));
    runGSettings({ QLatin1String("set"), QLatin1String("org.cinnamon"), QLatin1String("next-applet-id"),
                   QString::number(instanceId + 1) },
                 &ok);
    if (!ok)
        return false;

    runGSettings({ QLatin1String("set"), QLatin1String("org.cinnamon"), QLatin1String("enabled-applets"),
                   serializeGSettingsStringList(applets) },
                 &ok);
    return ok;
}

void CinnamonPlugin::ensureAppletEnabled()
{
    const bool installed = isAppletInstalled();
    const bool enabled   = installed && isAppletEnabled();
    qInfo() << "Cinnamon integration applet state:" << "installed" << installed << "enabled" << enabled;
    if (!installed || enabled)
        return;

    QSettings settings;
    settings.beginGroup(QLatin1String("cinnamonintegration"));
    if (settings.value(QLatin1String("appletEnableAsked"), false).toBool())
        return;
    settings.setValue(QLatin1String("appletEnableAsked"), true);

    const auto result = QMessageBox::question(nullptr, tr("Add QtNote Cinnamon Applet"),
                                              tr("QtNote can add a native applet to the Cinnamon panel. "
                                                 "It provides Wayland-friendly access to recent notes."),
                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (result == QMessageBox::Yes && !enableApplet()) {
        QMessageBox::warning(
            nullptr, tr("Add QtNote Cinnamon Applet"),
            tr("Failed to add the QtNote applet. You can add it manually in Cinnamon Applets settings."));
    } else if (result == QMessageBox::Yes) {
        qInfo() << "Cinnamon integration: QtNote applet added to the panel";
    }
}

void CinnamonPlugin::notifyError(const QString &message)
{
    if (FreedesktopNotifier::notifyError(tr("Error"), message))
        return;

    QMessageBox::warning(nullptr, tr("Error"), message);
}

void CinnamonPlugin::activateWindow(QWindow *window)
{
    QPointer<QWindow> guarded(window);
    QTimer::singleShot(100, this, [guarded]() {
        if (!guarded)
            return;
        guarded->showNormal();
        guarded->raise();
        guarded->requestActivate();
    });
}

WindowGeometryRestoreResult CinnamonPlugin::restoreWindowGeometry(QWindow *, const QString &key)
{
    if (QGuiApplication::platformName() != QLatin1String("wayland") || !geometryBridgeReady)
        return WindowGeometryRestoreResult::Unsupported;
    if (!pendingWindowGeometryKeys.contains(key))
        pendingWindowGeometryKeys.enqueue(key);
    return WindowGeometryRestoreResult::Pending;
}

bool CinnamonPlugin::saveWindowGeometry(QWindow *, const QString &)
{
    if (QGuiApplication::platformName() != QLatin1String("wayland") || !geometryBridgeReady)
        return false;
    // The applet has already claimed the key queued by restoreWindowGeometry()
    // and tracks the window until it is unmanaged. Re-enqueuing here would make
    // the next window claim this stale key.
    return true;
}

bool CinnamonPlugin::removeWindowGeometry(const QString &)
{
    return QGuiApplication::platformName() == QLatin1String("wayland") && geometryBridgeReady;
}

QString CinnamonPlugin::takePendingWindowGeometryKey()
{
    return pendingWindowGeometryKeys.isEmpty() ? QString() : pendingWindowGeometryKeys.dequeue();
}

void CinnamonPlugin::windowGeometryBridgeReady() { geometryBridgeReady = true; }

bool CinnamonPlugin::isStickyNotesAvailable() const { return isDeskletInstalled(); }

bool CinnamonPlugin::presentStickyNote(const QUuid &stickyId, const QRect &preferredGeometry)
{
    if (stickyId.isNull() || !isDeskletInstalled())
        return false;

    bool        ok       = false;
    QStringList desklets = parseGSettingsStringList(
        runGSettings({ QLatin1String("get"), QLatin1String("org.cinnamon"), QLatin1String("enabled-desklets") }, &ok));
    if (!ok)
        return false;

    QSettings settings;
    settings.beginGroup(stickyPresentationsGroup);
    QString       instanceId;
    const QString idText = stickyId.toString(QUuid::WithoutBraces);
    for (const auto &key : settings.childKeys()) {
        if (settings.value(key).toString() == idText) {
            instanceId = key;
            break;
        }
    }

    if (instanceId.isEmpty()) {
        instanceId = runGSettings(
                         { QLatin1String("get"), QLatin1String("org.cinnamon"), QLatin1String("next-desklet-id") }, &ok)
                         .trimmed();
        if (!ok || instanceId.isEmpty())
            return false;
        runGSettings({ QLatin1String("set"), QLatin1String("org.cinnamon"), QLatin1String("next-desklet-id"),
                       QString::number(instanceId.toInt() + 1) },
                     &ok);
        if (!ok)
            return false;
        settings.setValue(instanceId, idText);
    }

    const QString prefix = deskletId + QLatin1Char(':') + instanceId + QLatin1Char(':');
    for (const auto &definition : std::as_const(desklets)) {
        if (definition.startsWith(prefix))
            return true;
    }

    const int x = preferredGeometry.isValid() ? qMax(0, preferredGeometry.x()) : 100;
    const int y = preferredGeometry.isValid() ? qMax(0, preferredGeometry.y()) : 100;
    desklets.append(QStringLiteral("%1:%2:%3:%4").arg(deskletId, instanceId).arg(x).arg(y));
    runGSettings({ QLatin1String("set"), QLatin1String("org.cinnamon"), QLatin1String("enabled-desklets"),
                   serializeGSettingsStringList(desklets) },
                 &ok);
    return ok;
}

bool CinnamonPlugin::dismissStickyNote(const QUuid &stickyId)
{
    QSettings settings;
    settings.beginGroup(stickyPresentationsGroup);
    QString       instanceId;
    const QString idText = stickyId.toString(QUuid::WithoutBraces);
    for (const auto &key : settings.childKeys()) {
        if (settings.value(key).toString() == idText) {
            instanceId = key;
            break;
        }
    }
    if (instanceId.isEmpty())
        return false;

    bool        ok       = false;
    QStringList desklets = parseGSettingsStringList(
        runGSettings({ QLatin1String("get"), QLatin1String("org.cinnamon"), QLatin1String("enabled-desklets") }, &ok));
    if (!ok)
        return false;
    const QString prefix = deskletId + QLatin1Char(':') + instanceId + QLatin1Char(':');
    for (int i = desklets.size() - 1; i >= 0; --i) {
        if (desklets.at(i).startsWith(prefix))
            desklets.removeAt(i);
    }
    runGSettings({ QLatin1String("set"), QLatin1String("org.cinnamon"), QLatin1String("enabled-desklets"),
                   serializeGSettingsStringList(desklets) },
                 &ok);
    if (ok)
        settings.remove(instanceId);
    return ok;
}

QUuid CinnamonPlugin::stickyNoteIdForPresentation(const QString &presentationId) const
{
    QSettings settings;
    settings.beginGroup(stickyPresentationsGroup);
    return QUuid(settings.value(presentationId).toString());
}

} // namespace QtNote
