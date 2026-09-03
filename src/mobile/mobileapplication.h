#ifndef ANYKEEP_MOBILEAPPLICATION_H
#define ANYKEEP_MOBILEAPPLICATION_H

#include "bundledpluginregistry.h"
#include "note.h"
#include "pluginlistmodel.h"
#include "storageprioritymodel.h"

#include <QAbstractItemModel>
#include <QObject>
#include <QPalette>
#include <QPointer>
#include <QUrl>
#include <QVariantList>

namespace AnyKeep {

class AndroidPlatformServices;
class SpeechRecognitionController;
class SpeechRecognitionProviderInterface;
class DialogService;
class NoteEditor;
class NoteStorage;
class PluginHost;
class NotesWorkspaceController;
class MobileEditorPlatformBackend;

class MobileApplication final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *notesModel READ notesModel CONSTANT)
    Q_PROPERTY(QObject *workspace READ workspace CONSTANT)
    Q_PROPERTY(QObject *dialogs READ dialogs CONSTANT)
    Q_PROPERTY(QAbstractItemModel *pluginsModel READ pluginsModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *storagesModel READ storagesModel CONSTANT)
    Q_PROPERTY(QObject *currentNoteEditor READ currentNoteEditor NOTIFY currentNoteEditorChanged)
    Q_PROPERTY(QObject *editorPlatformBackend READ editorPlatformBackend CONSTANT)
    Q_PROPERTY(QObject *speechController READ speechController CONSTANT)
    Q_PROPERTY(bool askBeforeDelete READ askBeforeDelete WRITE setAskBeforeDelete NOTIFY askBeforeDeleteChanged)
    Q_PROPERTY(int notesPerPage READ notesPerPage WRITE setNotesPerPage NOTIFY notesPerPageChanged)
    Q_PROPERTY(qreal editorFontSize READ editorFontSize WRITE setEditorFontSize NOTIFY editorFontSizeChanged)
    Q_PROPERTY(int colorScheme READ colorScheme WRITE setColorScheme NOTIFY colorSchemeChanged)
    Q_PROPERTY(bool darkColorScheme READ darkColorScheme NOTIFY colorSchemeChanged)
    Q_PROPERTY(bool androidSpeechEnabled READ androidSpeechEnabled WRITE setAndroidSpeechEnabled NOTIFY
                   androidSpeechEnabledChanged)
    Q_PROPERTY(bool androidSpeechAvailable READ androidSpeechAvailable CONSTANT)
    Q_PROPERTY(bool audioRecordingAvailable READ audioRecordingAvailable NOTIFY voiceInputStateChanged)
    Q_PROPERTY(bool microphoneAvailable READ microphoneAvailable NOTIFY voiceInputStateChanged)
    Q_PROPERTY(bool microphoneBusy READ microphoneBusy NOTIFY voiceInputStateChanged)
    Q_PROPERTY(bool microphoneRecording READ microphoneRecording NOTIFY voiceInputStateChanged)
    Q_PROPERTY(bool microphoneModeSwitchVisible READ microphoneModeSwitchVisible NOTIFY voiceInputStateChanged)
    Q_PROPERTY(VoiceInputMode microphoneMode READ microphoneMode WRITE setMicrophoneMode NOTIFY voiceInputStateChanged)
    Q_PROPERTY(bool homeScreenShortcutAvailable READ homeScreenShortcutAvailable CONSTANT)
    Q_PROPERTY(QVariantList recoverableDrafts READ recoverableDrafts NOTIFY recoverableDraftsChanged)

public:
    enum VoiceInputMode {
        AndroidSpeech = 0,
        AudioRecording,
    };
    Q_ENUM(VoiceInputMode)

    explicit MobileApplication(QObject *parent = nullptr);

    QAbstractItemModel *notesModel();
    QAbstractItemModel *pluginsModel();
    QAbstractItemModel *storagesModel();
    QObject            *currentNoteEditor() const;
    QObject            *editorPlatformBackend() const;
    QObject            *speechController() const;
    QObject            *workspace();
    QObject            *dialogs() const;

    bool           askBeforeDelete() const;
    int            notesPerPage() const;
    qreal          editorFontSize() const;
    int            colorScheme() const;
    bool           darkColorScheme() const;
    bool           androidSpeechEnabled() const;
    bool           androidSpeechAvailable() const;
    bool           audioRecordingAvailable() const;
    bool           microphoneAvailable() const;
    bool           microphoneBusy() const;
    bool           microphoneRecording() const;
    bool           microphoneModeSwitchVisible() const;
    VoiceInputMode microphoneMode() const;
    bool           homeScreenShortcutAvailable() const;
    QVariantList   recoverableDrafts() const;

    Q_INVOKABLE bool     createNote();
    Q_INVOKABLE bool     saveCurrentNote();
    Q_INVOKABLE bool     closeCurrentNote();
    Q_INVOKABLE bool     shareCurrentNote();
    Q_INVOKABLE bool     exportCurrentNote();
    Q_INVOKABLE bool     requestSpeechRecognition();
    Q_INVOKABLE bool     requestVoiceInput(int insertionRow = -1);
    Q_INVOKABLE bool     addCurrentNoteToHomeScreen();
    Q_INVOKABLE bool     processPendingLaunchIntent();
    Q_INVOKABLE bool     openDraft(const QString &draftId);
    Q_INVOKABLE bool     discardDraft(const QString &draftId);
    Q_INVOKABLE bool     setPluginEnabled(int row, bool enabled);
    Q_INVOKABLE bool     moveStorage(int sourceRow, int destinationRow);
    Q_INVOKABLE QUrl     pluginSettingsComponent(const QString &pluginId) const;
    Q_INVOKABLE QObject *createPluginSettingsController(const QString &pluginId, QObject *owner);
    Q_INVOKABLE QUrl     storageSettingsComponent(const QString &storageId) const;
    Q_INVOKABLE QObject *createStorageSettingsController(const QString &storageId, QObject *owner);

public slots:
    void setAskBeforeDelete(bool value);
    void setNotesPerPage(int value);
    void setEditorFontSize(qreal value);
    void setColorScheme(int value);
    void setAndroidSpeechEnabled(bool value);
    void setMicrophoneMode(VoiceInputMode mode);

signals:
    void askBeforeDeleteChanged();
    void currentNoteEditorChanged();
    void notesPerPageChanged();
    void editorFontSizeChanged();
    void colorSchemeChanged();
    void androidSpeechEnabledChanged();
    void voiceInputStateChanged();
    void recoverableDraftsChanged();
    void speechRecognized(const QString &text);
    void operationFailed(const QString &message);
    void operationCompleted(const QString &message);

private:
    bool           openEditor(const Note &note, const QUuid &draftId = {});
    void           recoverDraft(NoteStorage *storage);
    QString        currentNoteTitle() const;
    QString        currentNoteFileName(const QString &suffix) const;
    void           applyAndroidSpeechEnabled(bool enabled);
    VoiceInputMode effectiveVoiceInputMode() const;
    void           refreshSpeechProvider();
    void           applyColorScheme();

    AndroidPlatformServices     *platformServices_ { nullptr };
    SpeechRecognitionController *speechController_ { nullptr };
    DialogService               *dialogs_ { nullptr };
    PluginHost                  *pluginHost_ { nullptr };
    BundledPluginRegistry        bundledPlugins_;
    PluginListModel              plugins_;
    StoragePriorityModel         storages_;
    NotesWorkspaceController    *workspace_ { nullptr };
    MobileEditorPlatformBackend *editorPlatformBackend_ { nullptr };
    QString                      initializationError_;
    QString                      handledLaunchUrl_;
    bool                         askBeforeDelete_ { true };
    bool                         androidSpeechEnabled_ { false };
    VoiceInputMode               microphoneMode_ { AndroidSpeech };
    int                          notesPerPage_ { 30 };
    qreal                        editorFontSize_ { 16.0 };
    QPalette                     systemPalette_;
    int                          colorScheme_ { 0 }; // 0: system, 1: light, 2: dark
};

} // namespace AnyKeep

#endif // ANYKEEP_MOBILEAPPLICATION_H
