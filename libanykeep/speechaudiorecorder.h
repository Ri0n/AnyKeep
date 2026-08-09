#ifndef SPEECHAUDIORECORDER_H
#define SPEECHAUDIORECORDER_H

#include <QBuffer>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include "anykeep_export.h"
#include "speechrecognitionprovider.h"

#ifdef ANYKEEP_MULTIMEDIA_AVAILABLE
#include <QAudioFormat>
#include <QAudioSource>
#endif

namespace AnyKeep {

class ANYKEEP_EXPORT SpeechAudioRecorder : public QObject {
    Q_OBJECT
public:
    explicit SpeechAudioRecorder(QObject *parent = nullptr);
    ~SpeechAudioRecorder() override;

    bool    isAvailable() const;
    bool    isRecording() const;
    QString errorString() const;
    qint64  elapsedMs() const;

    bool                   start(int maxDurationMs);
    SpeechRecognitionAudio stop();
    void                   cancel();

signals:
    void elapsedChanged(qint64 elapsedMs, qint64 maxDurationMs);
    void maxDurationReached();
    void failed(const QString &error);

private:
    void cleanup();
    void setError(const QString &error);

#ifdef ANYKEEP_MULTIMEDIA_AVAILABLE
    QAudioFormat  format() const;
    QAudioSource *audioInput = nullptr;
#endif

    QBuffer       buffer;
    QElapsedTimer elapsed;
    QTimer        progressTimer;
    int           maxDuration         = 0;
    int           activeSampleRate    = 16000;
    int           activeChannels      = 1;
    int           activeBitsPerSample = 16;
    QString       lastError;
};

} // namespace AnyKeep

#endif // SPEECHAUDIORECORDER_H
