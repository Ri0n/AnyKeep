/*
QtNote - Simple note-taking application
Copyright (C) 2020 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

Contacts:
E-Mail: rion4ik@gmail.com XMPP: rion@jabber.ru
*/

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QJsonObject>
#include <QLibrary>
#include <QPluginLoader>
#include <QSet>
#include <QSettings>
#include <QStringList>

#include <utility>

#include "actionnotificationinterface.h"
#include "notemanager.h"
#include "pluginhost.h"
#include "pluginiconimageprovider.h"
#include "pluginmanager.h"

#include "editorplatformbackend.h"
#include "qtnote.h"
#include "shortcutsmanager.h"
#include "spellcheckproviderinterface.h"
#include "utils.h"

#include "deintegrationinterface.h"
#include "globalshortcutsinterface.h"
#include "notificationinterface.h"
#include "qtnote_config.h"
#include "stickynotesintegrationinterface.h"
#include "stickynotesmanager.h"
#include "trayinterface.h"

namespace QtNote {

static PluginManager::LoadPolicy defaultLoadPolicy(const PluginMetadata &metadata)
{
    const auto value = metadata.extra.value(QLatin1String("defaultLoadPolicy"));
    const auto text  = value.toString().toLower();
    if (text == QLatin1String("enabled")) {
        return PluginManager::LP_Enabled;
    }
    if (text == QLatin1String("disabled")) {
        return PluginManager::LP_Disabled;
    }
    if (!text.isEmpty() && text != QLatin1String("auto")) {
        qWarning() << "Unknown plugin default load policy" << text;
    }

    if (value.userType() != QMetaType::QString && value.canConvert<int>()) {
        const auto policy = value.toInt();
        if (policy >= PluginManager::LP_Auto && policy <= PluginManager::LP_Disabled) {
            return PluginManager::LoadPolicy(policy);
        }
    }
    return PluginManager::LP_Auto;
}

static QString loadPolicyName(PluginManager::LoadPolicy policy)
{
    switch (policy) {
    case PluginManager::LP_Auto:
        return QStringLiteral("auto");
    case PluginManager::LP_Enabled:
        return QStringLiteral("enabled");
    case PluginManager::LP_Disabled:
        return QStringLiteral("disabled");
    }
    return QStringLiteral("unknown");
}

static QString loadStatusName(PluginManager::LoadStatus status)
{
    switch (status) {
    case PluginManager::LS_Undefined:
        return QStringLiteral("undefined");
    case PluginManager::LS_Loaded:
        return QStringLiteral("loaded");
    case PluginManager::LS_Initialized:
        return QStringLiteral("initialized");
    case PluginManager::LS_ErrNotPlugin:
        return QStringLiteral("not-plugin");
    case PluginManager::LS_ErrVersion:
        return QStringLiteral("version-error");
    case PluginManager::LS_ErrAbi:
        return QStringLiteral("abi-error");
    case PluginManager::LS_ErrMetadata:
        return QStringLiteral("metadata-error");
    case PluginManager::LS_Unloaded:
        return QStringLiteral("unloaded");
    case PluginManager::LS_Errors:
        return QStringLiteral("error");
    }
    return QStringLiteral("unknown");
}

static QString pluginFeaturesName(PluginManager::PluginFeatures features)
{
    QStringList ret;
    if (features & PluginManager::RegularPlugin)
        ret.append(QStringLiteral("regular"));
    if (features & PluginManager::DEIntegration)
        ret.append(QStringLiteral("de"));
    if (features & PluginManager::TrayIcon)
        ret.append(QStringLiteral("tray"));
    if (features & PluginManager::GlobalShortcuts)
        ret.append(QStringLiteral("shortcuts"));
    if (features & PluginManager::Notifications)
        ret.append(QStringLiteral("notifications"));
    if (features & PluginManager::StickyNotes)
        ret.append(QStringLiteral("sticky-notes"));
    return ret.isEmpty() ? QStringLiteral("none") : ret.join(QLatin1Char(','));
}

static bool pluginFeaturesFromNames(const QStringList &names, PluginManager::PluginFeatures *features,
                                    QString *unknownFeature)
{
    Q_ASSERT(features);
    *features = {};
    for (const auto &name : names) {
        if (name.compare(QLatin1String("regular"), Qt::CaseInsensitive) == 0)
            *features |= PluginManager::RegularPlugin;
        else if (name.compare(QLatin1String("desktopIntegration"), Qt::CaseInsensitive) == 0)
            *features |= PluginManager::DEIntegration;
        else if (name.compare(QLatin1String("tray"), Qt::CaseInsensitive) == 0)
            *features |= PluginManager::TrayIcon;
        else if (name.compare(QLatin1String("globalShortcuts"), Qt::CaseInsensitive) == 0)
            *features |= PluginManager::GlobalShortcuts;
        else if (name.compare(QLatin1String("notifications"), Qt::CaseInsensitive) == 0)
            *features |= PluginManager::Notifications;
        else if (name.compare(QLatin1String("stickyNotes"), Qt::CaseInsensitive) == 0)
            *features |= PluginManager::StickyNotes;
        else {
            if (unknownFeature)
                *unknownFeature = name;
            return false;
        }
    }
    if (unknownFeature)
        unknownFeature->clear();
    return true;
}

QJsonObject readPluginMetadata(const QString &fileName)
{
#ifdef QTNOTE_DEVEL
    qDebug("Reading plugin metadata: %s", qPrintable(fileName));
#endif
    QPluginLoader loader(fileName);
    const auto    metadata = loader.metaData();
#ifdef QTNOTE_DEVEL
    qDebug("Plugin metadata block extracted: %s", qPrintable(fileName));
#endif
    return metadata;
}

class PluginsIterator {
    QDir                        currentDir;
    QStringList                 pluginsDirs;
    QStringList                 plugins;
    QStringList::const_iterator pluginsDirsIt;
    QStringList::const_iterator pluginsIt;

public:
    PluginsIterator()
    {
#ifdef QTNOTE_DEVEL
        QDir pluginsDir = QDir(qApp->applicationDirPath());

#if defined(Q_OS_MAC)
        if (pluginsDir.dirName() == "MacOS") {
            pluginsDir.cdUp();
            pluginsDir.cdUp();
            pluginsDir.cdUp();
        }
#else
        pluginsDir.cdUp(); // on linux application dir is <builddir>/src. probably same same on windows
#endif
        pluginsDir.cd("plugins");
#ifdef Q_OS_WIN
        pluginsDirs << pluginsDir.path();
        pluginsDir = QDir::current();
        pluginsDir.cd("plugins");
        pluginsDirs << pluginsDir.path();
#else
        foreach (const QString &dirName, pluginsDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QDir d(pluginsDir);
            d.cd(dirName);
            pluginsDirs << d.path();
        }
#endif
        qDebug() << "Plugins dirs: " << pluginsDirs;
#else // not devel

        // A binary launched directly from <build>/src must use plugins from
        // the same build. Otherwise it silently falls back to an older
        // installed plugin, which is especially confusing while evolving the
        // editor/highlighter ABI.
        QDir buildPlugins(qApp->applicationDirPath());
        if (buildPlugins.cdUp() && buildPlugins.cd(QStringLiteral("plugins"))) {
            for (const auto &dirName : buildPlugins.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QDir pluginDir(buildPlugins);
                if (pluginDir.cd(dirName))
                    pluginsDirs << pluginDir.path();
            }
        }

        pluginsDirs << Utils::qtnoteDataDir() + "/plugins";
#if defined(Q_OS_UNIX) && !defined(Q_OS_MAC)
        pluginsDirs << LIBDIR "/" APPNAME;
#elif defined(Q_OS_WIN)
        pluginsDirs << qApp->applicationDirPath() + "/plugins";
#endif
#endif
        pluginsDirsIt = pluginsDirs.constBegin();
        findDirWithPlugins();
    }

    void next()
    {
        if (pluginsIt != plugins.constEnd()) {
            pluginsIt++;
        }
        if (pluginsIt == plugins.constEnd() && pluginsDirsIt != pluginsDirs.constEnd()) {
            pluginsDirsIt++;
            findDirWithPlugins();
        }
    }

    bool isFinished() const { return pluginsIt == plugins.constEnd() && pluginsDirsIt == pluginsDirs.constEnd(); }

    QString fileName() const
    {
        if (pluginsIt != plugins.constEnd()) {
            return currentDir.absoluteFilePath(*pluginsIt);
        }
        return QString();
    }

private:
    void findDirWithPlugins()
    {
        while (pluginsDirsIt != pluginsDirs.constEnd()) {
            currentDir = QDir(*pluginsDirsIt);
            if (currentDir.isReadable()) {
                plugins.clear();
                foreach (const QString &file, currentDir.entryList(QDir::Files)) {
                    if (QLibrary::isLibrary(file)) {
                        plugins.append(file);
                    }
                }
                if (!plugins.isEmpty()) {
                    pluginsIt = plugins.constBegin();
                    return;
                }
            }
            pluginsDirsIt++;
        }
        pluginsIt = plugins.constEnd();
    }
};

PluginManager::PluginManager(Main *parent) : PluginListSource(parent), qtnote(parent), pluginHost(new PluginHost(this))
{
    updateMetadata();

    connect(pluginHost, &PluginHost::spellCheckProviderConflict, this,
            [this](const QString &activeName, const QString &ignoredName) {
                qtnote->notify(
                    tr("Spell checker selected"),
                    tr("QtNote is using %1. %2 is also enabled, but is not being used.").arg(activeName, ignoredName),
                    tr("Configure plugins"),
                    [this]() { QMetaObject::invokeMethod(qtnote, "showOptions", Qt::QueuedConnection); });
            });
}

PluginManager::~PluginManager()
{
    for (const auto &plugin : std::as_const(plugins)) {
        if (!plugin || !plugin->instance)
            continue;
        delete plugin->instance;
        plugin->instance = nullptr;
    }
}

void PluginManager::attachEditorPlatformBackend(EditorPlatformBackend *backend)
{
    if (!backend)
        return;
    connect(pluginHost, &PluginHost::rehightlight_requested, backend, &EditorPlatformBackend::rehighlight,
            Qt::UniqueConnection);
    pluginHost->attachSpellCheck(backend);
}

void PluginManager::loadPlugins()
{
    struct FeaturedPlugin {
        QStringList native;
        QStringList base;
    };

    QMap<PluginFeature, FeaturedPlugin> featurePriority;
    QList<PluginData::Ptr>              regularPlugins;
    bool                                nativeExternalTrayAvailable = false;

    QSettings                  s;
    QStringList                prioritizedList = s.value("plugins-priority").toStringList();
    QMutableStringListIterator it(prioritizedList);
    while (it.hasNext()) {
        if (!plugins.contains(it.next())) {
            it.remove();
        }
    }

    auto pluginKeys     = QSet<QString>(plugins.keyBegin(), plugins.keyEnd());
    auto prioritizedSet = pluginKeys - QSet<QString>(prioritizedList.constBegin(), prioritizedList.constEnd());
    prioritizedList += QStringList(prioritizedSet.constBegin(), prioritizedSet.constEnd());
    s.setValue("plugins-priority", prioritizedList);

    /*
     * now we have fully prioritized list.
     * time to load the plugins. start from required unique features
     * like tray integration, global shortcuts integrtion etc.
     */
#ifdef Q_OS_OSX
    QStringList sessionCandidates { QStringLiteral("macosx") };
#else
    const QString xdgCurrentDesktop = QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP"));
    const QString xdgSessionDesktop = QString::fromLocal8Bit(qgetenv("XDG_SESSION_DESKTOP"));
    const QString desktopSession    = QString::fromLocal8Bit(qgetenv("DESKTOP_SESSION"));
    const QString gnomeSessionId    = QString::fromLocal8Bit(qgetenv("GNOME_DESKTOP_SESSION_ID"));
    const QString xdgSessionType    = QString::fromLocal8Bit(qgetenv("XDG_SESSION_TYPE"));

    QStringList sessionCandidates;
    for (const auto &component : xdgCurrentDesktop.split(QLatin1Char(':'), Qt::SkipEmptyParts))
        sessionCandidates.append(component.toLower());
    if (!xdgSessionDesktop.isEmpty())
        sessionCandidates.append(xdgSessionDesktop.toLower());
    if (!desktopSession.isEmpty())
        sessionCandidates.append(desktopSession.toLower());
    if (!gnomeSessionId.isEmpty() && gnomeSessionId != QLatin1String("this-is-deprecated"))
        sessionCandidates.append(QStringLiteral("gnome"));
    sessionCandidates.removeDuplicates();

    qInfo().noquote() << "Desktop environment detection:"
                      << "XDG_CURRENT_DESKTOP=" << xdgCurrentDesktop << "XDG_SESSION_DESKTOP=" << xdgSessionDesktop
                      << "DESKTOP_SESSION=" << desktopSession << "GNOME_DESKTOP_SESSION_ID=" << gnomeSessionId
                      << "XDG_SESSION_TYPE=" << xdgSessionType
                      << "candidates=" << sessionCandidates.join(QLatin1Char(','));
    if (gnomeSessionId == QLatin1String("this-is-deprecated")) {
        qInfo() << "Ignoring deprecated GNOME_DESKTOP_SESSION_ID marker for desktop detection";
    }
#endif
    qInfo().noquote() << "Plugin priority order:" << prioritizedList.join(QLatin1Char(','));

    foreach (const QString &plugin, prioritizedList) {
        PluginData::Ptr   pd     = plugins[plugin];
        const QStringList deList = pd->metadata.desktopEnvironments;
        bool              native = false;
        for (const auto &session : std::as_const(sessionCandidates)) {
            if (deList.contains(session)) {
                native = true;
                break;
            }
        }

        qInfo().noquote() << "Plugin candidate:" << plugin << "policy=" << loadPolicyName(pd->loadPolicy)
                          << "explicit=" << pd->loadPolicyExplicit << "status=" << loadStatusName(pd->loadStatus)
                          << "features=" << pluginFeaturesName(pd->features)
                          << "de=" << (deList.isEmpty() ? QStringLiteral("-") : deList.join(QLatin1Char(',')))
                          << "native=" << native;

        if (pd->loadPolicy == LP_Disabled) {
            qInfo() << "Plugin skipped by load policy:" << plugin;
            continue;
        }
        if (pd->loadPolicy == LP_Auto && !native && !deList.isEmpty()) {
            qInfo().noquote() << "Plugin skipped by desktop mismatch in automatic mode:" << plugin
                              << "required=" << deList.join(QLatin1Char(','))
                              << "candidates=" << sessionCandidates.join(QLatin1Char(','));
            continue;
        }
        if (pd->loadPolicy == LP_Enabled && !native && !deList.isEmpty()) {
            qInfo().noquote() << "Plugin desktop mismatch overridden by enabled policy:" << plugin
                              << "required=" << deList.join(QLatin1Char(','))
                              << "candidates=" << sessionCandidates.join(QLatin1Char(','));
        }

        if (native && pd->metadata.extra.value(QLatin1String("externalTray")).toBool())
            nativeExternalTrayAvailable = true;

        auto f = FirstFeature;
        while (f != LastFeature) {
            if (pd->features & f) {
                FeaturedPlugin &fp = featurePriority[f];
                if (native) {
                    fp.native.push_back(plugin);
                } else {
                    fp.base.push_back(plugin);
                }
            }
            f = PluginFeature(f << 1);
        }

        if (pd->features & RegularPlugin) {
            regularPlugins.append(pd);
        }
    }

    /* set most desirable tray implementation */
    if (nativeExternalTrayAvailable)
        qtnote->setExternalTrayAvailable(true);

    QStringList trayPlugins = featurePriority[TrayIcon].native;
    if (!nativeExternalTrayAvailable || !trayPlugins.isEmpty())
        trayPlugins += featurePriority[TrayIcon].base;
    qInfo().noquote() << "Tray plugin candidates:" << trayPlugins.join(QLatin1Char(','))
                      << "nativeExternalTrayAvailable=" << nativeExternalTrayAvailable;
    foreach (const QString &plugin, trayPlugins) {
        auto pd = plugins[plugin];
        qInfo() << "Trying tray plugin:" << plugin;
        if (!ensureLoaded(pd)) {
            qInfo() << "Tray plugin failed to load:" << plugin << loadStatusName(pd->loadStatus);
            continue;
        }
        TrayInterface *tp   = qobject_cast<TrayInterface *>(pd->instance);
        TrayImpl      *tray = tp->initTray(qtnote);
        if (tray) {
            qtnote->setTrayImpl(tray);
            pd->loadStatus = LS_Initialized;
            qInfo() << "Tray plugin selected:" << plugin;
            break;
        }
        qInfo() << "Tray plugin returned no implementation:" << plugin;
    }

    /* set most desirable desktop environment plugin */
    QStringList dePlugins = featurePriority[DEIntegration].native + featurePriority[DEIntegration].base;
    qInfo().noquote() << "Desktop integration plugin candidates:" << dePlugins.join(QLatin1Char(','));
    foreach (const QString &plugin, dePlugins) {
        auto pd = plugins[plugin];
        qInfo() << "Trying desktop integration plugin:" << plugin;
        if (!ensureLoaded(pd)) {
            qInfo() << "Desktop integration plugin failed to load:" << plugin << loadStatusName(pd->loadStatus);
            continue;
        }
        auto *desktop = qobject_cast<DEIntegrationInterface *>(pd->instance);
        qtnote->setDesktopImpl(desktop);
        if (auto *spellCheck = qobject_cast<SpellCheckProviderInterface *>(pd->instance))
            pluginHost->offerSpellCheckProvider(spellCheck->spellCheckProvider());
        pd->loadStatus = LS_Initialized;
        qInfo() << "Desktop integration plugin selected:" << plugin;
        break;
    }

    /* Sticky notes are optional and selected independently from the basic
     * desktop integration. Native implementations still take precedence. */
    QStringList stickyPlugins = featurePriority[StickyNotes].native + featurePriority[StickyNotes].base;
    qInfo().noquote() << "Sticky notes plugin candidates:" << stickyPlugins.join(QLatin1Char(','));
    foreach (const QString &plugin, stickyPlugins) {
        auto pd = plugins[plugin];
        if (!ensureLoaded(pd))
            continue;
        if (auto *host = qobject_cast<StickyNotesHostInterface *>(pd->instance)) {
            host->initializeStickyNotes(qtnote->stickyNotesManager());
            qtnote->stickyNotesManager()->setRequiresApplicationAutostart(
                host->stickyNotesRequireApplicationAutostart());
        }
        auto *sticky = qobject_cast<StickyNotesIntegrationInterface *>(pd->instance);
        if (!sticky || !sticky->isStickyNotesAvailable())
            continue;
        qtnote->setStickyNotesImpl(sticky);
        pd->loadStatus = LS_Initialized;
        qInfo() << "Sticky notes plugin selected:" << plugin;
        break;
    }

    /* set most desirable global shortcuts plugin */
    QStringList gsPlugins = featurePriority[GlobalShortcuts].native + featurePriority[GlobalShortcuts].base;
    qInfo().noquote() << "Global shortcuts plugin candidates:" << gsPlugins.join(QLatin1Char(','));
    foreach (const QString &plugin, gsPlugins) {
        auto pd = plugins[plugin];
        qInfo() << "Trying global shortcuts plugin:" << plugin;
        if (!ensureLoaded(pd)) {
            qInfo() << "Global shortcuts plugin failed to load:" << plugin << loadStatusName(pd->loadStatus);
            continue;
        }
        qtnote->setGlobalShortcutsImpl(qobject_cast<GlobalShortcutsInterface *>(pd->instance));
        pd->loadStatus = LS_Initialized;
        qInfo() << "Global shortcuts plugin selected:" << plugin;
        break;
    }

    /* set most desirable notifications plugin */
    QStringList nPlugins = featurePriority[Notifications].native + featurePriority[Notifications].base;
    qInfo().noquote() << "Notifications plugin candidates:" << nPlugins.join(QLatin1Char(','));
    foreach (const QString &plugin, nPlugins) {
        auto pd = plugins[plugin];
        qInfo() << "Trying notifications plugin:" << plugin;
        if (!ensureLoaded(pd)) {
            qInfo() << "Notifications plugin failed to load:" << plugin << loadStatusName(pd->loadStatus);
            continue;
        }
        qtnote->setNotificationImpl(qobject_cast<NotificationInterface *>(pd->instance));
        qtnote->setActionNotificationImpl(qobject_cast<ActionNotificationInterface *>(pd->instance));
        pd->loadStatus = LS_Initialized;
        qInfo() << "Notifications plugin selected:" << plugin;
        break;
    }

    foreach (PluginData::Ptr pd, regularPlugins) {
        initRegularPlugin(pd);
    }
    emit pluginsReset();
}

bool PluginManager::ensureLoaded(PluginData::Ptr pd)
{
    if (pd->loadStatus == LS_Undefined || pd->loadStatus == LS_Unloaded) {
        return loadPlugin(pd->fileName, pd) == LS_Loaded;
    }
    if (pd->loadStatus > LS_Errors) {
        return false;
    }
    return true;
}

bool PluginManager::initRegularPlugin(const PluginData::Ptr &pd)
{
    if (!pd || !(pd->features & RegularPlugin)) {
        return false;
    }
    if (pd->loadStatus == LS_Initialized) {
        return true;
    }
    if (!ensureLoaded(pd)) {
        return false;
    }

    RegularPluginInterface *plugin = qobject_cast<RegularPluginInterface *>(pd->instance);
    if (!plugin) {
        return false;
    }
    const auto beforeStorages = NoteManager::instance()->storages(true).keys();
    if (plugin->initialize()) {
        const auto afterStorages = NoteManager::instance()->storages(true).keys();
        for (const auto &storageId : afterStorages) {
            if (!beforeStorages.contains(storageId))
                bindStorageIconToPlugin(storageId, pd->metadata.id);
        }
        pd->loadStatus = LS_Initialized;
        return true;
    }
    return false;
}

void PluginManager::deinitRegularPlugin(const PluginData::Ptr &pd)
{
    if (!pd || !(pd->features & RegularPlugin) || pd->loadStatus != LS_Initialized) {
        return;
    }

    RegularPluginInterface *plugin = qobject_cast<RegularPluginInterface *>(pd->instance);
    if (!plugin) {
        return;
    }
    const auto beforeStorages = NoteManager::instance()->storages(true).keys();
    plugin->shutdown();
    const auto afterStorages = NoteManager::instance()->storages(true).keys();
    for (const auto &storageId : beforeStorages) {
        if (!afterStorages.contains(storageId))
            unbindStorageIcon(storageId);
    }
    pd->loadStatus = LS_Loaded;
}

void PluginManager::setLoadPolicy(const QString &pluginId, PluginManager::LoadPolicy lp)
{
    auto pd = plugins.value(pluginId);
    if (!pd)
        return;

    QSettings settings;
    pd->loadPolicy         = lp;
    pd->loadPolicyExplicit = true;
    settings.beginGroup(QStringLiteral("plugins"));
    settings.beginGroup(pluginId);
    settings.setValue(QStringLiteral("loadPolicy"), int(lp));
    settings.setValue(QStringLiteral("loadPolicyExplicit"), true);

    if (pd->features & RegularPlugin) {
        if (lp == LP_Disabled)
            deinitRegularPlugin(pd);
        else
            initRegularPlugin(pd);
    }
    emit pluginChanged(pluginId);
}

bool PluginManager::hasLiveInstance(const PluginData::Ptr &plugin)
{
    if (!plugin || !plugin->instance || plugin->loadPolicy == LP_Disabled)
        return false;

    return plugin->loadStatus == LS_Loaded || plugin->loadStatus == LS_Initialized;
}

bool PluginManager::canConfigure(const QString &pluginId) const
{
    const auto plugin = plugins.value(pluginId);
    if (!hasLiveInstance(plugin))
        return false;

    if (plugin->metadata.extra.value(QStringLiteral("configurable")).toBool())
        return true;

    return qobject_cast<SettingsProviderInterface *>(plugin->instance) != nullptr;
}

QStringList PluginManager::pluginIds() const { return pluginsIds(); }

PluginListSource::Entry PluginManager::pluginEntry(const QString &pluginId) const
{
    Entry      entry;
    const auto pd = plugins.value(pluginId);
    if (!pd)
        return entry;

    entry.id           = pluginId;
    entry.name         = pd->metadata.name;
    entry.description  = pd->metadata.description;
    entry.fileName     = pd->fileName;
    entry.iconSource   = pluginIconSource(pluginId);
    entry.loadPolicy   = pd->loadPolicy;
    entry.loadStatus   = pd->loadStatus;
    entry.loaded       = isLoaded(pluginId);
    entry.configurable = canConfigure(pluginId);

    entry.versionText = pd->metadata.version;

    entry.tooltip = pd->metadata.description;
    if (!entry.fileName.isEmpty())
        entry.tooltip += QStringLiteral("<br/><br/>") + tr("<b>Filename:</b> %1").arg(entry.fileName);
    const QString pluginTooltip = tooltip(pluginId);
    if (!pluginTooltip.isEmpty())
        entry.tooltip += QStringLiteral("<br/><br/>") + pluginTooltip;
    return entry;
}

QUrl PluginManager::settingsComponent(const QString &pluginId) const
{
    const auto pd = plugins.value(pluginId);
    if (!hasLiveInstance(pd))
        return {};

    auto *provider = qobject_cast<SettingsProviderInterface *>(pd->instance);
    return provider ? provider->settingsComponent() : QUrl();
}

SettingsController *PluginManager::createSettingsController(const QString &pluginId, QObject *parent)
{
    const auto pd = plugins.value(pluginId);
    if (!pd || pd->loadPolicy == LP_Disabled || !ensureLoaded(pd) || !hasLiveInstance(pd))
        return nullptr;

    auto *provider = qobject_cast<SettingsProviderInterface *>(pd->instance);
    return provider ? provider->createSettingsController(parent) : nullptr;
}

bool PluginManager::setPluginLoadPolicy(const QString &pluginId, LoadPolicy policy)
{
    if (!plugins.contains(pluginId))
        return false;
    setLoadPolicy(pluginId, policy);
    return true;
}

bool PluginManager::setPluginOrder(const QStringList &pluginIds)
{
    if (pluginIds.size() != plugins.size()
        || QSet<QString>(pluginIds.cbegin(), pluginIds.cend()).size() != plugins.size())
        return false;
    for (const auto &pluginId : pluginIds) {
        if (!plugins.contains(pluginId))
            return false;
    }
    QSettings().setValue(QStringLiteral("plugins-priority"), pluginIds);
    emit pluginsReset();
    return true;
}

QStringList PluginManager::pluginsIds() const { return QSettings().value("plugins-priority").toStringList(); }

QString PluginManager::tooltip(const QString &pluginId) const
{
    const auto pd = plugins.value(pluginId);
    if (!hasLiveInstance(pd))
        return {};

    auto *plugin = qobject_cast<PluginOptionsTooltipInterface *>(pd->instance);
    return plugin ? plugin->tooltip() : QString();
}

QList<SpeechRecognitionProviderInterface *> PluginManager::speechRecognitionProviders() const
{
    QList<SpeechRecognitionProviderInterface *> ret;
    const auto                                  ids = pluginsIds();
    for (const auto &pluginId : ids) {
        auto pd = plugins.value(pluginId);
        if (!pd)
            continue;
        auto provider = castInterface<SpeechRecognitionProviderInterface>(pd);
        if (provider)
            ret.append(provider);
    }
    return ret;
}

SpeechRecognitionProviderInterface *PluginManager::speechRecognitionProvider() const
{
    const auto providers = speechRecognitionProviders();
    for (auto provider : providers) {
        if (provider->isSpeechRecognitionReady())
            return provider;
    }
    return nullptr;
}

void PluginManager::updateMetadata()
{
    for (auto it = plugins.cbegin(); it != plugins.cend(); ++it)
        unregisterPluginIcon(it.key());

    decltype(plugins) tmpPlugins;
    const auto        locale = pluginMetadataLocale();
    QSettings         settings;

    PluginsIterator it;
    while (!it.isFinished()) {
        const auto fileName       = it.fileName();
        const auto loaderMetadata = readPluginMetadata(fileName);
#ifdef QTNOTE_DEVEL
        qDebug("Plugin metadata reader released: %s", qPrintable(fileName));
#endif
        if (loaderMetadata.value(QStringLiteral("IID")).toString() != QLatin1String(QTNOTE_PLUGIN_INTERFACE_IID)) {
            it.next();
            continue;
        }
        PluginMetadata metadata;
        QString        metadataError;
        if (!pluginMetadataFromJson(loaderMetadata, locale, &metadata, &metadataError)) {
            qDebug("Invalid QtNote plugin metadata in %s: %s", qPrintable(fileName), qPrintable(metadataError));
            it.next();
            continue;
        }
#ifdef QTNOTE_DEVEL
        qDebug("Plugin metadata parsed: %s (%s)", qPrintable(metadata.id), qPrintable(fileName));
#endif
        QString compatibilityError;
        if (!semanticVersionInRange(QString::fromLatin1(QTNOTE_VERSION_STR), metadata.minVersion, metadata.maxVersion,
                                    &compatibilityError)) {
            const auto reason = compatibilityError.isEmpty() ? QStringLiteral("version is outside the supported range")
                                                             : compatibilityError;
            qDebug().noquote() << "Incompatible QtNote plugin" << fileName << "application version"
                               << QTNOTE_VERSION_STR << "supported range" << metadata.minVersion << ".."
                               << metadata.maxVersion << reason;
            it.next();
            continue;
        }
        if (tmpPlugins.contains(metadata.id)) { // first search path wins
            it.next();
            continue;
        }

        PluginData::Ptr pd(new PluginData);
        pd->fileName = fileName;
        pd->metadata = std::move(metadata);

        QString unknownFeature;
        if (!pluginFeaturesFromNames(pd->metadata.features, &pd->features, &unknownFeature)) {
            qDebug("Unknown feature %s in QtNote plugin metadata %s. ignore it", qPrintable(unknownFeature),
                   qPrintable(fileName));
            it.next();
            continue;
        }
        pd->loadStatus = LS_Unloaded;

        settings.beginGroup(QStringLiteral("plugins"));
        settings.beginGroup(pd->metadata.id);
        pd->loadPolicyExplicit = settings.value(QStringLiteral("loadPolicyExplicit"), false).toBool();
        pd->loadPolicy         = pd->loadPolicyExplicit
                    ? LoadPolicy(settings.value(QStringLiteral("loadPolicy"), LP_Auto).toInt())
                    : defaultLoadPolicy(pd->metadata);
        settings.setValue(QStringLiteral("loadPolicy"), int(pd->loadPolicy));
        settings.setValue(QStringLiteral("loadPolicyExplicit"), pd->loadPolicyExplicit);
        settings.setValue(QStringLiteral("filename"), pd->fileName);
        for (const auto &obsoleteKey :
             { QStringLiteral("metaversion"), QStringLiteral("lastModify"), QStringLiteral("features"),
               QStringLiteral("name"), QStringLiteral("description"), QStringLiteral("author"),
               QStringLiteral("version"), QStringLiteral("minVersion"), QStringLiteral("maxVersion"),
               QStringLiteral("extra") }) {
            settings.remove(obsoleteKey);
        }
        settings.endGroup();
        settings.endGroup();

        registerPluginIcon(pd->metadata.id, { pd->metadata.iconData, pd->metadata.iconMimeType, {} });
        tmpPlugins.insert(pd->metadata.id, pd);
        it.next();
    }
    plugins = std::move(tmpPlugins);
}

PluginManager::LoadStatus PluginManager::loadPlugin(const QString &fileName, PluginData::Ptr &cache)
{
    if (!cache)
        return LS_ErrMetadata;
    cache->instance = nullptr;
    if (cache->loadPolicy == LP_Disabled) {
        cache->loadStatus = LS_Unloaded;
        return LS_Unloaded;
    }

    const auto fail = [&cache](LoadStatus status) {
        cache->instance   = nullptr;
        cache->loadStatus = status;
        return status;
    };

#ifdef QTNOTE_DEVEL
    qDebug("Loading plugin: %s", qPrintable(fileName));
#endif
    QPluginLoader loader(fileName);
    QObject      *plugin = loader.instance();
    if (!plugin) {
        qDebug("failed to load %s : %s", qPrintable(fileName), qPrintable(loader.errorString()));
        return fail(LS_ErrNotPlugin);
    }

    auto *qnp = qobject_cast<PluginInterface *>(plugin);
    if (!qnp) {
        loader.unload();
        qDebug("not a compatible QtNote plugin %s. ignore it", qPrintable(fileName));
        return fail(LS_ErrAbi);
    }

    const auto featureMismatch = [plugin, &cache](PluginFeature feature, const char *name, bool implemented) {
        const bool declared = cache->features.testFlag(feature);
        if (declared == implemented)
            return false;
        if (declared)
            qWarning() << "Plugin" << cache->metadata.id << "declares" << name << "but does not implement it";
        else
            qWarning() << "Plugin" << cache->metadata.id << "implements" << name << "but does not declare it";
        return true;
    };
    if (featureMismatch(RegularPlugin, "regular", qobject_cast<RegularPluginInterface *>(plugin))
        || featureMismatch(DEIntegration, "desktopIntegration", qobject_cast<DEIntegrationInterface *>(plugin))
        || featureMismatch(TrayIcon, "tray", qobject_cast<TrayInterface *>(plugin))
        || featureMismatch(GlobalShortcuts, "globalShortcuts", qobject_cast<GlobalShortcutsInterface *>(plugin))
        || featureMismatch(Notifications, "notifications", qobject_cast<NotificationInterface *>(plugin))
        || featureMismatch(StickyNotes, "stickyNotes", qobject_cast<StickyNotesIntegrationInterface *>(plugin))) {
        loader.unload();
        return fail(LS_ErrAbi);
    }

    cache->instance   = plugin;
    cache->fileName   = fileName;
    cache->loadStatus = LS_Loaded;
    qnp->setHost(pluginHost);
    return LS_Loaded;
}

} // namespace QtNote
