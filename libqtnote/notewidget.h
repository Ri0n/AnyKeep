#ifndef NOTEWIDGET_H
#define NOTEWIDGET_H

#include <QElapsedTimer>
#include <QIcon>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#include "highlighterext.h"
#include "note.h"

namespace Ui {
class NoteWidget;
}

class QAction;
class QToolButton;

namespace QtNote {

class NoteEditor;
class DesktopNoteEditorHost;
class SpeechAudioRecorder;
class SpeechRecognitionJob;
class SpeechRecognitionProviderInterface;

class QTNOTE_EXPORT NoteWidget : public QWidget {
    Q_OBJECT

public:
    enum Feature { RichText = 1 };
    Q_DECLARE_FLAGS(Features, Feature)

    explicit NoteWidget(const Note &note = {}, const QUuid &draftId = {});
    ~NoteWidget();

    void            setText(QString text);
    QString         text();
    const Features &features() const { return _features; }
    bool            isMarkdown() const;
    void            setFeatures(const Features &features) { _features = features; }
    void            setAcceptRichText(bool state);
    void            setSpeechRecognitionProvider(SpeechRecognitionProviderInterface *provider);
    Note            note() const;
    QString         storageId() const;
    QString         noteId() const;
    QUuid           draftId() const;
    const QString  &firstLine() const { return _firstLine; }
    qint64          lastChangeElapsed() const { return _lastChangeElapsed.elapsed(); }
    bool            isTrashRequested() const { return _trashRequested; }
    bool            hasPersistedDraft() const;
    void            setTrashRequested(bool state) { _trashRequested = state; }
    void            setStickyNotesAvailable(bool available);
    void            setAlwaysOnTop(bool enabled);
    bool            alwaysOnTop() const;
    void            rehighlight();
    void            addHighlightExtension(const std::shared_ptr<HighlighterExtension> &extension, int type);
    bool            prepareToClose();
    void            discardDraft();

signals:
    void firstLineChanged();
    void trashRequested();
    void pinRequested();
    void alwaysOnTopChanged(bool enabled);

protected:
    void changeEvent(QEvent *event) override;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool event(QEvent *event) override;
#endif

private:
    enum class ContentLoadPolicy { ResetHistory, RecordFormatConversion };

    void     initFromNote();
    void     setContents(const QString &title, const QString &body, Note::Format format,
                         ContentLoadPolicy policy = ContentLoadPolicy::ResetHistory);
    QAction *initAction(const char *icon, const QString &title, const QString &toolTip, const char *hotkey);

public slots:
    void save();
    void rereadSettings();

private slots:
    void autosave();
    void onCopyClicked();
    void textChanged();
    void onPrintClicked();
    void onSaveClicked();
    void onTrashClicked();
    void updateFirstLineColor();
    void focusReceived();
    void startSpeechRecognition();
    void finishSpeechRecognition();
    void cancelSpeechRecognition();
    void updateSpeechRecognitionProgress(qint64 elapsedMs, qint64 maxDurationMs);

private:
    void    updateSpeechRecognitionAction();
    void    showSpeechRecognitionError(const QString &error);
    void    appendRecognizedText(const QString &text);
    QString speechRecognitionLanguage() const;
    QString normalizeSpeechRecognitionLanguage(const QString &language) const;
    QString speechContextId() const;

    Ui::NoteWidget        *ui        = nullptr;
    NoteEditor            *editor    = nullptr;
    DesktopNoteEditorHost *qmlEditor = nullptr;

    QAction                            *speechAction      = nullptr;
    QAction                            *stickyPinAction   = nullptr;
    QAction                            *alwaysOnTopAction = nullptr;
    QIcon                               speechIdleIcon;
    QToolButton                        *speechButton   = nullptr;
    SpeechRecognitionProviderInterface *speechProvider = nullptr;
    SpeechAudioRecorder                *speechRecorder = nullptr;
    QPointer<SpeechRecognitionJob>      speechJob;
    QString                             localSpeechContextId;
    QString                             _firstLine;
    QString                             _extFileName;
    QString                             _extSelecteFilter;
    QTimer                              _autosaveTimer;
    QElapsedTimer                       _lastChangeElapsed;
    Features                            _features;
    bool                                _trashRequested       = false;
    bool                                speechRecognitionBusy = false;
};

} // namespace QtNote

Q_DECLARE_OPERATORS_FOR_FLAGS(QtNote::NoteWidget::Features)

#endif // NOTEWIDGET_H
