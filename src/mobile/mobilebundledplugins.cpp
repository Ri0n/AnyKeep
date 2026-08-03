#include "mobilebundledplugins.h"

#include "bundledpluginregistry.h"
#include "geminiplugin.h"
#include "nextcloudplugin.h"
#include "openaiwhisperplugin.h"
#include "yandexplugin.h"

#include <QResource>
#ifdef ANYKEEP_MOBILE_XMPP_AVAILABLE
#include "xmppplugin.h"
#endif

static void initializeNextcloudResourcesForMobile() { Q_INIT_RESOURCE(nextcloudresources); }
static void initializeGeminiResourcesForMobile() { Q_INIT_RESOURCE(gemini); }
static void initializeOpenAIWhisperResourcesForMobile() { Q_INIT_RESOURCE(openaiwhisper); }
static void initializeYandexResourcesForMobile() { Q_INIT_RESOURCE(yandex); }
#ifdef ANYKEEP_MOBILE_XMPP_AVAILABLE
static void initializeXmppResourcesForMobile() { Q_INIT_RESOURCE(xmppsettings); }
#endif

namespace AnyKeep {

void registerMobileBundledPlugins(BundledPluginRegistry &registry)
{
    initializeNextcloudResourcesForMobile();

    PluginListSource::Entry nextcloud;
    nextcloud.id           = QStringLiteral("nextcloud_storage");
    nextcloud.name         = NextcloudPlugin::tr("Nextcloud Notes");
    nextcloud.description  = NextcloudPlugin::tr("Reads and writes notes using the Nextcloud Notes REST API");
    nextcloud.versionText  = QStringLiteral("1.0");
    nextcloud.iconSource   = QStringLiteral("qrc:/nextcloud/nextcloud-notes.svg");
    nextcloud.loadPolicy   = PluginListSource::LP_Auto;
    nextcloud.configurable = true;
    registry.registerFactory(nextcloud, [](QObject *parent) { return new NextcloudPlugin(parent); });

    initializeGeminiResourcesForMobile();
    PluginListSource::Entry gemini;
    gemini.id           = QStringLiteral("gemini");
    gemini.name         = GeminiPlugin::tr("Gemini speech recognition");
    gemini.description  = GeminiPlugin::tr("Transcribes live and recorded audio with the Gemini API");
    gemini.versionText  = QStringLiteral("0.0.1");
    gemini.iconSource   = QStringLiteral("qrc:/icons/gemini-logo");
    gemini.loadPolicy   = PluginListSource::LP_Auto;
    gemini.configurable = true;
    registry.registerFactory(gemini, [](QObject *parent) { return new GeminiPlugin(parent); });

    initializeOpenAIWhisperResourcesForMobile();
    PluginListSource::Entry openai;
    openai.id           = QStringLiteral("openaiwhisper");
    openai.name         = OpenAIWhisperPlugin::tr("OpenAI speech recognition");
    openai.description  = OpenAIWhisperPlugin::tr("Transcribes live and recorded audio with the OpenAI API");
    openai.versionText  = QStringLiteral("0.0.1");
    openai.iconSource   = QStringLiteral("qrc:/icons/openai-logo");
    openai.loadPolicy   = PluginListSource::LP_Disabled;
    openai.configurable = true;
    registry.registerFactory(openai, [](QObject *parent) { return new OpenAIWhisperPlugin(parent); });

    initializeYandexResourcesForMobile();
    PluginListSource::Entry yandex;
    yandex.id           = QStringLiteral("yandex");
    yandex.name         = YandexPlugin::tr("Yandex");
    yandex.description  = YandexPlugin::tr("Yandex cloud services, starting with SpeechKit speech recognition");
    yandex.versionText  = QStringLiteral("0.0.1");
    yandex.iconSource   = QStringLiteral("qrc:/icons/yandex-logo");
    yandex.loadPolicy   = PluginListSource::LP_Auto;
    yandex.configurable = true;
    registry.registerFactory(yandex, [](QObject *parent) { return new YandexPlugin(parent); });

#ifdef ANYKEEP_MOBILE_XMPP_AVAILABLE
    initializeXmppResourcesForMobile();

    PluginListSource::Entry xmpp;
    xmpp.id          = QStringLiteral("xmpp_pubsub_storage");
    xmpp.name        = XmppPlugin::tr("XMPP Private Notes");
    xmpp.description = XmppPlugin::tr("Synchronizes end-to-end encrypted notes through the account's XMPP PEP service");
    xmpp.versionText = QStringLiteral("1.0");
    xmpp.iconSource  = QStringLiteral("qrc:/icons/xmpp-logo");
    xmpp.loadPolicy  = PluginListSource::LP_Auto;
    xmpp.configurable = true;
    registry.registerFactory(xmpp, [](QObject *parent) { return new XmppPlugin(parent); });
#endif
}

} // namespace AnyKeep
