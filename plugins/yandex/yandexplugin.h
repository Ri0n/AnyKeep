#ifndef YANDEXPLUGIN_H
#define YANDEXPLUGIN_H

#include <QNetworkAccessManager>

#include "bundledplugininterface.h"
#include "qtnoteplugininterface.h"
#include "settingsproviderinterface.h"
#include "speechrecognitionprovider.h"

namespace QtNote {

class YandexSpeechJob;
class YandexSettingsController;

class YandexPlugin : public QObject,
                     public PluginInterface,
                     public RegularPluginInterface,
                     public PluginOptionsTooltipInterface,
                     public SettingsProviderInterface,
                     public SpeechRecognitionProviderInterface,
                     public BundledPluginInterface {
    Q_OBJECT
#ifndef QTNOTE_BUNDLED_PLUGIN_BUILD
#include "yandex_plugin_metadata.inc"
#endif
    Q_INTERFACES(
        QtNote::PluginInterface QtNote::RegularPluginInterface QtNote::PluginOptionsTooltipInterface
            QtNote::SettingsProviderInterface QtNote::SpeechRecognitionProviderInterface QtNote::BundledPluginInterface)

public:
    explicit YandexPlugin(QObject *parent = nullptr);
    ~YandexPlugin() override;

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

signals:
    void usageChanged();

private:
    friend class YandexSpeechJob;
    friend class YandexSettingsController;

    struct Settings {
        QString apiKey;
        bool    deferredRecognition { false };
        bool    normalizeText { true };
        bool    literatureText { false };
    };

    Settings settings() const;
    void     saveSettings(const Settings &settings);
    void     addUsage(qint64 audioMs, int channels, qint64 bytesSent);
    void     resetUsage();

    QNetworkAccessManager *network_ = nullptr;
};

} // namespace QtNote

#endif // YANDEXPLUGIN_H
