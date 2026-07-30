#ifndef OPENAIWHISPERPLUGIN_H
#define OPENAIWHISPERPLUGIN_H

#include <QNetworkAccessManager>
#include <QPointer>

#include "bundledplugininterface.h"
#include "qtnoteplugininterface.h"
#include "settingsproviderinterface.h"
#include "speechrecognitionprovider.h"

namespace QtNote {

class OpenAIWhisperPlugin : public QObject,
                            public PluginInterface,
                            public RegularPluginInterface,
                            public PluginOptionsTooltipInterface,
                            public SettingsProviderInterface,
                            public SpeechRecognitionProviderInterface,
                            public BundledPluginInterface {
    Q_OBJECT
#ifndef QTNOTE_BUNDLED_PLUGIN_BUILD
#include "openaiwhisper_plugin_metadata.inc"
#endif
    Q_INTERFACES(
        QtNote::PluginInterface QtNote::RegularPluginInterface QtNote::PluginOptionsTooltipInterface
            QtNote::SettingsProviderInterface QtNote::SpeechRecognitionProviderInterface QtNote::BundledPluginInterface)
public:
    explicit OpenAIWhisperPlugin(QObject *parent = nullptr);
    ~OpenAIWhisperPlugin() override;
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
    friend class OpenAIWhisperSpeechJob;
    friend class OpenAIWhisperSettingsController;

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

} // namespace QtNote

#endif // OPENAIWHISPERPLUGIN_H
