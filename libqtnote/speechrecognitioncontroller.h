#ifndef QTNOTE_SPEECHRECOGNITIONCONTROLLER_H
#define QTNOTE_SPEECHRECOGNITIONCONTROLLER_H

#include "qtnote_export.h"

#include <QObject>
#include <QPointer>
#include <QUuid>

namespace QtNote {

class NoteEditor;
class SpeechAudioRecorder;
class SpeechRecognitionJob;
class SpeechRecognitionProviderInterface;

class QTNOTE_EXPORT SpeechRecognitionController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)

public:
    explicit SpeechRecognitionController(QObject *parent = nullptr);
    ~SpeechRecognitionController() override;

    void setEditor(NoteEditor *editor);
    void setProvider(SpeechRecognitionProviderInterface *provider);

    bool    available() const;
    bool    busy() const { return busy_; }
    bool    recording() const;
    QString statusText() const { return statusText_; }

    Q_INVOKABLE bool start();
    Q_INVOKABLE void finish();
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    void recognizedText(const QString &text);
    void operationFailed(const QString &message);

private:
    QString language() const;
    QString normalizeLanguage(const QString &value) const;
    QString contextId() const;
    void    setBusy(bool busy, const QString &status = {});

    QPointer<NoteEditor>                editor_;
    SpeechRecognitionProviderInterface *provider_ { nullptr };
    SpeechAudioRecorder                *recorder_ { nullptr };
    QPointer<SpeechRecognitionJob>      job_;
    QString                             localContextId_;
    QString                             statusText_;
    bool                                busy_ { false };
};

} // namespace QtNote

#endif // QTNOTE_SPEECHRECOGNITIONCONTROLLER_H
