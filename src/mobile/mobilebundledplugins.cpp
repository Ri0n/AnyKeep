#include "mobilebundledplugins.h"

#include "bundledpluginregistry.h"
#include "nextcloudplugin.h"

#include <QIcon>
#include <QResource>
#ifdef QTNOTE_MOBILE_XMPP_AVAILABLE
#include "xmppplugin.h"
#endif

static void initializeNextcloudResourcesForMobile() { Q_INIT_RESOURCE(nextcloudresources); }

namespace QtNote {

void registerMobileBundledPlugins(BundledPluginRegistry &registry)
{
    initializeNextcloudResourcesForMobile();

    PluginListSource::Entry nextcloud;
    nextcloud.id           = QStringLiteral("nextcloud_storage");
    nextcloud.name         = NextcloudPlugin::tr("Nextcloud Notes");
    nextcloud.description  = NextcloudPlugin::tr("Reads and writes notes using the Nextcloud Notes REST API");
    nextcloud.versionText  = QStringLiteral("1.0");
    nextcloud.iconSource   = QStringLiteral("qrc:/nextcloud/nextcloud-notes.svg");
    nextcloud.icon         = QIcon(QStringLiteral(":/nextcloud/nextcloud-notes.svg"));
    nextcloud.loadPolicy   = PluginListSource::LP_Auto;
    nextcloud.configurable = true;
    registry.registerFactory(nextcloud, [](QObject *parent) { return new NextcloudPlugin(parent); });

#ifdef QTNOTE_MOBILE_XMPP_AVAILABLE
    PluginListSource::Entry xmpp;
    xmpp.id          = QStringLiteral("xmpp_pubsub_storage");
    xmpp.name        = XmppPlugin::tr("XMPP Private Notes");
    xmpp.description = XmppPlugin::tr("Synchronizes end-to-end encrypted notes through the account's XMPP PEP service");
    xmpp.versionText = QStringLiteral("1.0");
    xmpp.loadPolicy  = PluginListSource::LP_Auto;
    xmpp.configurable = true;
    registry.registerFactory(xmpp, [](QObject *parent) { return new XmppPlugin(parent); });
#endif
}

} // namespace QtNote
