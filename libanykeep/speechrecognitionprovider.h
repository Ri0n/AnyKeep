#ifndef SPEECHRECOGNITIONPROVIDER_H
#define SPEECHRECOGNITIONPROVIDER_H

#include <QByteArray>
#include <QDateTime>
#include <QLocale>
#include <QObject>
#include <QString>
#include <QStringList>

#include "anykeep_export.h"

namespace AnyKeep {

struct SpeechRecognitionCapabilities {
    bool        supportsOneShot        = false;
    bool        supportsStreaming      = false;
    bool        returnsPartialResults  = false;
    bool        supportsPunctuation    = false;
    int         maxOneShotDurationMs   = 0;
    int         maxStreamingDurationMs = 0;
    QStringList languages;
    QStringList encodedAudioMediaTypes;
    QString     preferredLanguage;
};

struct SpeechRecognitionUsageStats {
    bool      available        = false;
    qint64    audioMsUsed      = 0;
    qint64    audioMsLimit     = -1;
    qint64    audioMsRemaining = -1;
    qint64    bytesSent        = 0;
    QDateTime periodStarted;
    QDateTime periodEnds;
    QString   humanSummary;
};

struct SpeechRecognitionAudio {
    QByteArray data;
    QString    format;
    QString    mediaType;
    QString    fileName;
    int        sampleRate    = 0;
    int        channels      = 0;
    int        bitsPerSample = 0;
    qint64     durationMs    = 0;
};

struct SpeechRecognitionRequest {
    QString contextId;
    QLocale locale;
    QString language;
};

class ANYKEEP_EXPORT SpeechRecognitionJob : public QObject {
    Q_OBJECT
public:
    explicit SpeechRecognitionJob(QObject *parent = nullptr) : QObject(parent) {}
    ~SpeechRecognitionJob() override = default;

public slots:
    virtual void cancel() = 0;

signals:
    void partialTextAvailable(const QString &text);
    void progressChanged(qint64 processedMs, qint64 totalMs);
    void finished(const QString &text);
    void failed(const QString &error);
};

class SpeechRecognitionProviderInterface {
public:
    virtual ~SpeechRecognitionProviderInterface() = default;

    virtual bool                          isSpeechRecognitionReady() const                         = 0;
    virtual SpeechRecognitionCapabilities speechRecognitionCapabilities() const                    = 0;
    virtual SpeechRecognitionUsageStats   speechRecognitionUsageStats() const                      = 0;
    virtual SpeechRecognitionJob         *recognizeSpeech(const SpeechRecognitionAudio   &audio,
                                                          const SpeechRecognitionRequest &request) = 0;
};

} // namespace AnyKeep

Q_DECLARE_INTERFACE(AnyKeep::SpeechRecognitionProviderInterface,
                    "com.rion-soft.AnyKeep.SpeechRecognitionProviderInterface/2.0")

#endif // SPEECHRECOGNITIONPROVIDER_H
