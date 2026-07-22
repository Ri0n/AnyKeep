#include "mobilebundledplugins.h"

#include "bundledpluginregistry.h"
#include "nextcloudplugin.h"

namespace QtNote {

void registerMobileBundledPlugins(BundledPluginRegistry &registry)
{
    PluginListSource::Entry nextcloud;
    nextcloud.id           = QStringLiteral("nextcloud_storage");
    nextcloud.name         = NextcloudPlugin::tr("Nextcloud Notes");
    nextcloud.description  = NextcloudPlugin::tr("Reads and writes notes using the Nextcloud Notes REST API");
    nextcloud.versionText  = QStringLiteral("1.0");
    nextcloud.loadPolicy   = PluginListSource::LP_Auto;
    nextcloud.configurable = true;
    registry.registerFactory(nextcloud, [](QObject *parent) { return new NextcloudPlugin(parent); });
}

} // namespace QtNote
