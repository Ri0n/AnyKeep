#ifndef ANYKEEP_SPEECHRECOGNITIONCONTROLLER_H
#define ANYKEEP_SPEECHRECOGNITIONCONTROLLER_H

#include "anykeep_export.h"

#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QUuid>

namespace AnyKeep {

class AudioAttachmentRecorder;
class NoteEditor;
class SpeechAudioRecorder;
class SpeechRecognitionJob;
class SpeechRecognitionProviderInterface;

class ANYKEEP_EXPORT SpeechRecognitionController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool speechAvailable READ speechAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool audioRecordingAvailable READ audioRecordingAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool audioTranscriptionAvailable READ audioTranscriptionAvailable NOTIFY stateChanged)
    Q_PROPERTY(int transcribingAudioRow READ transcribingAudioRow NOTIFY stateChanged)
    Q_PROPERTY(bool modeSwitchVisible READ modeSwitchVisible NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(InputMode mode READ mode WRITE setMode NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)

public:
    enum InputMode {
        SpeechToText = 0,
        AudioRecording,
    };
    Q_ENUM(InputMode)

    explicit SpeechRecognitionController(QObject *parent = nullptr);
    ~SpeechRecognitionController() override;

    void setEditor(NoteEditor *editor);
    void setProvider(SpeechRecognitionProviderInterface *provider);

    bool      available() const;
    bool      speechAvailable() const;
    bool      audioRecordingAvailable() const;
    bool      modeSwitchVisible() const;
    bool      audioTranscriptionAvailable() const;
    int       transcribingAudioRow() const { return transcriptionIndex_.isValid() ? transcriptionIndex_.row() : -1; }
    bool      busy() const { return busy_; }
    bool      recording() const;
    InputMode mode() const { return effectiveMode(); }
    QString   statusText() const { return statusText_; }

    Q_INVOKABLE void setMode(InputMode mode);
    Q_INVOKABLE bool start(int insertionRow = -1);
    Q_INVOKABLE void finish();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool transcribeAudio(int row, const QString &sourceUri, qint64 durationMs = 0);

signals:
    void stateChanged();
    void recognizedText(const QString &text);
    void operationFailed(const QString &message);

private:
    InputMode effectiveMode() const;
    QString   language() const;
    QString   normalizeLanguage(const QString &value) const;
    QString   contextId() const;
    void      setBusy(bool busy, const QString &status = {});

    QPointer<NoteEditor>                editor_;
    QPointer<NoteEditor>                audioEditor_;
    QPointer<NoteEditor>                transcriptionEditor_;
    SpeechRecognitionProviderInterface *provider_ { nullptr };
    SpeechAudioRecorder                *speechRecorder_ { nullptr };
    AudioAttachmentRecorder            *audioRecorder_ { nullptr };
    QPointer<SpeechRecognitionJob>      job_;
    QString                             localContextId_;
    QString                             statusText_;
    int                                 pendingAudioRow_ { -1 };
    QPersistentModelIndex               transcriptionIndex_;
    InputMode                           mode_ { SpeechToText };
    bool                                busy_ { false };
};

} // namespace AnyKeep

#endif // ANYKEEP_SPEECHRECOGNITIONCONTROLLER_H
