#include "yandexspeechutils.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace QtNote::YandexSpeech {
namespace {

    QString normalizedMediaType(QString mediaType)
    {
        const int separator = mediaType.indexOf(QLatin1Char(';'));
        if (separator >= 0)
            mediaType.truncate(separator);
        return mediaType.trimmed().toLower();
    }

    QString alternativeText(const QJsonObject &update)
    {
        const auto alternatives = update.value(QLatin1String("alternatives")).toArray();
        if (alternatives.isEmpty())
            return {};
        return alternatives.first().toObject().value(QLatin1String("text")).toString().trimmed();
    }

    QList<QJsonObject> responseObjects(const QByteArray &body)
    {
        QList<QJsonObject> result;
        const auto         whole = QJsonDocument::fromJson(body);
        if (whole.isObject()) {
            result.append(whole.object());
            return result;
        }
        if (whole.isArray()) {
            for (const auto &value : whole.array()) {
                if (value.isObject())
                    result.append(value.toObject());
            }
            return result;
        }

        // SpeechKit returns a stream of JSON objects. Parse complete top-level
        // objects without assuming a particular line separator or chunk boundary.
        int  start    = -1;
        int  depth    = 0;
        bool inString = false;
        bool escaped  = false;
        for (int i = 0; i < body.size(); ++i) {
            const char ch = body.at(i);
            if (inString) {
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    inString = false;
                }
                continue;
            }
            if (ch == '"') {
                inString = true;
            } else if (ch == '{') {
                if (depth++ == 0)
                    start = i;
            } else if (ch == '}' && depth > 0) {
                if (--depth == 0 && start >= 0) {
                    const auto document = QJsonDocument::fromJson(body.mid(start, i - start + 1));
                    if (document.isObject())
                        result.append(document.object());
                    start = -1;
                }
            }
        }
        return result;
    }

} // namespace

ContainerAudioType directContainerAudioType(const QString &mediaType, const QString &fileName, int bitsPerSample)
{
    const QString type   = normalizedMediaType(mediaType);
    const QString suffix = QFileInfo(fileName).suffix().toLower();
    if (type == QLatin1String("audio/mpeg") || type == QLatin1String("audio/mp3") || suffix == QLatin1String("mp3")) {
        return ContainerAudioType::Mp3;
    }
    if (type == QLatin1String("audio/opus") || suffix == QLatin1String("opus"))
        return ContainerAudioType::OggOpus;
    if ((type == QLatin1String("audio/wav") || type == QLatin1String("audio/x-wav") || suffix == QLatin1String("wav"))
        && bitsPerSample == 16) {
        return ContainerAudioType::Wav;
    }
    return ContainerAudioType::None;
}

float normalizedSample(const QAudioBuffer &buffer, qsizetype sampleIndex)
{
    const auto format         = buffer.format();
    const int  bytesPerSample = format.bytesPerSample();
    if (!buffer.isValid() || bytesPerSample <= 0 || sampleIndex < 0
        || sampleIndex >= buffer.byteCount() / bytesPerSample) {
        return 0.0f;
    }
    const char *sample = buffer.constData<char>() + sampleIndex * bytesPerSample;
    switch (format.sampleFormat()) {
    case QAudioFormat::UInt8: {
        quint8 value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return (float(value) - 128.0f) / 128.0f;
    }
    case QAudioFormat::Int16: {
        qint16 value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return float(value) / 32768.0f;
    }
    case QAudioFormat::Int32: {
        qint32 value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return float(double(value) / 2147483648.0);
    }
    case QAudioFormat::Float: {
        float value = 0.0f;
        std::memcpy(&value, sample, sizeof(value));
        return std::clamp(value, -1.0f, 1.0f);
    }
    case QAudioFormat::Unknown:
        break;
    }
    return 0.0f;
}

MonoPcm16Resampler::MonoPcm16Resampler(int targetSampleRate, qsizetype maximumBufferedBytes) :
    targetSampleRate_(targetSampleRate), maximumBufferedBytes_(maximumBufferedBytes)
{
}

void MonoPcm16Resampler::appendSample(float value)
{
    const qint16 encoded = qint16(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    char         bytes[2];
    qToLittleEndian<quint16>(quint16(encoded), bytes);
    pcm_.append(bytes, sizeof(bytes));
}

bool MonoPcm16Resampler::append(const QVector<float> &monoFrames, int sourceSampleRate)
{
    if (error_ != NoError)
        return false;
    if (targetSampleRate_ <= 0 || maximumBufferedBytes_ <= 0 || sourceSampleRate <= 0) {
        error_ = InvalidConfiguration;
        return false;
    }
    if (monoFrames.isEmpty())
        return true;
    if (sourceSampleRate_ == 0)
        sourceSampleRate_ = sourceSampleRate;
    if (sourceSampleRate_ != sourceSampleRate) {
        error_ = SourceSampleRateChanged;
        return false;
    }

    QVector<float> window;
    window.reserve(monoFrames.size() + (hasPreviousSample_ ? 1 : 0));
    const qint64 baseFrame = hasPreviousSample_ ? sourceFramesSeen_ - 1 : sourceFramesSeen_;
    if (hasPreviousSample_)
        window.append(previousSample_);
    window.append(monoFrames);

    const double step      = double(sourceSampleRate_) / double(targetSampleRate_);
    const double lastFrame = double(baseFrame + window.size() - 1);
    while (nextSourcePosition_ <= lastFrame) {
        const double localPosition = nextSourcePosition_ - double(baseFrame);
        if (localPosition < 0.0) {
            nextSourcePosition_ += step;
            continue;
        }
        const qsizetype left  = qsizetype(localPosition);
        const qsizetype right = left + 1;
        if (left >= window.size())
            break;
        if (right >= window.size() && localPosition > double(left))
            break; // Keep the fractional sample until the next input buffer.

        const float fraction   = float(localPosition - double(left));
        const float leftValue  = window.at(left);
        const float rightValue = right < window.size() ? window.at(right) : leftValue;
        if (pcm_.size() > maximumBufferedBytes_ - 2) {
            error_ = OutputTooLarge;
            return false;
        }
        appendSample(leftValue * (1.0f - fraction) + rightValue * fraction);
        nextSourcePosition_ += step;
    }

    sourceFramesSeen_ += monoFrames.size();
    previousSample_    = monoFrames.constLast();
    hasPreviousSample_ = true;
    return true;
}

QByteArray MonoPcm16Resampler::takeAvailable()
{
    QByteArray result;
    result.swap(pcm_);
    return result;
}

void MonoPcm16Resampler::resetStreamState()
{
    hasPreviousSample_  = false;
    sourceFramesSeen_   = 0;
    nextSourcePosition_ = 0.0;
    sourceSampleRate_   = 0;
    error_              = NoError;
}

QByteArray MonoPcm16Resampler::takeResult()
{
    QByteArray result = takeAvailable();
    resetStreamState();
    return result;
}

MonoPcm16Resampler::Error MonoPcm16Resampler::error() const { return error_; }

QString MonoPcm16Resampler::errorString() const
{
    switch (error_) {
    case NoError:
        return {};
    case InvalidConfiguration:
        return QStringLiteral("Invalid resampler configuration");
    case SourceSampleRateChanged:
        return QStringLiteral("The decoded audio changed sample rate unexpectedly");
    case OutputTooLarge:
        return QStringLiteral("Audio is too large for an inline SpeechKit request");
    }
    return {};
}

QString transcriptFromResult(const QByteArray &body, QString *statusError)
{
    if (statusError)
        statusError->clear();

    QMap<qint64, QString> finalTexts;
    QSet<qint64>          refinedIndices;
    qint64                fallbackIndex = 0;
    for (const auto &response : responseObjects(body)) {
        const QJsonObject wrappedResult = response.value(QLatin1String("result")).toObject();
        const QJsonObject object        = wrappedResult.isEmpty() ? response : wrappedResult;
        const auto        status        = object.value(QLatin1String("statusCode")).toObject();
        const QString     code          = status.value(QLatin1String("codeType")).toString();
        if (!code.isEmpty() && code != QLatin1String("WORKING") && code != QLatin1String("CLOSED")) {
            const QString message = status.value(QLatin1String("message")).toString().trimmed();
            if (statusError && !message.isEmpty())
                *statusError = message;
        }

        const auto final = object.value(QLatin1String("final")).toObject();
        if (!final.isEmpty()) {
            bool   ok    = false;
            qint64 index = object.value(QLatin1String("audioCursors"))
                               .toObject()
                               .value(QLatin1String("finalIndex"))
                               .toString()
                               .toLongLong(&ok);
            if (!ok)
                index = ++fallbackIndex;
            const QString text = alternativeText(final);
            if (!text.isEmpty() && !refinedIndices.contains(index))
                finalTexts[index] = text;
        }

        const auto refinement = object.value(QLatin1String("finalRefinement")).toObject();
        if (!refinement.isEmpty()) {
            bool          ok    = false;
            const qint64  index = refinement.value(QLatin1String("finalIndex")).toString().toLongLong(&ok);
            const QString text  = alternativeText(refinement.value(QLatin1String("normalizedText")).toObject());
            if (!text.isEmpty()) {
                if (ok) {
                    finalTexts[index] = text;
                    refinedIndices.insert(index);
                } else {
                    const qint64 fallback = ++fallbackIndex;
                    finalTexts[fallback]  = text;
                    refinedIndices.insert(fallback);
                }
            }
        }
    }

    QStringList parts;
    for (auto it = finalTexts.cbegin(); it != finalTexts.cend(); ++it) {
        if (!it.value().isEmpty())
            parts.append(it.value());
    }
    return parts.join(QLatin1Char(' ')).simplified();
}

qint64 estimatedAsyncBillableAudioMs(qint64 durationMs, int channels)
{
    const qint64 roundedSeconds = qMax<qint64>(15, (qMax<qint64>(0, durationMs) + 999) / 1000);
    const qint64 channelPairs   = qMax<qint64>(1, (qMax(1, channels) + 1) / 2);
    return roundedSeconds * 1000 * channelPairs;
}

} // namespace QtNote::YandexSpeech
