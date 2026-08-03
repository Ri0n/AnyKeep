#include "yandexplugin.h"

#include <QSettings>

#include <QtPlugin>
#include <cmath>

#include "yandexsettingscontroller.h"
#include "yandexspeechjob.h"
#include "yandexspeechutils.h"

namespace AnyKeep {

static const QLatin1String SettingsGroup("yandex");

namespace {

    QString formatUsageDuration(qint64 milliseconds)
    {
        const double seconds  = qMax<qint64>(0, milliseconds) / 1000.0;
        const int    decimals = seconds < 60.0 && std::fmod(seconds, 1.0) > 0.0001 ? 1 : 0;
        return YandexPlugin::tr("%1 s").arg(QLocale().toString(seconds, 'f', decimals));
    }

    QString formatUsageBytes(qint64 bytes)
    {
        const qint64 safeBytes = qMax<qint64>(0, bytes);
        if (safeBytes < 1024)
            return YandexPlugin::tr("%1 B").arg(QLocale().toString(safeBytes));
        const double kibibytes = safeBytes / 1024.0;
        if (kibibytes < 1024.0)
            return YandexPlugin::tr("%1 KiB").arg(QLocale().toString(kibibytes, 'f', 1));
        return YandexPlugin::tr("%1 MiB").arg(QLocale().toString(kibibytes / 1024.0, 'f', 1));
    }

} // namespace

YandexPlugin::YandexPlugin(QObject *parent) : QObject(parent), network_(new QNetworkAccessManager(this)) { }
YandexPlugin::~YandexPlugin() = default;

void YandexPlugin::setHost(PluginHostInterface *host) { Q_UNUSED(host); }
bool YandexPlugin::initialize() { return true; }
void YandexPlugin::shutdown() { }

QString YandexPlugin::tooltip() const
{
    QStringList lines;
    const auto  s = settings();
    if (isSpeechRecognitionReady()) {
        lines.append(tr("<b>SpeechKit STT:</b> ready"));
        lines.append(s.deferredRecognition ? tr("<b>Mode:</b> deferred") : tr("<b>Mode:</b> standard"));
    } else {
        lines.append(tr("<b>SpeechKit STT:</b> API key is not set"));
    }
    lines.append(speechRecognitionUsageStats().humanSummary);
    return lines.join(QLatin1String("<br/>"));
}

QUrl YandexPlugin::settingsComponent() const { return QUrl(QStringLiteral("qrc:/qml/YandexSettings.qml")); }

SettingsController *YandexPlugin::createSettingsController(QObject *parent)
{
    return new YandexSettingsController(this, parent);
}

bool YandexPlugin::isSpeechRecognitionReady() const { return !settings().apiKey.isEmpty(); }

SpeechRecognitionCapabilities YandexPlugin::speechRecognitionCapabilities() const
{
    SpeechRecognitionCapabilities caps;
    caps.supportsOneShot     = true;
    caps.supportsPunctuation = true;
    // This limit controls AnyKeep's in-memory live PCM recorder. Recorded
    // attachments use the provider job directly and retain SpeechKit's
    // four-hour asynchronous file limit.
    caps.maxOneShotDurationMs = 2 * 60 * 1000;
    caps.languages
        = { QStringLiteral("de-DE"), QStringLiteral("en-US"), QStringLiteral("es-ES"), QStringLiteral("fi-FI"),
            QStringLiteral("fr-FR"), QStringLiteral("he-IL"), QStringLiteral("it-IT"), QStringLiteral("kk-KZ"),
            QStringLiteral("nl-NL"), QStringLiteral("pl-PL"), QStringLiteral("pt-PT"), QStringLiteral("pt-BR"),
            QStringLiteral("ru-RU"), QStringLiteral("sv-SE"), QStringLiteral("tr-TR"), QStringLiteral("uz-UZ") };
    caps.preferredLanguage = QStringLiteral("ru-RU");
    caps.encodedAudioMediaTypes
        = { QStringLiteral("audio/mp4"),  QStringLiteral("audio/m4a"),  QStringLiteral("audio/x-m4a"),
            QStringLiteral("audio/aac"),  QStringLiteral("audio/mpeg"), QStringLiteral("audio/mp3"),
            QStringLiteral("audio/ogg"),  QStringLiteral("audio/opus"), QStringLiteral("audio/wav"),
            QStringLiteral("audio/x-wav") };
    return caps;
}

SpeechRecognitionUsageStats YandexPlugin::speechRecognitionUsageStats() const
{
    QSettings s;
    s.beginGroup(SettingsGroup);
    SpeechRecognitionUsageStats stats;
    stats.available               = true;
    const qint64 trackedAudioMs   = s.value(QLatin1String("speechkit/stats/v2/audioMsUsed"), 0).toLongLong();
    const qint64 trackedBytesSent = s.value(QLatin1String("speechkit/stats/v2/bytesSent"), 0).toLongLong();
    const qint64 sessionCount     = s.value(QLatin1String("speechkit/stats/v2/sessionCount"), 0).toLongLong();
    const qint64 billableAudioMs  = s.value(QLatin1String("speechkit/stats/v2/billableAudioMs"), 0).toLongLong();
    stats.audioMsUsed             = trackedAudioMs;
    stats.bytesSent               = trackedBytesSent;
    stats.humanSummary            = tr("<b>Tracked audio sessions:</b> %1<br/><b>Actual audio:</b> %2<br/>"
                                                  "<b>Estimated billable audio:</b> %3<br/><b>Uploaded:</b> %4")
                             .arg(QLocale().toString(sessionCount), formatUsageDuration(stats.audioMsUsed),
                                  formatUsageDuration(billableAudioMs), formatUsageBytes(stats.bytesSent));
    return stats;
}

SpeechRecognitionJob *YandexPlugin::recognizeSpeech(const SpeechRecognitionAudio   &audio,
                                                    const SpeechRecognitionRequest &request)
{
    return new YandexSpeechJob(this, audio, request);
}

YandexPlugin::Settings YandexPlugin::settings() const
{
    QSettings s;
    s.beginGroup(SettingsGroup);
    Settings result;
    result.apiKey              = s.value(QLatin1String("apiKey")).toString().trimmed();
    result.deferredRecognition = s.value(QLatin1String("speechkit/deferredRecognition"), false).toBool();
    result.normalizeText       = s.value(QLatin1String("speechkit/normalizeText"), true).toBool();
    result.literatureText      = s.value(QLatin1String("speechkit/literatureText"), false).toBool();
    return result;
}

void YandexPlugin::saveSettings(const Settings &settings)
{
    QSettings s;
    s.beginGroup(SettingsGroup);
    s.setValue(QLatin1String("apiKey"), settings.apiKey.trimmed());
    s.setValue(QLatin1String("speechkit/deferredRecognition"), settings.deferredRecognition);
    s.setValue(QLatin1String("speechkit/normalizeText"), settings.normalizeText);
    s.setValue(QLatin1String("speechkit/literatureText"), settings.literatureText);
}

void YandexPlugin::addUsage(qint64 audioMs, int channels, qint64 bytesSent)
{
    const qint64 safeAudioMs     = qMax<qint64>(0, audioMs);
    const qint64 billableAudioMs = YandexSpeech::estimatedAsyncBillableAudioMs(safeAudioMs, channels);

    QSettings s;
    s.beginGroup(SettingsGroup);
    s.setValue(QLatin1String("speechkit/stats/v2/sessionCount"),
               s.value(QLatin1String("speechkit/stats/v2/sessionCount"), 0).toLongLong() + 1);
    s.setValue(QLatin1String("speechkit/stats/v2/audioMsUsed"),
               s.value(QLatin1String("speechkit/stats/v2/audioMsUsed"), 0).toLongLong() + safeAudioMs);
    s.setValue(QLatin1String("speechkit/stats/v2/billableAudioMs"),
               s.value(QLatin1String("speechkit/stats/v2/billableAudioMs"), 0).toLongLong() + billableAudioMs);
    s.setValue(QLatin1String("speechkit/stats/v2/bytesSent"),
               s.value(QLatin1String("speechkit/stats/v2/bytesSent"), 0).toLongLong() + qMax<qint64>(0, bytesSent));
}

} // namespace AnyKeep
