#include "xmppplugin.h"

#include <memory>

#include "notemanager.h"
#include "pluginhostinterface.h"
#include "qtnote_config.h"
#include "xmppstorage.h"

namespace QtNote {

namespace {

    const QLatin1String pluginId("xmpp_pubsub_storage");
    NoteStorage::Ptr    storage;

} // namespace

XmppPlugin::XmppPlugin(QObject *parent) : QObject(parent) { }

XmppPlugin::~XmppPlugin() { shutdown(); }

int XmppPlugin::metadataVersion() const { return MetadataVersion; }

void XmppPlugin::setHost(PluginHostInterface *host) { host_ = host; }

PluginMetadata XmppPlugin::metadata()
{
    PluginMetadata metadata;
    metadata.id          = pluginId;
    metadata.icon        = QIcon::fromTheme(QStringLiteral("im-jabber"));
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
    if (!host_ || !host_->noteManager())
        return false;
    auto ownedStorage = std::make_unique<XmppStorage>(nullptr);
    storage           = ownedStorage.get();
    host_->noteManager()->registerStorage(std::move(ownedStorage));

    // Keep an unconfigured remote storage enabled so its settings remain reachable.
    return true;
}

void XmppPlugin::shutdown()
{
    if (!storage)
        return;
    if (host_ && host_->noteManager())
        host_->noteManager()->unregisterStorage(storage.data());
    storage.clear();
}

} // namespace QtNote
