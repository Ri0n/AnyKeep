#include "openaiwhisperplugin.h"

#include <QBuffer>
#include <QDebug>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QtEndian>
#include <QtPlugin>

#include "qtnote_config.h"
#include "settingscontroller.h"

namespace QtNote {

static const QLatin1String SettingsGroup("openaiwhisper");
static const QLatin1String DefaultModel("whisper-1");

static QString defaultPrompt() { return OpenAIWhisperPlugin::tr("Transcribe this speech to text."); }

static QString billingNotice()
{
    return OpenAIWhisperPlugin::tr(
        "OpenAI API usage is paid. Transcription requests are billed to the OpenAI account that owns this API key.");
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
        auto error   = doc.object().value(QLatin1String("error")).toObject();
        auto message = error.value(QLatin1String("message")).toString();
        if (!message.isEmpty()) {
            return message;
        }
    }
    return reply->errorString();
}

static QString transcriptFromResponse(const QByteArray &body)
{
    auto doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) {
        return QString::fromUtf8(body).trimmed();
    }
    return doc.object().value(QLatin1String("text")).toString().trimmed();
}

static void appendTextPart(QHttpMultiPart *multipart, const QByteArray &name, const QString &value)
{
    if (value.isEmpty()) {
        return;
    }

    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                   QString::fromLatin1("form-data; name=\"%1\"").arg(QString::fromLatin1(name)));
    part.setBody(value.toUtf8());
    multipart->append(part);
}

class OpenAIWhisperSpeechJob : public SpeechRecognitionJob {
    Q_OBJECT
public:
    OpenAIWhisperSpeechJob(OpenAIWhisperPlugin *plugin, const SpeechRecognitionAudio &audio,
                           const SpeechRecognitionRequest &request) :
        SpeechRecognitionJob(plugin), plugin(plugin), audio(audio), request(request)
    {
        QMetaObject::invokeMethod(this, &OpenAIWhisperSpeechJob::start, Qt::QueuedConnection);
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
        qDebug() << "OpenAI Whisper speech job starting:"
                 << "audioBytes" << audio.data.size() << "durationMs" << audio.durationMs << "sampleRate"
                 << audio.sampleRate << "channels" << audio.channels << "bitsPerSample" << audio.bitsPerSample
                 << "format" << audio.format << "language" << request.language << "contextIdPresent"
                 << !request.contextId.isEmpty() << "model"
                 << (settings.model.isEmpty() ? QString(DefaultModel) : settings.model);
        if (settings.apiKey.isEmpty()) {
            qDebug() << "OpenAI Whisper speech job failed before request: API key is empty";
            emit failed(tr("OpenAI API key is empty"));
            return;
        }

        auto wav = wavFromPcm(audio);
        qDebug() << "OpenAI Whisper speech job prepared WAV:"
                 << "wavBytes" << wav.size() << "pcmBytes" << audio.data.size();
        if (wav.size() > 25 * 1024 * 1024) {
            qDebug() << "OpenAI Whisper speech job failed before request: audio is too large"
                     << "wavBytes" << wav.size();
            emit failed(tr("Audio is too large for OpenAI transcription request"));
            return;
        }

        auto multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        appendTextPart(multipart, "model", settings.model.isEmpty() ? DefaultModel : settings.model);
        appendTextPart(multipart, "response_format", QLatin1String("json"));
        appendTextPart(multipart, "prompt", settings.prompt.isEmpty() ? defaultPrompt() : settings.prompt);
        if (!request.language.isEmpty()) {
            auto language = request.language.left(2).toLower();
            appendTextPart(multipart, "language", language);
        }

        auto wavBuffer = new QBuffer(multipart);
        wavBuffer->setData(wav);
        wavBuffer->open(QIODevice::ReadOnly);

        QHttpPart filePart;
        filePart.setHeader(QNetworkRequest::ContentTypeHeader, QLatin1String("audio/wav"));
        filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QLatin1String("form-data; name=\"file\"; filename=\"speech.wav\""));
        filePart.setBodyDevice(wavBuffer);
        multipart->append(filePart);

        const QUrl      url(QLatin1String("https://api.openai.com/v1/audio/transcriptions"));
        QNetworkRequest req(url);
        req.setRawHeader("Authorization", QByteArray("Bearer ") + settings.apiKey.toUtf8());

        qDebug() << "OpenAI Whisper speech request posting:"
                 << "url" << url << "wavBytes" << wav.size() << "promptChars"
                 << (settings.prompt.isEmpty() ? defaultPrompt() : settings.prompt).size();
        reply = plugin->network->post(req, multipart);
        multipart->setParent(reply);
        connect(reply, &QNetworkReply::finished, this, [this, wavSize = wav.size()]() {
            if (!reply) {
                qDebug() << "OpenAI Whisper speech reply finished after reply was already cleared";
                return;
            }
            auto r = reply;
            reply  = nullptr;
            if (cancelled) {
                qDebug() << "OpenAI Whisper speech request finished after cancellation";
                r->deleteLater();
                return;
            }
            auto body       = r->readAll();
            auto httpStatus = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            qDebug() << "OpenAI Whisper speech reply finished:"
                     << "networkError" << r->error() << "errorString" << r->errorString() << "httpStatus" << httpStatus
                     << "responseBytes" << body.size();
            if (r->error() != QNetworkReply::NoError) {
                auto error = apiError(r, body);
                qDebug() << "OpenAI Whisper speech request failed:"
                         << "error" << error << "responsePreview" << QString::fromUtf8(body.left(1000));
                r->deleteLater();
                emit failed(error);
                return;
            }
            r->deleteLater();

            auto text = transcriptFromResponse(body);
            if (text.isEmpty()) {
                qDebug() << "OpenAI Whisper speech response parse failed:"
                         << "responsePreview" << QString::fromUtf8(body.left(4000));
                emit failed(tr("OpenAI response did not contain recognized text"));
                return;
            }

            plugin->addUsage(audio.durationMs, wavSize);
            qDebug() << "OpenAI Whisper speech recognition finished:"
                     << "textChars" << text.size() << "audioDurationMs" << audio.durationMs << "wavBytes" << wavSize;
            emit finished(text);
        });
    }

private:
    OpenAIWhisperPlugin     *plugin;
    SpeechRecognitionAudio   audio;
    SpeechRecognitionRequest request;
    QPointer<QNetworkReply>  reply;
    bool                     cancelled = false;
};

class OpenAIWhisperSettingsController final : public SettingsController {
public:
    explicit OpenAIWhisperSettingsController(OpenAIWhisperPlugin *plugin, QObject *parent) :
        SettingsController(parent), plugin_(plugin)
    {
        const auto settings = plugin_->settings();
        addField({ QStringLiteral("apiKey"), tr("API key"), billingNotice(), Password, settings.apiKey });
        addField({ QStringLiteral("model"), tr("Model"), QString(), Text, settings.model });
        addField({ QStringLiteral("prompt"), tr("Transcription prompt"), QString(), Multiline, settings.prompt });
        const auto stats = plugin_->speechRecognitionUsageStats();
        addField({ QStringLiteral("usage"), tr("Usage"), QString(), ReadOnly, stats.humanSummary });
    }

protected:
    bool applyValues(const QVariantMap &values, QString *) override
    {
        OpenAIWhisperPlugin::Settings settings;
        settings.apiKey = values.value(QStringLiteral("apiKey")).toString().trimmed();
        settings.model  = values.value(QStringLiteral("model")).toString().trimmed();
        settings.prompt = values.value(QStringLiteral("prompt")).toString().trimmed();
        plugin_->saveSettings(settings);
        return true;
    }

private:
    OpenAIWhisperPlugin *plugin_;
};

OpenAIWhisperPlugin::OpenAIWhisperPlugin(QObject *parent) : QObject(parent) { }

OpenAIWhisperPlugin::~OpenAIWhisperPlugin() = default;

int OpenAIWhisperPlugin::metadataVersion() const { return MetadataVersion; }

PluginMetadata OpenAIWhisperPlugin::metadata()
{
    PluginMetadata md;
    md.id          = QLatin1String("openaiwhisper");
    md.name        = tr("OpenAI Whisper");
    md.description = tr("OpenAI Whisper speech recognition backend");
    md.author      = QLatin1String("Rion");
    md.version     = 0x000100;
    md.minVersion  = 0x030200;
    md.maxVersion  = QTNOTE_VERSION;
    md.extra.insert(QStringLiteral("configurable"), true);
    md.extra.insert(QLatin1String("defaultLoadPolicy"), QLatin1String("disabled"));
    return md;
}

void OpenAIWhisperPlugin::setHost(PluginHostInterface *host) { Q_UNUSED(host); }

bool OpenAIWhisperPlugin::initialize()
{
    if (!network)
        network = new QNetworkAccessManager(this);
    return true;
}

void OpenAIWhisperPlugin::shutdown() { }

QString OpenAIWhisperPlugin::tooltip() const
{
    QStringList lines;
    lines.append(isSpeechRecognitionReady() ? tr("<b>Speech:</b> ready") : tr("<b>Speech:</b> API key is not set"));
    lines.append(billingNotice());
    auto stats = speechRecognitionUsageStats();
    lines.append(stats.humanSummary);
    return lines.join(QLatin1String("<br/>"));
}

QUrl OpenAIWhisperPlugin::settingsComponent() const { return QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml")); }

SettingsController *OpenAIWhisperPlugin::createSettingsController(QObject *parent)
{
    return new OpenAIWhisperSettingsController(this, parent);
}

bool OpenAIWhisperPlugin::isSpeechRecognitionReady() const
{
    auto s     = settings();
    auto ready = !s.apiKey.isEmpty();
    qDebug() << "OpenAI Whisper speech readiness:" << ready << "apiKeyPresent" << !s.apiKey.isEmpty() << "model"
             << s.model;
    return ready;
}

SpeechRecognitionCapabilities OpenAIWhisperPlugin::speechRecognitionCapabilities() const
{
    SpeechRecognitionCapabilities caps;
    caps.supportsOneShot      = true;
    caps.supportsPunctuation  = true;
    caps.maxOneShotDurationMs = 120000;
    return caps;
}

SpeechRecognitionUsageStats OpenAIWhisperPlugin::speechRecognitionUsageStats() const
{
    QSettings s;
    s.beginGroup(SettingsGroup);
    SpeechRecognitionUsageStats stats;
    stats.available   = true;
    stats.audioMsUsed = s.value(QLatin1String("stats/audioMsUsed"), 0).toLongLong();
    stats.bytesSent   = s.value(QLatin1String("stats/bytesSent"), 0).toLongLong();
    stats.humanSummary
        = tr("<b>Used:</b> %1 seconds, %2 bytes sent").arg(stats.audioMsUsed / 1000).arg(stats.bytesSent);
    return stats;
}

SpeechRecognitionJob *OpenAIWhisperPlugin::recognizeSpeech(const SpeechRecognitionAudio   &audio,
                                                           const SpeechRecognitionRequest &request)
{
    qDebug() << "OpenAI Whisper recognizeSpeech requested:"
             << "audioBytes" << audio.data.size() << "durationMs" << audio.durationMs << "format" << audio.format
             << "language" << request.language;
    if (!network)
        network = new QNetworkAccessManager(this);
    return new OpenAIWhisperSpeechJob(this, audio, request);
}

OpenAIWhisperPlugin::Settings OpenAIWhisperPlugin::settings() const
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

void OpenAIWhisperPlugin::saveSettings(const Settings &settings)
{
    QSettings s;
    s.beginGroup(SettingsGroup);
    auto model = settings.model.trimmed();
    if (model.isEmpty()) {
        model = DefaultModel;
    }
    auto prompt = settings.prompt.isEmpty() ? defaultPrompt() : settings.prompt;
    qDebug() << "Saving OpenAI Whisper settings:"
             << "apiKeyPresent" << !settings.apiKey.isEmpty() << "apiKeyLength" << settings.apiKey.size() << "model"
             << model << "promptLength" << prompt.size();
    s.setValue(QLatin1String("apiKey"), settings.apiKey);
    s.setValue(QLatin1String("model"), model);
    s.setValue(QLatin1String("prompt"), prompt);
}

void OpenAIWhisperPlugin::addUsage(qint64 audioMs, qint64 bytesSent)
{
    QSettings s;
    s.beginGroup(SettingsGroup);
    s.setValue(QLatin1String("stats/audioMsUsed"),
               s.value(QLatin1String("stats/audioMsUsed"), 0).toLongLong() + audioMs);
    s.setValue(QLatin1String("stats/bytesSent"), s.value(QLatin1String("stats/bytesSent"), 0).toLongLong() + bytesSent);
}

} // namespace QtNote

#include "openaiwhisperplugin.moc"
