#include "mobileapplication.h"

#include "androidplatformservices.h"
#include "corestorageregistry.h"
#include "dialogservice.h"
#include "draftmanager.h"
#include "mobilebundledplugins.h"
#include "noteeditor.h"
#include "notemanager.h"
#include "notesmodel.h"
#include "notesworkspacecontroller.h"
#include "settingscontroller.h"

#include <QGuiApplication>
#include <QLocale>
#ifdef Q_OS_ANDROID
#include <QPermissions>
#endif
#include <QRegularExpression>
#include <QSettings>
#include <QUrlQuery>

namespace QtNote {

MobileApplication::MobileApplication(QObject *parent) :
    QObject(parent), platformServices_(new AndroidPlatformServices(this)), dialogs_(new DialogService(this)),
    bundledPlugins_(this), plugins_(&bundledPlugins_, this), storages_(this)
{
    workspace_ = new NotesWorkspaceController(this);
    connect(workspace_, &NotesWorkspaceController::currentEditorChanged, this,
            &MobileApplication::currentNoteEditorChanged);
    connect(platformServices_, &AndroidPlatformServices::speechRecognized, this, &MobileApplication::speechRecognized);
    connect(platformServices_, &AndroidPlatformServices::operationFailed, this, &MobileApplication::operationFailed);
    connect(platformServices_, &AndroidPlatformServices::exportCompleted, this,
            [this]() { emit operationCompleted(tr("Note exported.")); });

    QSettings settings;
    askBeforeDelete_      = settings.value(QStringLiteral("ui.ask-on-delete"), true).toBool();
    notesPerPage_         = settings.value(QStringLiteral("mobile.notes-per-page"), 30).toInt();
    androidSpeechEnabled_ = settings.value(QStringLiteral("mobile.android-speech-enabled"), false).toBool();
    workspace_->sourceModel()->setPageSize(notesPerPage_);
    editorFontSize_ = settings.value(QStringLiteral("mobile.editor-font-size"), 16.0).toReal();

    if (androidSpeechEnabled_ && !androidSpeechAvailable()) {
        androidSpeechEnabled_ = false;
        settings.setValue(QStringLiteral("mobile.android-speech-enabled"), false);
    }

    if (!DraftManager::instance()->initialize(&initializationError_))
        qWarning() << "Failed to initialize encrypted draft store:" << initializationError_;

    auto *notes = NoteManager::instance();
    connect(notes, &NoteManager::storageReady, this,
            [this](const NoteStorage::Ptr &storage) { recoverDraft(storage.data()); });
    registerCoreStorages();

    registerMobileBundledPlugins(bundledPlugins_);
    bundledPlugins_.initializeEnabledPlugins();
}

QAbstractItemModel *MobileApplication::notesModel() { return workspace_->recentNotesModel(); }
QAbstractItemModel *MobileApplication::pluginsModel() { return &plugins_; }
QAbstractItemModel *MobileApplication::storagesModel() { return &storages_; }
QObject            *MobileApplication::currentNoteEditor() const { return workspace_->currentEditor(); }
QObject            *MobileApplication::workspace() { return workspace_; }
QObject            *MobileApplication::dialogs() const { return dialogs_; }

bool  MobileApplication::askBeforeDelete() const { return askBeforeDelete_; }
int   MobileApplication::notesPerPage() const { return notesPerPage_; }
qreal MobileApplication::editorFontSize() const { return editorFontSize_; }
bool  MobileApplication::androidSpeechEnabled() const { return androidSpeechEnabled_; }
bool  MobileApplication::androidSpeechAvailable() const { return platformServices_->speechRecognitionAvailable(); }
bool MobileApplication::homeScreenShortcutAvailable() const { return platformServices_->homeScreenShortcutAvailable(); }

bool MobileApplication::createNote()
{
    if (!DraftManager::instance()->isReady()) {
        qWarning() << "Cannot create note; draft store is unavailable:" << initializationError_;
        return false;
    }
    return workspace_->createNote();
}

bool MobileApplication::openEditor(const Note &note, const QUuid &draftId)
{
    return workspace_->openNote(note, draftId);
}

void MobileApplication::recoverDraft(NoteStorage *storage)
{
    if (!storage || workspace_->currentEditor() || !DraftManager::instance()->isReady())
        return;
    for (const auto &draft : DraftManager::instance()->recoverableDrafts()) {
        if (!draft.storageId.isEmpty() && draft.storageId != storage->systemName())
            continue;
        auto note = draft.remoteNoteId.isEmpty() ? storage->createNote() : storage->note(draft.remoteNoteId);
        if (!note.isNull() && openEditor(note, draft.id))
            return;
    }
}

bool MobileApplication::saveCurrentNote() { return workspace_->saveCurrentNote(); }
bool MobileApplication::closeCurrentNote() { return workspace_->closeCurrentNote(); }

QString MobileApplication::currentNoteTitle() const
{
    auto title = workspace_->currentTitle().trimmed();
    return title.isEmpty() ? tr("QtNote note") : title;
}

QString MobileApplication::currentNoteFileName(const QString &suffix) const
{
    auto name = currentNoteTitle();
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")), QStringLiteral("_"));
    name = name.trimmed().left(96);
    if (name.isEmpty())
        name = QStringLiteral("qtnote-note");
    return name + suffix;
}

bool MobileApplication::shareCurrentNote()
{
    const auto *editor = workspace_->editor();
    if (!editor)
        return false;
    if (!platformServices_->shareText(currentNoteTitle(), editor->text())) {
        emit operationFailed(tr("No application is available for sharing this note."));
        return false;
    }
    return true;
}

bool MobileApplication::exportCurrentNote()
{
    const auto *editor = workspace_->editor();
    if (!editor)
        return false;
    const bool markdown = editor->isMarkdown();
    if (!platformServices_->exportText(currentNoteFileName(markdown ? QStringLiteral(".md") : QStringLiteral(".txt")),
                                       QStringLiteral("text/plain"), editor->text().toUtf8())) {
        emit operationFailed(tr("Could not open the Android document exporter."));
        return false;
    }
    return true;
}

void MobileApplication::applyAndroidSpeechEnabled(bool enabled)
{
    if (androidSpeechEnabled_ == enabled)
        return;
    androidSpeechEnabled_ = enabled;
    QSettings().setValue(QStringLiteral("mobile.android-speech-enabled"), enabled);
    emit androidSpeechEnabledChanged();
}

void MobileApplication::setAndroidSpeechEnabled(bool value)
{
    if (!value) {
        applyAndroidSpeechEnabled(false);
        return;
    }
    if (!androidSpeechAvailable()) {
        applyAndroidSpeechEnabled(false);
        emit operationFailed(tr("Android speech recognition is not available on this device."));
        return;
    }

#ifdef Q_OS_ANDROID
    QMicrophonePermission permission;
    switch (qApp->checkPermission(permission)) {
    case Qt::PermissionStatus::Granted:
        applyAndroidSpeechEnabled(true);
        return;
    case Qt::PermissionStatus::Denied:
        applyAndroidSpeechEnabled(false);
        emit operationFailed(tr("Microphone permission is required for voice input."));
        return;
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(permission, this, [this]() { setAndroidSpeechEnabled(true); });
        return;
    }
#else
    applyAndroidSpeechEnabled(false);
#endif
}

bool MobileApplication::requestSpeechRecognition()
{
    if (!androidSpeechEnabled_) {
        emit operationFailed(tr("Enable Android speech recognition in Settings first."));
        return false;
    }

#ifdef Q_OS_ANDROID
    QMicrophonePermission permission;
    if (qApp->checkPermission(permission) != Qt::PermissionStatus::Granted) {
        setAndroidSpeechEnabled(true);
        return false;
    }
#else
    return false;
#endif

    if (!platformServices_->requestSpeechRecognition(QLocale().name().replace(QLatin1Char('_'), QLatin1Char('-')))) {
        emit operationFailed(tr("Could not start Android speech recognition."));
        return false;
    }
    return true;
}

bool MobileApplication::addCurrentNoteToHomeScreen()
{
    const auto *editor = workspace_->editor();
    if (!editor || editor->storageId().isEmpty() || editor->noteId().isEmpty()) {
        emit operationFailed(tr("Save the note before adding it to the Home screen."));
        return false;
    }
    if (!platformServices_->addHomeScreenShortcut(editor->storageId(), editor->noteId(), currentNoteTitle())) {
        emit operationFailed(tr("The launcher could not add this note to the Home screen."));
        return false;
    }
    return true;
}

bool MobileApplication::processPendingLaunchIntent()
{
    const auto url = platformServices_->pendingLaunchUrl();
    if (!url.isValid() || url.scheme() != QStringLiteral("qtnote") || url.host() != QStringLiteral("note"))
        return false;
    const auto encoded = url.toString(QUrl::FullyEncoded);
    if (encoded == handledLaunchUrl_)
        return false;

    const QUrlQuery query(url);
    const auto      storageId = query.queryItemValue(QStringLiteral("storage"));
    const auto      noteId    = query.queryItemValue(QStringLiteral("id"));
    if (storageId.isEmpty() || noteId.isEmpty() || !workspace_->openNote(storageId, noteId))
        return false;
    handledLaunchUrl_ = encoded;
    return true;
}

bool MobileApplication::setPluginEnabled(int row, bool enabled) { return plugins_.setEnabled(row, enabled); }

QUrl MobileApplication::pluginSettingsComponent(const QString &pluginId) const
{
    auto component = bundledPlugins_.settingsComponent(pluginId);
    return component.isEmpty() ? QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml")) : component;
}

QObject *MobileApplication::createPluginSettingsController(const QString &pluginId, QObject *owner)
{
    return bundledPlugins_.createSettingsController(pluginId, owner ? owner : this);
}

QUrl MobileApplication::storageSettingsComponent(const QString &storageId) const
{
    const auto storage = NoteManager::instance()->storage(storageId);
    if (!storage)
        return {};
    const auto component = storage->settingsComponent();
    return component.isEmpty() ? QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml")) : component;
}

QObject *MobileApplication::createStorageSettingsController(const QString &storageId, QObject *owner)
{
    const auto storage = NoteManager::instance()->storage(storageId);
    return storage ? storage->createSettingsController(owner ? owner : this) : nullptr;
}

void MobileApplication::setAskBeforeDelete(bool value)
{
    if (askBeforeDelete_ == value)
        return;
    askBeforeDelete_ = value;
    QSettings().setValue(QStringLiteral("ui.ask-on-delete"), value);
    emit askBeforeDeleteChanged();
}

void MobileApplication::setNotesPerPage(int value)
{
    value = qBound(10, value, 200);
    if (notesPerPage_ == value)
        return;
    notesPerPage_ = value;
    QSettings().setValue(QStringLiteral("mobile.notes-per-page"), value);
    workspace_->sourceModel()->setPageSize(value);
    emit notesPerPageChanged();
}

void MobileApplication::setEditorFontSize(qreal value)
{
    value = qBound(10.0, value, 36.0);
    if (qFuzzyCompare(editorFontSize_, value))
        return;
    editorFontSize_ = value;
    QSettings().setValue(QStringLiteral("mobile.editor-font-size"), value);
    emit editorFontSizeChanged();
}

} // namespace QtNote
