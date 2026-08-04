#ifndef GEMINIPLUGIN_H
#define GEMINIPLUGIN_H

#include <QNetworkAccessManager>
#include <QPointer>

#include "anykeepplugininterface.h"
#include "bundledplugininterface.h"
#include "settingsproviderinterface.h"
#include "speechrecognitionprovider.h"

namespace AnyKeep {

class GeminiPlugin : public QObject,
                     public PluginInterface,
                     public RegularPluginInterface,
                     public PluginOptionsTooltipInterface,
                     public SettingsProviderInterface,
                     public SpeechRecognitionProviderInterface,
                     public BundledPluginInterface {
    Q_OBJECT
#ifndef ANYKEEP_BUNDLED_PLUGIN_BUILD
#include "gemini_plugin_metadata.inc"
#endif
    Q_INTERFACES(AnyKeep::PluginInterface AnyKeep::RegularPluginInterface AnyKeep::PluginOptionsTooltipInterface
                     AnyKeep::SettingsProviderInterface AnyKeep::SpeechRecognitionProviderInterface
                                                        AnyKeep::BundledPluginInterface)
public:
    explicit GeminiPlugin(QObject *parent = nullptr);
    ~GeminiPlugin() override;
    void setHost(PluginHostInterface *host) override;

    bool initialize() override;
    void shutdown() override;

    QString             tooltip() const override;
    QUrl                settingsComponent() const override;
    SettingsController *createSettingsController(QObject *parent) override;

    bool                          isSpeechRecognitionReady() const override;
    SpeechRecognitionCapabilities speechRecognitionCapabilities() const override;
    SpeechRecognitionUsageStats   speechRecognitionUsageStats() const override;
    SpeechRecognitionJob         *recognizeSpeech(const SpeechRecognitionAudio   &audio,
                                                  const SpeechRecognitionRequest &request) override;

private:
    friend class GeminiSpeechJob;
    friend class GeminiSettingsController;

    struct Settings {
        QString apiKey;
        QString model;
        QString prompt;
    };

    Settings settings() const;
    void     saveSettings(const Settings &settings);
    void     addUsage(qint64 audioMs, qint64 bytesSent);

    QNetworkAccessManager *network = nullptr;
};

} // namespace AnyKeep

#endif // GEMINIPLUGIN_H
