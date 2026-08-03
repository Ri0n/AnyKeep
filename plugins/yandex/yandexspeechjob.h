#ifndef YANDEXSPEECHJOB_H
#define YANDEXSPEECHJOB_H

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioFormat>
#include <QBuffer>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTimer>
#include <QVector>

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAudioBufferInput>
#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QQueue>

#include <memory>
#endif

#include "speechrecognitionprovider.h"
#include "yandexspeechutils.h"

namespace AnyKeep {

class YandexPlugin;

class YandexSpeechJob final : public SpeechRecognitionJob {
    Q_OBJECT

public:
    YandexSpeechJob(YandexPlugin *plugin, const SpeechRecognitionAudio &audio, const SpeechRecognitionRequest &request);
    ~YandexSpeechJob() override;

public slots:
    void cancel() override;

private slots:
    void start();
    void readDecodedBuffer();
    void finishDecoding();
    void decoderFailed(QAudioDecoder::Error error);
    void submitRecognition();
    void pollOperation();
    void fetchRecognition();

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    void drainEncoderQueue();
    void encoderStateChanged(QMediaRecorder::RecorderState state);
    void encoderFailed(QMediaRecorder::Error error, const QString &message);
#endif

private:
    enum class PayloadFormat { None, RawPcm, Wav, OggOpus, Mp3 };

    bool            prepareRawPcm();
    bool            prepareDirectContainer();
    bool            appendDecodedBuffer(const QAudioBuffer &buffer);
    void            startDecoder();
    void            startDecodedUploadPreparation();
    void            finishRawConversion();
    void            schedulePoll();
    void            clearReply();
    void            fail(const QString &message);
    void            cleanupRecognitionResult();
    void            clearAudioPreparation();
    QNetworkRequest authenticatedRequest(const QUrl &url) const;
    QJsonObject     audioFormatJson() const;

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    bool startCompactEncoder(bool decodeSource);
    bool configureCompactEncoder(QMediaFormat format, PayloadFormat payloadFormat, int bitRate, bool decodeSource);
    void queuePcmForEncoder(QByteArray pcm);
    void finishCompactEncoding();
    void clearCompactEncoder();
#endif

    YandexPlugin                    *plugin_;
    SpeechRecognitionAudio           audio_;
    SpeechRecognitionRequest         request_;
    QPointer<QNetworkReply>          reply_;
    QAudioDecoder                    decoder_;
    QBuffer                          sourceBuffer_;
    YandexSpeech::MonoPcm16Resampler resampler_;
    QByteArray                       payload_;
    PayloadFormat                    payloadFormat_ { PayloadFormat::None };
    QString                          operationId_;
    qint64                           submittedBytes_ { 0 };
    bool                             decoderStarted_ { false };
    bool                             cancelled_ { false };
    bool                             completed_ { false };
    int                              pollIntervalMs_ { 1000 };
    int                              pollStepMs_ { 1000 };
    int                              maximumPollIntervalMs_ { 10000 };

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    std::unique_ptr<QMediaCaptureSession> encoderSession_;
    std::unique_ptr<QAudioBufferInput>    encoderInput_;
    std::unique_ptr<QMediaRecorder>       encoderRecorder_;
    QBuffer                               encodedOutput_;
    QQueue<QAudioBuffer>                  encoderQueue_;
    QAudioFormat                          encoderPcmFormat_;
    qint64                                encoderTimestampUs_ { 0 };
    qsizetype                             encoderQueuedBytes_ { 0 };
    bool                                  encoderActive_ { false };
    bool                                  encoderDecodeSource_ { true };
    bool                                  encoderStarted_ { false };
    bool                                  decoderFinished_ { false };
    bool                                  encoderEosSent_ { false };
#endif
};

} // namespace AnyKeep

#endif // YANDEXSPEECHJOB_H
