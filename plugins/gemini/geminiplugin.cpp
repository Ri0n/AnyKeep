#include "geminiplugin.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrlQuery>
#include <QtEndian>
#include <QtPlugin>

#include "qtnote_config.h"
#include "settingscontroller.h"

namespace QtNote {

static const QLatin1String SettingsGroup("gemini");
static const QLatin1String DefaultModel("gemini-3.1-flash-lite");

static QString defaultPrompt()
{
    return GeminiPlugin::tr("Transcribe this speech to text. Return only the transcript without comments.");
}

static void appendLE16(QByteArray *data, quint16 value)
{
    char buf[2];
    qToLittleEndian(value, buf);
    data->append(buf, sizeof(buf));
}

static void appendLE32(QByteArray *data, quint32 value)
{
    char buf[4];
    qToLittleEndian(value, buf);
    data->append(buf, sizeof(buf));
}

static QByteArray wavFromPcm(const SpeechRecognitionAudio &audio)
{
    QByteArray wav;
    const auto byteRate   = quint32(audio.sampleRate * audio.channels * audio.bitsPerSample / 8);
    const auto blockAlign = quint16(audio.channels * audio.bitsPerSample / 8);

    wav.append("RIFF", 4);
    appendLE32(&wav, quint32(36 + audio.data.size()));
    wav.append("WAVE", 4);
    wav.append("fmt ", 4);
    appendLE32(&wav, 16);
    appendLE16(&wav, 1);
    appendLE16(&wav, quint16(audio.channels));
    appendLE32(&wav, quint32(audio.sampleRate));
    appendLE32(&wav, byteRate);
    appendLE16(&wav, blockAlign);
    appendLE16(&wav, quint16(audio.bitsPerSample));
    wav.append("data", 4);
    appendLE32(&wav, quint32(audio.data.size()));
    wav.append(audio.data);
    return wav;
}

static QString apiError(QNetworkReply *reply, const QByteArray &body)
{
    auto doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        auto obj     = doc.object();
        auto error   = obj.value(QLatin1String("error")).toObject();
        auto message = error.value(QLatin1String("message")).toString();
        if (!message.isEmpty()) {
            return message;
        }
    }
    return reply->errorString();
}

static QString textFromResponseValue(const QJsonValue &value)
{
    if (value.isString()) {
        return value.toString();
    }
    if (value.isArray()) {
        QStringList parts;
        for (const auto &item : value.toArray()) {
            auto text = textFromResponseValue(item);
            if (!text.isEmpty()) {
                parts.append(text);
            }
        }
        return parts.join(QString());
    }
    if (value.isObject()) {
        auto obj = value.toObject();
        for (const auto &key : { QLatin1String("output_text"), QLatin1String("outputText"), QLatin1String("transcript"),
                                 QLatin1String("text") }) {
            auto text = obj.value(key).toString();
            if (!text.isEmpty()) {
                return text;
            }
        }
        for (const auto &key :
             { QLatin1String("parts"), QLatin1String("content"), QLatin1String("message"), QLatin1String("result"),
               QLatin1String("response"), QLatin1String("output"), QLatin1String("outputs"), QLatin1String("steps") }) {
            auto text = textFromResponseValue(obj.value(key));
            if (!text.isEmpty()) {
                return text;
            }
        }
    }
    return QString();
}

static QString transcriptFromResponse(const QByteArray &body)
{
    auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        return QString();
    }

    auto obj = doc.object();
    for (const auto &key : { QLatin1String("output_text"), QLatin1String("outputText") }) {
        auto text = obj.value(key).toString();
        if (!text.isEmpty()) {
            return text.trimmed();
        }
    }

    auto candidates = obj.value(QLatin1String("candidates")).toArray();
    if (!candidates.isEmpty()) {
        auto content = candidates.first().toObject().value(QLatin1String("content")).toObject();
        return textFromResponseValue(content.value(QLatin1String("parts"))).trimmed();
    }

    auto output = obj.value(QLatin1String("output"));
    auto text   = textFromResponseValue(output).trimmed();
    if (!text.isEmpty()) {
        return text;
    }

    return textFromResponseValue(obj).trimmed();
}

class GeminiSpeechJob : public SpeechRecognitionJob {
    Q_OBJECT
public:
    GeminiSpeechJob(GeminiPlugin *plugin, const SpeechRecognitionAudio &audio,
                    const SpeechRecognitionRequest &request) :
        SpeechRecognitionJob(plugin), plugin(plugin), audio(audio), request(request)
    {
        QMetaObject::invokeMethod(this, &GeminiSpeechJob::start, Qt::QueuedConnection);
    }

public slots:
    void cancel() override
    {
        cancelled = true;
        if (reply) {
            auto r = reply;
            reply  = nullptr;
            r->abort();
            r->deleteLater();
        }
    }

private slots:
    void start()
    {
        auto settings = plugin->settings();
        if (settings.apiKey.isEmpty()) {
            qWarning() << "Gemini speech job failed before request: API key is empty";
            emit failed(tr("Gemini API key is empty"));
            return;
        }

        auto wav = wavFromPcm(audio);
        if (wav.size() > 20 * 1024 * 1024) {
            qWarning() << "Gemini speech job failed before request: inline audio is too large"
                       << "wavBytes" << wav.size();
            emit failed(tr("Audio is too large for inline Gemini request"));
            return;
        }

        QJsonArray input;
        auto       promptText = settings.prompt.isEmpty() ? defaultPrompt() : settings.prompt;
        if (!request.language.isEmpty()) {
            promptText += QLatin1Char('\n') + tr("Expected speech language: %1.").arg(request.language);
        }

        QJsonObject textPart;
        textPart.insert(QLatin1String("type"), QLatin1String("text"));
        textPart.insert(QLatin1String("text"), promptText);
        input.append(textPart);

        QJsonObject audioPart;
        audioPart.insert(QLatin1String("type"), QLatin1String("audio"));
        auto audioBase64 = QString::fromLatin1(wav.toBase64());
        audioPart.insert(QLatin1String("data"), audioBase64);
        audioPart.insert(QLatin1String("mime_type"), QLatin1String("audio/wav"));
        input.append(audioPart);

        QJsonObject root;
        root.insert(QLatin1String("model"), settings.model.isEmpty() ? DefaultModel : settings.model);
        root.insert(QLatin1String("input"), input);

        auto payload = QJsonDocument(root).toJson(QJsonDocument::Compact);

        const QUrl      url(QLatin1String("https://generativelanguage.googleapis.com/v1beta/interactions"));
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QLatin1String("application/json"));
        req.setRawHeader("x-goog-api-key", settings.apiKey.toUtf8());

        reply = plugin->network->post(req, payload);
        connect(reply, &QNetworkReply::finished, this, [this, payload]() {
            if (!reply) {
                return;
            }
            auto r = reply;
            reply  = nullptr;
            if (cancelled) {
                r->deleteLater();
                return;
            }
            auto body = r->readAll();
            r->deleteLater();
            auto httpStatus = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (r->error() != QNetworkReply::NoError) {
                auto error = apiError(r, body);
                qWarning() << "Gemini speech request failed:"
                           << "error" << error << "responsePreview" << QString::fromUtf8(body.left(1000));
                emit failed(error);
                return;
            }

            auto text = transcriptFromResponse(body);
            if (text.isEmpty()) {
                qWarning() << "Gemini speech response parse failed:"
                           << "responsePreview" << QString::fromUtf8(body.left(4000));
                emit failed(tr("Gemini response did not contain recognized text"));
                return;
            }

            plugin->addUsage(audio.durationMs, payload.size());
            emit finished(text);
        });
    }

private:
    GeminiPlugin            *plugin;
    SpeechRecognitionAudio   audio;
    SpeechRecognitionRequest request;
    QPointer<QNetworkReply>  reply;
    bool                     cancelled = false;
};

class GeminiSettingsController final : public SettingsController {
public:
    explicit GeminiSettingsController(GeminiPlugin *plugin, QObject *parent) :
        SettingsController(parent), plugin_(plugin)
    {
        const auto settings = plugin_->settings();
        addField({ QStringLiteral("apiKey"), tr("API key"), QString(), Password, settings.apiKey });
        addField({ QStringLiteral("model"), tr("Model"), QString(), Text, settings.model });
        addField({ QStringLiteral("prompt"), tr("Transcription prompt"), QString(), Multiline, settings.prompt });
        const auto stats = plugin_->speechRecognitionUsageStats();
        addField({ QStringLiteral("usage"), tr("Usage"), QString(), ReadOnly, stats.humanSummary });
    }

protected:
    bool applyValues(const QVariantMap &values, QString *) override
    {
        GeminiPlugin::Settings settings;
        settings.apiKey = values.value(QStringLiteral("apiKey")).toString().trimmed();
        settings.model  = values.value(QStringLiteral("model")).toString().trimmed();
        settings.prompt = values.value(QStringLiteral("prompt")).toString().trimmed();
        plugin_->saveSettings(settings);
        return true;
    }

private:
    GeminiPlugin *plugin_;
};

GeminiPlugin::GeminiPlugin(QObject *parent) : QObject(parent) { network = new QNetworkAccessManager(this); }

GeminiPlugin::~GeminiPlugin() = default;

int GeminiPlugin::metadataVersion() const { return MetadataVersion; }

PluginMetadata GeminiPlugin::metadata()
{
    PluginMetadata md;
    md.id          = QLatin1String("gemini");
    md.name        = tr("Gemini");
    md.description = tr("Gemini API speech recognition backend");
    md.author      = QLatin1String("Rion");
    md.version     = 0x000100;
    md.minVersion  = 0x030200;
    md.maxVersion  = QTNOTE_VERSION;
    md.extra.insert(QStringLiteral("configurable"), true);
    return md;
}

void GeminiPlugin::setHost(PluginHostInterface *host) { Q_UNUSED(host); }

bool GeminiPlugin::initialize() { return true; }

void GeminiPlugin::shutdown() { }

QString GeminiPlugin::tooltip() const
{
    QStringList lines;
    lines.append(isSpeechRecognitionReady() ? tr("<b>Speech:</b> ready") : tr("<b>Speech:</b> API key is not set"));
    auto stats = speechRecognitionUsageStats();
    lines.append(stats.humanSummary);
    return lines.join(QLatin1String("<br/>"));
}

QUrl GeminiPlugin::settingsComponent() const { return QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml")); }

SettingsController *GeminiPlugin::createSettingsController(QObject *parent)
{
    return new GeminiSettingsController(this, parent);
}

bool GeminiPlugin::isSpeechRecognitionReady() const
{
    auto s     = settings();
    auto ready = !s.apiKey.isEmpty();
    return ready;
}

SpeechRecognitionCapabilities GeminiPlugin::speechRecognitionCapabilities() const
{
    SpeechRecognitionCapabilities caps;
    caps.supportsOneShot      = true;
    caps.supportsPunctuation  = true;
    caps.maxOneShotDurationMs = 120000;
    return caps;
}

SpeechRecognitionUsageStats GeminiPlugin::speechRecognitionUsageStats() const
{
    QSettings s;
    s.beginGroup(SettingsGroup);
    SpeechRecognitionUsageStats stats;
    stats.available    = true;
    stats.audioMsUsed  = s.value(QLatin1String("stats/audioMsUsed"), 0).toLongLong();
    stats.bytesSent    = s.value(QLatin1String("stats/bytesSent"), 0).toLongLong();
    auto approxTokens  = stats.audioMsUsed * 32 / 1000;
    stats.humanSummary = tr("<b>Used:</b> %1 seconds, about %2 audio input tokens, %3 bytes sent")
                             .arg(stats.audioMsUsed / 1000)
                             .arg(approxTokens)
                             .arg(stats.bytesSent);
    return stats;
}

SpeechRecognitionJob *GeminiPlugin::recognizeSpeech(const SpeechRecognitionAudio   &audio,
                                                    const SpeechRecognitionRequest &request)
{
    return new GeminiSpeechJob(this, audio, request);
}

GeminiPlugin::Settings GeminiPlugin::settings() const
{
    QSettings s;
    s.beginGroup(SettingsGroup);
    Settings settings;
    settings.apiKey = s.value(QLatin1String("apiKey")).toString();
    settings.model  = s.value(QLatin1String("model"), DefaultModel).toString().trimmed();
    if (settings.model.isEmpty()) {
        settings.model = DefaultModel;
    }
    settings.prompt = s.value(QLatin1String("prompt"), defaultPrompt()).toString();
    return settings;
}

void GeminiPlugin::saveSettings(const Settings &settings)
{
    QSettings s;
    s.beginGroup(SettingsGroup);
    auto model = settings.model.trimmed();
    if (model.isEmpty()) {
        model = DefaultModel;
    }
    auto prompt = settings.prompt.isEmpty() ? defaultPrompt() : settings.prompt;
    s.setValue(QLatin1String("apiKey"), settings.apiKey);
    s.setValue(QLatin1String("model"), model);
    s.setValue(QLatin1String("prompt"), prompt);
}

void GeminiPlugin::addUsage(qint64 audioMs, qint64 bytesSent)
{
    QSettings s;
    s.beginGroup(SettingsGroup);
    s.setValue(QLatin1String("stats/audioMsUsed"),
               s.value(QLatin1String("stats/audioMsUsed"), 0).toLongLong() + audioMs);
    s.setValue(QLatin1String("stats/bytesSent"), s.value(QLatin1String("stats/bytesSent"), 0).toLongLong() + bytesSent);
}

} // namespace QtNote

#include "geminiplugin.moc"
