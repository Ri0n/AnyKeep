#include "nextcloudplugin.h"

#include <memory>

#include "nextcloudstorage.h"
#include "notemanager.h"
#include "pluginhostinterface.h"
#include "qtnote_config.h"

namespace QtNote {

namespace {
    const QLatin1String pluginId("nextcloud_storage");
}

NextcloudPlugin::NextcloudPlugin(QObject *parent) : QObject(parent) { }

NextcloudPlugin::~NextcloudPlugin() { shutdown(); }

int NextcloudPlugin::metadataVersion() const { return MetadataVersion; }

void NextcloudPlugin::setHost(PluginHostInterface *host) { host_ = host; }

PluginMetadata NextcloudPlugin::metadata()
{
    PluginMetadata metadata;
    metadata.id          = pluginId;
    metadata.icon        = QIcon::fromTheme(QStringLiteral("folder-cloud"));
    metadata.name        = QString("Nextcloud Notes");
    metadata.description = tr("Reads and writes notes using the Nextcloud Notes REST API");
    metadata.author      = QString("Sergei Ilinykh");
    metadata.version     = 0x010000;
    metadata.minVersion  = 0x020300;
    metadata.maxVersion  = QTNOTE_VERSION;
    metadata.homepage    = QUrl(QStringLiteral("https://github.com/nextcloud/notes"));
    return metadata;
}

bool NextcloudPlugin::initialize()
{
    shutdown();
    auto ownedStorage = std::make_unique<NextcloudStorage>(nullptr);
    storage_          = ownedStorage.get();
    NoteManager::instance()->registerStorage(std::move(ownedStorage));
    // Keep the backend registered while it is not configured so its QML
    // settings remain reachable on both desktop and Android.
    return true;
}

void NextcloudPlugin::shutdown()
{
    if (!storage_)
        return;
    NoteManager::instance()->unregisterStorage(storage_.data());
    storage_.clear();
}

} // namespace QtNote
