#include "xmppplugin.h"

#include <memory>

#include <QResource>

#include "notemanager.h"
#include "pluginhostinterface.h"
#include "xmppstorage.h"

#ifdef ANYKEEP_BUNDLED_PLUGIN_BUILD
static int initializeXmppPluginResources()
{
    Q_INIT_RESOURCE(xmppsettings);
    return 0;
}
#endif

namespace AnyKeep {

XmppPlugin::XmppPlugin(QObject *parent) : QObject(parent)
{
#ifdef ANYKEEP_BUNDLED_PLUGIN_BUILD
    static const int resourcesInitialized = initializeXmppPluginResources();
    Q_UNUSED(resourcesInitialized);
#endif
}

XmppPlugin::~XmppPlugin() { shutdown(); }

void XmppPlugin::setHost(PluginHostInterface *host) { host_ = host; }

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

} // namespace AnyKeep
