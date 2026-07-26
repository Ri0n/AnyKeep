#include "xmppplugin.h"

#include <memory>

#include <QResource>

#include "notemanager.h"
#include "pluginhostinterface.h"
#include "qtnote_config.h"
#include "xmppstorage.h"

#ifdef QTNOTE_BUNDLED_PLUGIN_BUILD
static int initializeXmppPluginResources()
{
    Q_INIT_RESOURCE(xmppsettings);
    return 0;
}
#endif

namespace QtNote {

namespace {

    const QLatin1String pluginId("xmpp_pubsub_storage");

} // namespace

XmppPlugin::XmppPlugin(QObject *parent) : QObject(parent)
{
#ifdef QTNOTE_BUNDLED_PLUGIN_BUILD
    static const int resourcesInitialized = initializeXmppPluginResources();
    Q_UNUSED(resourcesInitialized);
#endif
}

XmppPlugin::~XmppPlugin() { shutdown(); }

int XmppPlugin::metadataVersion() const { return MetadataVersion; }

void XmppPlugin::setHost(PluginHostInterface *host) { host_ = host; }

PluginMetadata XmppPlugin::metadata()
{
    PluginMetadata metadata;
    metadata.id          = pluginId;
    metadata.icon        = QIcon(QStringLiteral(":/icons/xmpp-logo"));
    metadata.name        = QString("XMPP Private Notes");
    metadata.description = tr("Stores notes as private persistent items in the account's XMPP PEP service");
    metadata.author      = QString("Sergei Ilinykh");
    metadata.version     = 0x010000;
    metadata.minVersion  = 0x020300;
    metadata.maxVersion  = QTNOTE_VERSION;
    metadata.homepage    = QUrl(QStringLiteral("https://xmpp.org/extensions/xep-0223.html"));
    return metadata;
}

bool XmppPlugin::initialize()
{
    shutdown();
    auto *manager = host_ && host_->noteManager() ? host_->noteManager() : NoteManager::instance();
    if (!manager)
        return false;

    auto ownedStorage = std::make_unique<XmppStorage>(nullptr);
    storage_          = ownedStorage.get();
    manager->registerStorage(std::move(ownedStorage));

    // Keep an unconfigured remote storage enabled so its settings remain reachable.
    return true;
}

void XmppPlugin::shutdown()
{
    if (!storage_)
        return;
    auto *manager = host_ && host_->noteManager() ? host_->noteManager() : NoteManager::instance();
    if (manager)
        manager->unregisterStorage(storage_.data());
    storage_.clear();
}

} // namespace QtNote
