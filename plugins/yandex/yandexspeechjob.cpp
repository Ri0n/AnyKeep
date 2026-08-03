#include "yandexspeechjob.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

#include "yandexapiutils.h"
#include "yandexplugin.h"
#include "yandexspeechutils.h"

namespace AnyKeep {
namespace {

    constexpr int       TargetSampleRate              = 16000;
    constexpr qint64    MaxAsyncDurationMs            = 4LL * 60 * 60 * 1000;
    constexpr qint64    MaxRawFallbackDurationMs      = 2LL * 60 * 1000;
    constexpr qsizetype MaxInlineAudioBytes           = 42LL * 1024 * 1024;
    constexpr qsizetype MaxBufferedPcmBytes           = 8LL * 1024 * 1024;
    constexpr qsizetype MaxEncoderQueuedPcmBytes      = 1024 * 1024;
    constexpr qsizetype EncoderPcmChunkBytes          = TargetSampleRate * 2 / 5; // 200 ms
    constexpr int       OpusBitRate                   = 24000;
    constexpr int       Mp3BitRate                    = 32000;
    constexpr int       StandardInitialPollIntervalMs = 1000;
    constexpr int       StandardPollStepMs            = 1000;
    constexpr int       StandardMaximumPollIntervalMs = 10000;
    constexpr int       DeferredInitialPollIntervalMs = 5000;
    constexpr int       DeferredPollStepMs            = 5000;
    constexpr int       DeferredMaximumPollIntervalMs = 60000;

    const QUrl RecognizeUrl(QStringLiteral("https://stt.api.cloud.yandex.net/stt/v3/recognizeFileAsync"));
    const QUrl OperationBaseUrl(QStringLiteral("https://operation.api.cloud.yandex.net/operations/"));
    const QUrl ResultUrl(QStringLiteral("https://stt.api.cloud.yandex.net/stt/v3/getRecognition"));
    const QUrl DeleteResultUrl(QStringLiteral("https://stt.api.cloud.yandex.net/stt/v3/deleteRecognition"));

    void appendBase64(QByteArray &target, const QByteArray &source)
    {
        // Keep each non-final chunk aligned to three input bytes so independently
        // encoded chunks concatenate into one valid base64 stream.
        constexpr qsizetype ChunkSize = 48 * 1024;
        for (qsizetype offset = 0; offset < source.size(); offset += ChunkSize) {
            const qsizetype size = qMin(ChunkSize, source.size() - offset);
            target.append(QByteArray::fromRawData(source.constData() + offset, size).toBase64());
        }
    }

} // namespace

YandexSpeechJob::YandexSpeechJob(YandexPlugin *plugin, const SpeechRecognitionAudio &audio,
                                 const SpeechRecognitionRequest &request) :
    SpeechRecognitionJob(plugin), plugin_(plugin), audio_(audio), request_(request), decoder_(this),
    sourceBuffer_(&audio_.data, this), resampler_(TargetSampleRate, MaxBufferedPcmBytes)
{
    connect(&decoder_, &QAudioDecoder::bufferReady, this, &YandexSpeechJob::readDecodedBuffer);
    connect(&decoder_, &QAudioDecoder::finished, this, &YandexSpeechJob::finishDecoding);
    connect(&decoder_, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this, &YandexSpeechJob::decoderFailed);
    connect(&decoder_, &QAudioDecoder::positionChanged, this,
            [this](qint64 position) { emit progressChanged(position, audio_.durationMs); });
    QMetaObject::invokeMethod(this, &YandexSpeechJob::start, Qt::QueuedConnection);
}

YandexSpeechJob::~YandexSpeechJob()
{
    if (!cancelled_ && !completed_)
        cancel();
}

void YandexSpeechJob::cancel()
{
    if (cancelled_ || completed_)
        return;
    cancelled_ = true;
    clearAudioPreparation();
    clearReply();
    if (!operationId_.isEmpty()) {
        QUrl url = OperationBaseUrl;
        url.setPath(url.path() + operationId_ + QStringLiteral(":cancel"));
        auto *cancelReply = plugin_->network_->get(authenticatedRequest(url));
        connect(cancelReply, &QNetworkReply::finished, cancelReply, &QObject::deleteLater);
    }
}

void YandexSpeechJob::start()
{
    if (cancelled_)
        return;
    const auto settings = plugin_->settings();
    if (settings.apiKey.isEmpty()) {
        fail(tr("Yandex API key is empty"));
        return;
    }
    if (audio_.data.isEmpty()) {
        fail(tr("Audio data is empty"));
        return;
    }
    if (audio_.durationMs > MaxAsyncDurationMs) {
        fail(tr("Audio is longer than the four-hour SpeechKit asynchronous recognition limit"));
        return;
    }

    if (audio_.mediaType.isEmpty()) {
        if (!prepareRawPcm())
            return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        if (startCompactEncoder(false))
            return;
#endif
        payload_       = std::move(audio_.data);
        payloadFormat_ = PayloadFormat::RawPcm;
        QMetaObject::invokeMethod(this, &YandexSpeechJob::submitRecognition, Qt::QueuedConnection);
        return;
    }

    if (prepareDirectContainer()) {
        QMetaObject::invokeMethod(this, &YandexSpeechJob::submitRecognition, Qt::QueuedConnection);
        return;
    }
    if (cancelled_ || completed_)
        return;

    startDecodedUploadPreparation();
}

bool YandexSpeechJob::prepareRawPcm()
{
    const bool isAnyKeepPcm = audio_.format.compare(QLatin1String("pcm_s16le"), Qt::CaseInsensitive) == 0
        || audio_.format.compare(QLatin1String("audio/l16"), Qt::CaseInsensitive) == 0;
    if (!isAnyKeepPcm || audio_.bitsPerSample != 16 || audio_.channels != 1 || audio_.sampleRate != TargetSampleRate) {
        fail(tr("Yandex live transcription requires mono PCM S16LE at 16 kHz"));
        return false;
    }
    if (audio_.data.size() % 2 != 0) {
        fail(tr("Live Yandex transcription received an incomplete PCM sample"));
        return false;
    }
    const qint64 durationFromBytes = audio_.data.size() * 1000LL / (TargetSampleRate * 2LL);
    if (qMax(audio_.durationMs, durationFromBytes) > MaxRawFallbackDurationMs) {
        fail(tr("Live Yandex transcription is limited to two minutes"));
        return false;
    }
    if (audio_.durationMs <= 0)
        audio_.durationMs = durationFromBytes;
    return true;
}

bool YandexSpeechJob::prepareDirectContainer()
{
    switch (YandexSpeech::directContainerAudioType(audio_.mediaType, audio_.fileName, audio_.bitsPerSample)) {
    case YandexSpeech::ContainerAudioType::Mp3:
        payloadFormat_ = PayloadFormat::Mp3;
        break;
    case YandexSpeech::ContainerAudioType::OggOpus:
        payloadFormat_ = PayloadFormat::OggOpus;
        break;
    case YandexSpeech::ContainerAudioType::Wav:
        payloadFormat_ = PayloadFormat::Wav;
        break;
    case YandexSpeech::ContainerAudioType::None:
        return false;
    }

    if (audio_.data.size() > MaxInlineAudioBytes) {
        fail(tr("Audio is too large for an inline SpeechKit request"));
        return false;
    }
    payload_ = std::move(audio_.data);
    return true;
}

void YandexSpeechJob::startDecodedUploadPreparation()
{
    if (cancelled_ || completed_)
        return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (startCompactEncoder(true))
        return;
#endif

    if (audio_.durationMs > MaxRawFallbackDurationMs) {
        fail(tr("This Qt Multimedia backend cannot encode Ogg Opus or MP3. Refusing to upload a large "
                "uncompressed audio stream."));
        return;
    }
    startDecoder();
}

void YandexSpeechJob::startDecoder()
{
    if (decoderStarted_ || cancelled_ || completed_)
        return;
    if (!decoder_.isSupported()) {
        fail(tr("Audio decoding is not supported by this Qt Multimedia backend"));
        return;
    }
    if (!sourceBuffer_.open(QIODevice::ReadOnly)) {
        fail(tr("Could not open the recorded audio for decoding"));
        return;
    }
    decoder_.setSourceDevice(&sourceBuffer_);
    decoderStarted_ = true;
    decoder_.start();
}

void YandexSpeechJob::readDecodedBuffer()
{
    if (cancelled_ || completed_)
        return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (encoderActive_ && encoderQueuedBytes_ >= MaxEncoderQueuedPcmBytes)
        return;
#endif
    const auto buffer = decoder_.read();
    if (!buffer.isValid())
        return;
    if (!appendDecodedBuffer(buffer))
        decoder_.stop();
}

bool YandexSpeechJob::appendDecodedBuffer(const QAudioBuffer &buffer)
{
    const auto format = buffer.format();
    if (format.sampleRate() <= 0 || format.channelCount() <= 0) {
        fail(tr("Decoded audio has an invalid format"));
        return false;
    }
    if (format.sampleFormat() == QAudioFormat::Unknown) {
        fail(tr("The decoded audio sample format is unsupported"));
        return false;
    }

    const qsizetype channels = format.channelCount();
    const qsizetype frames   = buffer.frameCount();
    if (frames <= 0)
        return true;

    QVector<float> mono;
    mono.reserve(frames);
    for (qsizetype frame = 0; frame < frames; ++frame) {
        float           value       = 0.0f;
        const qsizetype firstSample = frame * channels;
        for (qsizetype channel = 0; channel < channels; ++channel)
            value += YandexSpeech::normalizedSample(buffer, firstSample + channel);
        mono.append(std::clamp(value / float(channels), -1.0f, 1.0f));
    }
    if (!resampler_.append(mono, format.sampleRate())) {
        switch (resampler_.error()) {
        case YandexSpeech::MonoPcm16Resampler::SourceSampleRateChanged:
            fail(tr("The decoded audio changed sample rate unexpectedly"));
            break;
        case YandexSpeech::MonoPcm16Resampler::OutputTooLarge:
            fail(tr("Audio conversion used too much memory"));
            break;
        default:
            fail(tr("The audio could not be converted to SpeechKit input format"));
            break;
        }
        return false;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (encoderActive_)
        queuePcmForEncoder(resampler_.takeAvailable());
#endif
    return true;
}

void YandexSpeechJob::finishDecoding()
{
    if (cancelled_ || completed_)
        return;
    decoderStarted_ = false;
    sourceBuffer_.close();
    audio_.data.clear();

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (encoderActive_) {
        queuePcmForEncoder(resampler_.takeResult());
        decoderFinished_ = true;
        drainEncoderQueue();
        return;
    }
#endif

    finishRawConversion();
}

void YandexSpeechJob::finishRawConversion()
{
    payload_       = resampler_.takeResult();
    payloadFormat_ = PayloadFormat::RawPcm;
    if (payload_.isEmpty()) {
        fail(tr("The audio could not be converted to SpeechKit PCM format"));
        return;
    }
    if (payload_.size() > MaxInlineAudioBytes) {
        fail(tr("Audio is too large for an inline SpeechKit request"));
        return;
    }
    submitRecognition();
}

void YandexSpeechJob::decoderFailed(QAudioDecoder::Error error)
{
    if (cancelled_ || completed_ || error == QAudioDecoder::NoError)
        return;
    fail(decoder_.errorString().isEmpty() ? tr("Could not decode the recorded audio") : decoder_.errorString());
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
bool YandexSpeechJob::startCompactEncoder(bool decodeSource)
{
    QMediaFormat opus(QMediaFormat::Ogg);
    opus.setAudioCodec(QMediaFormat::AudioCodec::Opus);
    if (opus.isSupported(QMediaFormat::Encode)
        && configureCompactEncoder(opus, PayloadFormat::OggOpus, OpusBitRate, decodeSource)) {
        return true;
    }

    QMediaFormat mp3(QMediaFormat::MP3);
    mp3.setAudioCodec(QMediaFormat::AudioCodec::MP3);
    return mp3.isSupported(QMediaFormat::Encode)
        && configureCompactEncoder(mp3, PayloadFormat::Mp3, Mp3BitRate, decodeSource);
}

bool YandexSpeechJob::configureCompactEncoder(QMediaFormat format, PayloadFormat payloadFormat, int bitRate,
                                              bool decodeSource)
{
    clearCompactEncoder();
    encoderDecodeSource_ = decodeSource;

    encoderPcmFormat_.setSampleRate(TargetSampleRate);
    encoderPcmFormat_.setChannelCount(1);
    encoderPcmFormat_.setSampleFormat(QAudioFormat::Int16);

    encodedOutput_.setData(QByteArray());
    if (!encodedOutput_.open(QIODevice::ReadWrite))
        return false;

    encoderSession_  = std::make_unique<QMediaCaptureSession>();
    encoderInput_    = std::make_unique<QAudioBufferInput>(encoderPcmFormat_);
    encoderRecorder_ = std::make_unique<QMediaRecorder>();
    encoderSession_->setAudioBufferInput(encoderInput_.get());
    encoderSession_->setRecorder(encoderRecorder_.get());
    if (!encoderRecorder_->isAvailable()) {
        clearCompactEncoder();
        return false;
    }

    encoderRecorder_->setMediaFormat(format);
    encoderRecorder_->setEncodingMode(QMediaRecorder::AverageBitRateEncoding);
    encoderRecorder_->setAudioBitRate(bitRate);
    encoderRecorder_->setAudioSampleRate(TargetSampleRate);
    encoderRecorder_->setAudioChannelCount(1);
    encoderRecorder_->setAutoStop(true);
    encoderRecorder_->setOutputDevice(&encodedOutput_);

    connect(encoderInput_.get(), &QAudioBufferInput::readyToSendAudioBuffer, this, &YandexSpeechJob::drainEncoderQueue);
    connect(encoderRecorder_.get(), &QMediaRecorder::recorderStateChanged, this, &YandexSpeechJob::encoderStateChanged);
    connect(encoderRecorder_.get(), &QMediaRecorder::errorOccurred, this, &YandexSpeechJob::encoderFailed);

    payloadFormat_ = payloadFormat;
    encoderActive_ = true;
    encoderRecorder_->record();
    return true;
}

void YandexSpeechJob::queuePcmForEncoder(QByteArray pcm)
{
    if (pcm.isEmpty() || !encoderActive_)
        return;

    for (qsizetype offset = 0; offset < pcm.size(); offset += EncoderPcmChunkBytes) {
        const qsizetype size = qMin(EncoderPcmChunkBytes, pcm.size() - offset);
        QAudioBuffer    buffer(pcm.mid(offset, size), encoderPcmFormat_, encoderTimestampUs_);
        if (!buffer.isValid()) {
            fail(tr("Could not prepare decoded audio for compact encoding"));
            return;
        }
        encoderTimestampUs_ += buffer.duration();
        if (encoderTimestampUs_ / 1000 > MaxAsyncDurationMs) {
            fail(tr("Audio is longer than the four-hour SpeechKit asynchronous recognition limit"));
            return;
        }
        encoderQueuedBytes_ += buffer.byteCount();
        encoderQueue_.enqueue(buffer);
    }
    drainEncoderQueue();
}

void YandexSpeechJob::drainEncoderQueue()
{
    if (!encoderActive_ || !encoderInput_ || cancelled_ || completed_)
        return;
    while (!encoderQueue_.isEmpty()) {
        if (!encoderInput_->sendAudioBuffer(encoderQueue_.head()))
            return;
        encoderQueuedBytes_ -= encoderQueue_.head().byteCount();
        encoderQueue_.dequeue();
    }
    if (!decoderFinished_ && decoder_.bufferAvailable() && encoderQueuedBytes_ < MaxEncoderQueuedPcmBytes) {
        QMetaObject::invokeMethod(this, &YandexSpeechJob::readDecodedBuffer, Qt::QueuedConnection);
    }
    if (decoderFinished_ && !encoderEosSent_) {
        if (!encoderInput_->sendAudioBuffer(QAudioBuffer()))
            return;
        encoderEosSent_ = true;
    }
}

void YandexSpeechJob::encoderStateChanged(QMediaRecorder::RecorderState state)
{
    if (cancelled_ || completed_)
        return;
    if (state == QMediaRecorder::RecordingState && !encoderStarted_) {
        const QMediaFormat actual = encoderRecorder_->mediaFormat();
        const bool         expectedFormat
            = (payloadFormat_ == PayloadFormat::OggOpus && actual.fileFormat() == QMediaFormat::Ogg
               && actual.audioCodec() == QMediaFormat::AudioCodec::Opus)
            || (payloadFormat_ == PayloadFormat::Mp3 && actual.fileFormat() == QMediaFormat::MP3
                && actual.audioCodec() == QMediaFormat::AudioCodec::MP3);
        if (!expectedFormat) {
            const QString failure = tr("Qt Multimedia selected an audio format that SpeechKit does not accept");
            QMetaObject::invokeMethod(this, [this, failure] { fail(failure); }, Qt::QueuedConnection);
            return;
        }
        encoderStarted_ = true;
        // QMediaRecorder emits RecordingState synchronously from record().
        // Defer input so autoStop cannot finalize the encoder reentrantly.
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (cancelled_ || completed_ || !encoderActive_ || !encoderStarted_)
                    return;
                if (encoderDecodeSource_) {
                    startDecoder();
                } else {
                    queuePcmForEncoder(std::move(audio_.data));
                    decoderFinished_ = true;
                    drainEncoderQueue();
                }
            },
            Qt::QueuedConnection);
        return;
    }
    if (state == QMediaRecorder::StoppedState && encoderStarted_ && encoderEosSent_)
        QMetaObject::invokeMethod(this, &YandexSpeechJob::finishCompactEncoding, Qt::QueuedConnection);
}

void YandexSpeechJob::encoderFailed(QMediaRecorder::Error error, const QString &message)
{
    if (cancelled_ || completed_ || error == QMediaRecorder::NoError)
        return;
    const QString failure = message.isEmpty() ? tr("Could not encode compact audio for SpeechKit") : message;
    QMetaObject::invokeMethod(this, [this, failure] { fail(failure); }, Qt::QueuedConnection);
}

void YandexSpeechJob::finishCompactEncoding()
{
    if (cancelled_ || completed_ || !encoderActive_)
        return;
    payload_ = encodedOutput_.data();
    clearCompactEncoder();
    if (payload_.isEmpty()) {
        fail(tr("Compact audio encoding produced no data"));
        return;
    }
    if (payload_.size() > MaxInlineAudioBytes) {
        fail(tr("Audio is too large for an inline SpeechKit request"));
        return;
    }
    submitRecognition();
}

void YandexSpeechJob::clearCompactEncoder()
{
    encoderActive_       = false;
    encoderDecodeSource_ = true;
    if (encoderRecorder_ && encoderRecorder_->recorderState() != QMediaRecorder::StoppedState)
        encoderRecorder_->stop();
    if (encoderSession_) {
        encoderSession_->setAudioBufferInput(nullptr);
        encoderSession_->setRecorder(nullptr);
    }
    encoderQueue_.clear();
    encoderQueuedBytes_ = 0;
    encoderRecorder_.reset();
    encoderInput_.reset();
    encoderSession_.reset();
    encodedOutput_.close();
    encodedOutput_.setData(QByteArray());
    encoderTimestampUs_ = 0;
    encoderStarted_     = false;
    decoderFinished_    = false;
    encoderEosSent_     = false;
}
#endif

QNetworkRequest YandexSpeechJob::authenticatedRequest(const QUrl &url) const
{
    return YandexApi::authenticatedRequest(url, plugin_->settings().apiKey);
}

QJsonObject YandexSpeechJob::audioFormatJson() const
{
    QJsonObject audioFormat;
    if (payloadFormat_ == PayloadFormat::RawPcm) {
        QJsonObject rawAudio;
        rawAudio.insert(QLatin1String("audioEncoding"), QLatin1String("LINEAR16_PCM"));
        rawAudio.insert(QLatin1String("sampleRateHertz"), QString::number(TargetSampleRate));
        rawAudio.insert(QLatin1String("audioChannelCount"), QStringLiteral("1"));
        audioFormat.insert(QLatin1String("rawAudio"), rawAudio);
        return audioFormat;
    }

    QString containerType;
    switch (payloadFormat_) {
    case PayloadFormat::Wav:
        containerType = QStringLiteral("WAV");
        break;
    case PayloadFormat::OggOpus:
        containerType = QStringLiteral("OGG_OPUS");
        break;
    case PayloadFormat::Mp3:
        containerType = QStringLiteral("MP3");
        break;
    case PayloadFormat::None:
    case PayloadFormat::RawPcm:
        break;
    }
    if (!containerType.isEmpty()) {
        QJsonObject containerAudio;
        containerAudio.insert(QLatin1String("containerAudioType"), containerType);
        audioFormat.insert(QLatin1String("containerAudio"), containerAudio);
    }
    return audioFormat;
}

void YandexSpeechJob::submitRecognition()
{
    if (cancelled_ || completed_)
        return;
    if (payload_.isEmpty() || payloadFormat_ == PayloadFormat::None) {
        fail(tr("No prepared audio is available for SpeechKit"));
        return;
    }
    if (payload_.size() > MaxInlineAudioBytes) {
        fail(tr("Audio is too large for an inline SpeechKit request"));
        return;
    }

    const auto settings = plugin_->settings();
    if (settings.deferredRecognition) {
        pollIntervalMs_        = DeferredInitialPollIntervalMs;
        pollStepMs_            = DeferredPollStepMs;
        maximumPollIntervalMs_ = DeferredMaximumPollIntervalMs;
    } else {
        pollIntervalMs_        = StandardInitialPollIntervalMs;
        pollStepMs_            = StandardPollStepMs;
        maximumPollIntervalMs_ = StandardMaximumPollIntervalMs;
    }
    QJsonObject normalization;
    normalization.insert(QLatin1String("textNormalization"),
                         settings.normalizeText ? QLatin1String("TEXT_NORMALIZATION_ENABLED")
                                                : QLatin1String("TEXT_NORMALIZATION_DISABLED"));
    normalization.insert(QLatin1String("profanityFilter"), false);
    normalization.insert(QLatin1String("literatureText"), settings.literatureText);
    normalization.insert(QLatin1String("phoneFormattingMode"), QLatin1String("PHONE_FORMATTING_MODE_DISABLED"));

    QJsonObject model;
    model.insert(QLatin1String("model"),
                 settings.deferredRecognition ? QLatin1String("deferred-general") : QLatin1String("general"));
    model.insert(QLatin1String("audioFormat"), audioFormatJson());
    model.insert(QLatin1String("textNormalization"), normalization);
    if (!request_.language.isEmpty()) {
        QJsonObject restriction;
        restriction.insert(QLatin1String("restrictionType"), QLatin1String("WHITELIST"));
        QJsonArray languages;
        languages.append(request_.language);
        restriction.insert(QLatin1String("languageCode"), languages);
        model.insert(QLatin1String("languageRestriction"), restriction);
    }

    const QByteArray modelJson   = QJsonDocument(model).toJson(QJsonDocument::Compact);
    const qsizetype  encodedSize = ((payload_.size() + 2) / 3) * 4;
    QByteArray       body;
    body.reserve(encodedSize + modelJson.size() + 48);
    body.append("{\"content\":\"");
    appendBase64(body, payload_);
    body.append("\",\"recognitionModel\":");
    body.append(modelJson);
    body.append('}');
    submittedBytes_ = body.size();

    auto request = authenticatedRequest(RecognizeUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QLatin1String("application/json"));
    reply_ = plugin_->network_->post(request, body);
    payload_.clear();
    connect(reply_, &QNetworkReply::finished, this, [this] {
        if (!reply_ || cancelled_ || completed_)
            return;
        auto *reply            = reply_.data();
        reply_                 = nullptr;
        const QByteArray body  = reply->readAll();
        const bool       ok    = reply->error() == QNetworkReply::NoError;
        const QString    error = ok ? QString() : YandexApi::errorMessage(reply, body);
        reply->deleteLater();
        if (!ok) {
            fail(error);
            return;
        }
        const auto operation      = QJsonDocument::fromJson(body).object();
        operationId_              = operation.value(QLatin1String("id")).toString().trimmed();
        const auto operationError = operation.value(QLatin1String("error")).toObject();
        if (!operationError.isEmpty()) {
            fail(operationError.value(QLatin1String("message")).toString());
            return;
        }
        if (operationId_.isEmpty()) {
            fail(tr("SpeechKit did not return a recognition operation ID"));
            return;
        }
        // SpeechKit has accepted the audio at this point, so the request can be
        // billable even if AnyKeep is closed before the transcript is fetched.
        plugin_->addUsage(audio_.durationMs, 1, submittedBytes_);
        emit progressChanged(0, audio_.durationMs);
        if (operation.value(QLatin1String("done")).toBool())
            fetchRecognition();
        else
            schedulePoll();
    });
}

void YandexSpeechJob::schedulePoll()
{
    if (!cancelled_ && !completed_)
        QTimer::singleShot(pollIntervalMs_, this, &YandexSpeechJob::pollOperation);
}

void YandexSpeechJob::pollOperation()
{
    if (cancelled_ || completed_ || operationId_.isEmpty())
        return;
    QUrl url = OperationBaseUrl;
    url.setPath(url.path() + operationId_);
    reply_ = plugin_->network_->get(authenticatedRequest(url));
    connect(reply_, &QNetworkReply::finished, this, [this] {
        if (!reply_ || cancelled_ || completed_)
            return;
        auto *reply            = reply_.data();
        reply_                 = nullptr;
        const QByteArray body  = reply->readAll();
        const bool       ok    = reply->error() == QNetworkReply::NoError;
        const QString    error = ok ? QString() : YandexApi::errorMessage(reply, body);
        reply->deleteLater();
        if (!ok) {
            fail(error);
            return;
        }
        const auto operation      = QJsonDocument::fromJson(body).object();
        const auto operationError = operation.value(QLatin1String("error")).toObject();
        if (!operationError.isEmpty()) {
            fail(operationError.value(QLatin1String("message")).toString());
            return;
        }
        if (!operation.value(QLatin1String("done")).toBool()) {
            pollIntervalMs_ = qMin(maximumPollIntervalMs_, pollIntervalMs_ + pollStepMs_);
            schedulePoll();
            return;
        }
        emit progressChanged(audio_.durationMs, audio_.durationMs);
        fetchRecognition();
    });
}

void YandexSpeechJob::fetchRecognition()
{
    if (cancelled_ || completed_ || operationId_.isEmpty())
        return;
    QUrl      url = ResultUrl;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("operationId"), operationId_);
    url.setQuery(query);
    reply_ = plugin_->network_->get(authenticatedRequest(url));
    connect(reply_, &QNetworkReply::finished, this, [this] {
        if (!reply_ || cancelled_ || completed_)
            return;
        auto *reply            = reply_.data();
        reply_                 = nullptr;
        const QByteArray body  = reply->readAll();
        const bool       ok    = reply->error() == QNetworkReply::NoError;
        const QString    error = ok ? QString() : YandexApi::errorMessage(reply, body);
        reply->deleteLater();
        if (!ok) {
            fail(error);
            return;
        }

        QString       statusError;
        const QString transcript = YandexSpeech::transcriptFromResult(body, &statusError);
        if (transcript.isEmpty()) {
            fail(statusError.isEmpty() ? tr("SpeechKit returned no recognized text") : statusError);
            return;
        }

        completed_ = true;
        cleanupRecognitionResult();
        emit finished(transcript);
    });
}

void YandexSpeechJob::cleanupRecognitionResult()
{
    if (operationId_.isEmpty())
        return;
    QUrl      url = DeleteResultUrl;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("operationId"), operationId_);
    url.setQuery(query);
    auto *cleanupReply = plugin_->network_->deleteResource(authenticatedRequest(url));
    connect(cleanupReply, &QNetworkReply::finished, cleanupReply, &QObject::deleteLater);
}

void YandexSpeechJob::clearAudioPreparation()
{
    decoder_.stop();
    decoderStarted_ = false;
    sourceBuffer_.close();
    audio_.data.clear();
    payload_.clear();
    resampler_.takeResult();
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    clearCompactEncoder();
#endif
}

void YandexSpeechJob::clearReply()
{
    if (!reply_)
        return;
    auto *reply = reply_.data();
    reply_      = nullptr;
    reply->abort();
    reply->deleteLater();
}

void YandexSpeechJob::fail(const QString &message)
{
    if (cancelled_ || completed_)
        return;
    completed_ = true;
    clearAudioPreparation();
    clearReply();
    const QString text = message.trimmed();
    qWarning() << "Yandex SpeechKit recognition failed:" << text;
    emit failed(text.isEmpty() ? tr("Yandex SpeechKit recognition failed") : text);
}

} // namespace AnyKeep
