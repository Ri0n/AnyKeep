#ifndef GEMINIPLUGIN_H
#define GEMINIPLUGIN_H

#include <QNetworkAccessManager>
#include <QPointer>

#include "bundledplugininterface.h"
#include "qtnoteplugininterface.h"
#include "settingsproviderinterface.h"
#include "speechrecognitionprovider.h"

namespace QtNote {

class GeminiPlugin : public QObject,
                     public PluginInterface,
                     public RegularPluginInterface,
                     public PluginOptionsTooltipInterface,
                     public SettingsProviderInterface,
                     public SpeechRecognitionProviderInterface,
                     public BundledPluginInterface {
    Q_OBJECT
#ifndef QTNOTE_BUNDLED_PLUGIN_BUILD
    Q_PLUGIN_METADATA(IID "com.rion-soft.QtNote.gemini")
#endif
    Q_INTERFACES(
        QtNote::PluginInterface QtNote::RegularPluginInterface QtNote::PluginOptionsTooltipInterface
            QtNote::SettingsProviderInterface QtNote::SpeechRecognitionProviderInterface QtNote::BundledPluginInterface)
public:
    explicit GeminiPlugin(QObject *parent = nullptr);
    ~GeminiPlugin() override;

    int            metadataVersion() const override;
    PluginMetadata metadata() override;
    void           setHost(PluginHostInterface *host) override;

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

} // namespace QtNote

#endif // GEMINIPLUGIN_H
