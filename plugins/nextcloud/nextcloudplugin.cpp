#include "nextcloudplugin.h"

#include <memory>

#include "nextcloudstorage.h"
#include "notemanager.h"
#include "pluginhostinterface.h"

namespace QtNote {

NextcloudPlugin::NextcloudPlugin(QObject *parent) : QObject(parent) { }

NextcloudPlugin::~NextcloudPlugin() { shutdown(); }

void NextcloudPlugin::setHost(PluginHostInterface *host) { host_ = host; }

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
