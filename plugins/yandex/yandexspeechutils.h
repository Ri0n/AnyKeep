#ifndef YANDEXSPEECHUTILS_H
#define YANDEXSPEECHUTILS_H

#include <QAudioBuffer>
#include <QByteArray>
#include <QString>
#include <QVector>

namespace QtNote::YandexSpeech {

enum class ContainerAudioType { None, Wav, OggOpus, Mp3 };

ContainerAudioType directContainerAudioType(const QString &mediaType, const QString &fileName, int bitsPerSample);

float normalizedSample(const QAudioBuffer &buffer, qsizetype sampleIndex);

class MonoPcm16Resampler {
public:
    enum Error { NoError, InvalidConfiguration, SourceSampleRateChanged, OutputTooLarge };

    MonoPcm16Resampler(int targetSampleRate, qsizetype maximumBufferedBytes);

    bool       append(const QVector<float> &monoFrames, int sourceSampleRate);
    QByteArray takeAvailable();
    QByteArray takeResult();
    Error      error() const;
    QString    errorString() const;

private:
    void appendSample(float value);
    void resetStreamState();

    int        targetSampleRate_ { 0 };
    qsizetype  maximumBufferedBytes_ { 0 };
    int        sourceSampleRate_ { 0 };
    qint64     sourceFramesSeen_ { 0 };
    double     nextSourcePosition_ { 0.0 };
    float      previousSample_ { 0.0f };
    bool       hasPreviousSample_ { false };
    QByteArray pcm_;
    Error      error_ { NoError };
};

QString transcriptFromResult(const QByteArray &body, QString *statusError = nullptr);
qint64  estimatedAsyncBillableAudioMs(qint64 durationMs, int channels = 1);

} // namespace QtNote::YandexSpeech

#endif // YANDEXSPEECHUTILS_H
