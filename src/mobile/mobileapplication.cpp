#include "mobileapplication.h"

#include "androidplatformservices.h"
#include "corestorageregistry.h"
#include "dialogservice.h"
#include "draftmanager.h"
#include "mobilebundledplugins.h"
#include "mobileeditorplatformbackend.h"
#include "noteeditor.h"
#include "notemanager.h"
#include "notesmodel.h"
#include "notesworkspacecontroller.h"
#include "notetitleresolver.h"
#include "pluginhost.h"
#include "settingscontroller.h"
#include "speechrecognitioncontroller.h"
#include "speechrecognitionprovider.h"

#include <QGuiApplication>
#include <QLocale>
#include <QLoggingCategory>
#include <QPalette>
#include <QStyleHints>
#if (defined(Q_OS_ANDROID) || defined(Q_OS_IOS)) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QPermissions>
#endif
#include <QRegularExpression>
#include <QSettings>
#include <QUrlQuery>
#include <QVariantMap>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QtCore/qcoreapplication_platform.h>
#endif

#include <algorithm>

namespace AnyKeep {

Q_LOGGING_CATEGORY(logMobilePersistence, "anykeep.persistence.mobile")

namespace {
#ifdef Q_OS_ANDROID
    void applyAndroidNavigationBar(const QColor &background, bool darkBackground)
    {
        const jint color     = jint(background.rgba());
        const bool darkIcons = !darkBackground;
        QNativeInterface::QAndroidApplication::runOnAndroidMainThread([color, darkIcons] {
            const QJniObject activity = QNativeInterface::QAndroidApplication::context();
            if (!activity.isValid())
                return;
            const QJniObject window = activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
            if (!window.isValid())
                return;

            window.callMethod<void>("setNavigationBarColor", "(I)V", color);
            const int sdk = QNativeInterface::QAndroidApplication::sdkVersion();
            if (sdk >= 28)
                window.callMethod<void>("setNavigationBarDividerColor", "(I)V", color);
            if (sdk >= 29)
                window.callMethod<void>("setNavigationBarContrastEnforced", "(Z)V", jboolean(false));

            constexpr jint LightNavigationBars = 0x10;
            if (sdk >= 30) {
                const QJniObject controller
                    = window.callObjectMethod("getInsetsController", "()Landroid/view/WindowInsetsController;");
                if (controller.isValid())
                    controller.callMethod<void>("setSystemBarsAppearance", "(II)V", darkIcons ? LightNavigationBars : 0,
                                                LightNavigationBars);
            } else {
                const QJniObject decor = window.callObjectMethod("getDecorView", "()Landroid/view/View;");
                if (decor.isValid()) {
                    jint flags = decor.callMethod<jint>("getSystemUiVisibility", "()I");
                    flags      = darkIcons ? flags | LightNavigationBars : flags & ~LightNavigationBars;
                    decor.callMethod<void>("setSystemUiVisibility", "(I)V", flags);
                }
            }
        });
    }
#endif

    const char *applicationStateName(Qt::ApplicationState state)
    {
        switch (state) {
        case Qt::ApplicationSuspended:
            return "suspended";
        case Qt::ApplicationHidden:
            return "hidden";
        case Qt::ApplicationInactive:
            return "inactive";
        case Qt::ApplicationActive:
            return "active";
        }
        return "unknown";
    }
} // namespace

MobileApplication::MobileApplication(QObject *parent) :
    QObject(parent), platformServices_(new AndroidPlatformServices(this)), dialogs_(new DialogService(this)),
    bundledPlugins_(this), plugins_(&bundledPlugins_, this), storages_(this)
{
    systemPalette_ = QGuiApplication::palette();
    qCInfo(logMobilePersistence) << "Mobile persistence diagnostics started: pid="
                                 << QCoreApplication::applicationPid();
    workspace_             = new NotesWorkspaceController(this);
    speechController_      = new SpeechRecognitionController(this);
    editorPlatformBackend_ = new MobileEditorPlatformBackend(platformServices_, this);
    pluginHost_            = new PluginHost(this);
    bundledPlugins_.setHost(pluginHost_);
    pluginHost_->attachSpellCheck(editorPlatformBackend_);
    connect(pluginHost_, &PluginHost::rehightlight_requested, editorPlatformBackend_,
            &EditorPlatformBackend::rehighlight);
    connect(pluginHost_, &PluginHost::spellCheckProviderConflict, this,
            [this](const QString &active, const QString &ignored) {
                emit operationFailed(tr("Spell checker %1 is already active; %2 was ignored.").arg(active, ignored));
            });
    editorPlatformBackend_->setEditor(workspace_->editor());
    connect(workspace_, &NotesWorkspaceController::currentEditorChanged, this, [this] {
        editorPlatformBackend_->setEditor(workspace_->editor());
        speechController_->setEditor(workspace_->editor());
        const auto *editor = workspace_->editor();
        qCInfo(logMobilePersistence) << "Current mobile editor changed: present=" << bool(editor)
                                     << "storage=" << (editor ? editor->storageId() : QString())
                                     << "noteIdPresent=" << (editor ? !editor->noteId().isEmpty() : false)
                                     << "draft=" << (editor ? editor->draftIdString() : QString());
        emit currentNoteEditorChanged();
        emit voiceInputStateChanged();
    });
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        const auto *editor = workspace_->editor();
        qCInfo(logMobilePersistence) << "Android application state changed:" << applicationStateName(state)
                                     << "editor=" << bool(editor)
                                     << "storage=" << (editor ? editor->storageId() : QString())
                                     << "noteIdPresent=" << (editor ? !editor->noteId().isEmpty() : false)
                                     << "draft=" << (editor ? editor->draftIdString() : QString())
                                     << "dirty=" << (editor ? editor->isDirty() : false);
    });
    connect(editorPlatformBackend_, &EditorPlatformBackend::operationFailed, this, &MobileApplication::operationFailed);
    connect(platformServices_, &AndroidPlatformServices::speechRecognized, this, &MobileApplication::speechRecognized);
    connect(platformServices_, &AndroidPlatformServices::operationFailed, this, &MobileApplication::operationFailed);
    connect(platformServices_, &AndroidPlatformServices::exportCompleted, this,
            [this]() { emit operationCompleted(tr("Note exported.")); });
    connect(speechController_, &SpeechRecognitionController::stateChanged, this,
            &MobileApplication::voiceInputStateChanged);
    connect(speechController_, &SpeechRecognitionController::operationFailed, this,
            &MobileApplication::operationFailed);

    QSettings settings;
    askBeforeDelete_      = settings.value(QStringLiteral("ui.ask-on-delete"), true).toBool();
    notesPerPage_         = settings.value(QStringLiteral("mobile.notes-per-page"), 30).toInt();
    androidSpeechEnabled_ = settings.value(QStringLiteral("mobile.android-speech-enabled"), false).toBool();
    microphoneMode_       = VoiceInputMode(
        qBound(int(AndroidSpeech), settings.value(QStringLiteral("mobile.microphone-mode"), int(AndroidSpeech)).toInt(),
               int(AudioRecording)));
    workspace_->sourceModel()->setPageSize(notesPerPage_);
    editorFontSize_ = settings.value(QStringLiteral("mobile.editor-font-size"), 16.0).toReal();
    colorScheme_    = qBound(0, settings.value(QStringLiteral("mobile.color-scheme"), 0).toInt(), 2);
    applyColorScheme();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (colorScheme_ == 0) {
            applyColorScheme();
            emit colorSchemeChanged();
        }
    });

    if (androidSpeechEnabled_ && !androidSpeechAvailable()) {
        androidSpeechEnabled_ = false;
        settings.setValue(QStringLiteral("mobile.android-speech-enabled"), false);
    }

    auto *drafts = DraftManager::instance();
    connect(drafts, &DraftManager::draftsChanged, this, &MobileApplication::recoverableDraftsChanged);
    if (!drafts->initialize(&initializationError_))
        qWarning() << "Failed to initialize encrypted draft store:" << initializationError_;

    auto *notes = NoteManager::instance();
    connect(notes, &NoteManager::storageReady, this, [this](const NoteStorage::Ptr &storage) {
        qCInfo(logMobilePersistence) << "Mobile storage became ready:" << (storage ? storage->systemName() : QString())
                                     << "accessible=" << (storage ? storage->isAccessible() : false);
        recoverDraft(storage.data());
    });
    registerCoreStorages();

    registerMobileBundledPlugins(bundledPlugins_);
    connect(&bundledPlugins_, &BundledPluginRegistry::pluginsReset, this, &MobileApplication::refreshSpeechProvider);
    connect(&bundledPlugins_, &BundledPluginRegistry::pluginChanged, this,
            [this](const QString &) { refreshSpeechProvider(); });
    bundledPlugins_.initializeEnabledPlugins();
    speechController_->setEditor(workspace_->editor());
    refreshSpeechProvider();
}

QAbstractItemModel *MobileApplication::notesModel() { return workspace_->recentNotesModel(); }
QAbstractItemModel *MobileApplication::pluginsModel() { return &plugins_; }
QAbstractItemModel *MobileApplication::storagesModel() { return &storages_; }
QObject            *MobileApplication::currentNoteEditor() const { return workspace_->currentEditor(); }
QObject            *MobileApplication::editorPlatformBackend() const { return editorPlatformBackend_; }
QObject            *MobileApplication::speechController() const { return speechController_; }
QObject            *MobileApplication::workspace() { return workspace_; }
QObject            *MobileApplication::dialogs() const { return dialogs_; }

bool  MobileApplication::askBeforeDelete() const { return askBeforeDelete_; }
int   MobileApplication::notesPerPage() const { return notesPerPage_; }
qreal MobileApplication::editorFontSize() const { return editorFontSize_; }
int   MobileApplication::colorScheme() const { return colorScheme_; }
bool  MobileApplication::darkColorScheme() const
{
    if (colorScheme_ == 2)
        return true;
    if (colorScheme_ == 1)
        return false;
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    return scheme == Qt::ColorScheme::Dark
        || (scheme == Qt::ColorScheme::Unknown && systemPalette_.color(QPalette::Window).lightness() < 128);
}
bool MobileApplication::androidSpeechEnabled() const { return androidSpeechEnabled_; }
bool MobileApplication::androidSpeechAvailable() const { return platformServices_->speechRecognitionAvailable(); }
bool MobileApplication::audioRecordingAvailable() const
{
    const auto *editor = workspace_ ? workspace_->editor() : nullptr;
    return editor && speechController_ && speechController_->audioRecordingAvailable();
}
MobileApplication::VoiceInputMode MobileApplication::effectiveVoiceInputMode() const
{
    const bool speech = androidSpeechEnabled_ && androidSpeechAvailable();
    const bool audio  = audioRecordingAvailable();
    if (microphoneMode_ == AudioRecording && audio)
        return AudioRecording;
    if (microphoneMode_ == AndroidSpeech && speech)
        return AndroidSpeech;
    return audio ? AudioRecording : AndroidSpeech;
}
bool MobileApplication::microphoneAvailable() const
{
    return effectiveVoiceInputMode() == AudioRecording ? audioRecordingAvailable()
                                                       : androidSpeechEnabled_ && androidSpeechAvailable();
}
bool MobileApplication::microphoneBusy() const
{
    return speechController_ && speechController_->busy() && speechController_->transcribingAudioRow() < 0;
}
bool MobileApplication::microphoneRecording() const { return speechController_ && speechController_->recording(); }
bool MobileApplication::microphoneModeSwitchVisible() const
{
    return androidSpeechEnabled_ && androidSpeechAvailable() && audioRecordingAvailable();
}
MobileApplication::VoiceInputMode MobileApplication::microphoneMode() const { return effectiveVoiceInputMode(); }
bool MobileApplication::homeScreenShortcutAvailable() const { return platformServices_->homeScreenShortcutAvailable(); }

QVariantList MobileApplication::recoverableDrafts() const
{
    auto records = DraftManager::instance()->recoverableDrafts();
    std::sort(records.begin(), records.end(),
              [](const DraftRecord &left, const DraftRecord &right) { return left.updatedAt > right.updatedAt; });

    QVariantList result;
    result.reserve(records.size());
    for (const auto &record : records) {
        QString preview = record.body.simplified();
        if (preview.size() > 180)
            preview = preview.left(177) + QStringLiteral("...");

        QString storageName = tr("Unassigned");
        if (!record.storageId.isEmpty()) {
            const auto storage = NoteManager::instance()->storage(record.storageId);
            storageName        = storage ? storage->name() : record.storageId;
        }

        QString displayTitle = NoteTitleResolver::displayTitle(record.title, record.body, record.format);
        if (displayTitle.isEmpty())
            displayTitle = tr("Untitled note");
        result.append(QVariantMap {
            { QStringLiteral("draftId"), record.id.toString(QUuid::WithoutBraces) },
            { QStringLiteral("title"), displayTitle },
            { QStringLiteral("preview"), preview },
            { QStringLiteral("storageName"), storageName },
            { QStringLiteral("updated"), QLocale().toString(record.updatedAt.toLocalTime(), QLocale::ShortFormat) },
            { QStringLiteral("lastError"), record.lastError },
        });
    }
    return result;
}

bool MobileApplication::createNote()
{
    qCInfo(logMobilePersistence) << "Mobile create-note requested";
    if (!DraftManager::instance()->isReady()) {
        qWarning() << "Cannot create note; draft store is unavailable:" << initializationError_;
        return false;
    }
    const bool created = workspace_->createNote();
    qCInfo(logMobilePersistence) << "Mobile create-note result:" << created;
    return created;
}

bool MobileApplication::openEditor(const Note &note, const QUuid &draftId)
{
    return workspace_->openNote(note, draftId);
}

void MobileApplication::recoverDraft(NoteStorage *storage)
{
    qCInfo(logMobilePersistence) << "Attempting mobile draft recovery: storage="
                                 << (storage ? storage->systemName() : QString())
                                 << "currentEditor=" << bool(workspace_->currentEditor())
                                 << "draftStoreReady=" << DraftManager::instance()->isReady();
    if (!storage || workspace_->currentEditor() || !DraftManager::instance()->isReady())
        return;
    const auto drafts = DraftManager::instance()->recoverableDrafts();
    qCInfo(logMobilePersistence) << "Recoverable drafts considered for storage" << storage->systemName()
                                 << drafts.size();
    for (const auto &draft : drafts) {
        if (!draft.storageId.isEmpty() && draft.storageId != storage->systemName())
            continue;
        qCInfo(logMobilePersistence) << "Trying draft recovery: draft=" << draft.id.toString(QUuid::WithoutBraces)
                                     << "storage=" << draft.storageId
                                     << "remoteNotePresent=" << !draft.remoteNoteId.isEmpty()
                                     << "revision=" << draft.revision;
        auto note = draft.remoteNoteId.isEmpty() ? storage->createNote() : storage->note(draft.remoteNoteId);
        if (!note.isNull() && openEditor(note, draft.id)) {
            qCInfo(logMobilePersistence) << "Recovered draft into editor" << draft.id.toString(QUuid::WithoutBraces);
            return;
        }
        qCWarning(logMobilePersistence) << "Failed to recover draft" << draft.id.toString(QUuid::WithoutBraces)
                                        << "noteNull=" << note.isNull();
    }
}

bool MobileApplication::openDraft(const QString &draftId)
{
    if (workspace_->currentEditor()) {
        emit operationFailed(tr("Close the current note before opening another draft."));
        return false;
    }

    const QUuid id(draftId);
    if (id.isNull()) {
        emit operationFailed(tr("The draft identifier is invalid."));
        return false;
    }
    const auto draft = DraftManager::instance()->editingDraft(id);
    if (!draft) {
        emit operationFailed(draft.error.message.isEmpty() ? tr("The draft is no longer available.")
                                                           : draft.error.message);
        return false;
    }

    const auto storage = draft.value.storageId.isEmpty() ? NoteManager::instance()->defaultStorage()
                                                         : NoteManager::instance()->storage(draft.value.storageId);
    if (!storage) {
        emit operationFailed(tr("The storage associated with this draft is unavailable."));
        return false;
    }

    const auto note
        = draft.value.remoteNoteId.isEmpty() ? storage->createNote() : storage->note(draft.value.remoteNoteId);
    if (note.isNull()) {
        emit operationFailed(tr("The note associated with this draft could not be opened."));
        return false;
    }
    return openEditor(note, id);
}

bool MobileApplication::discardDraft(const QString &draftId)
{
    const QUuid id(draftId);
    if (id.isNull()) {
        emit operationFailed(tr("The draft identifier is invalid."));
        return false;
    }
    const auto error = DraftManager::instance()->discard(id);
    if (error) {
        emit operationFailed(error.message);
        return false;
    }
    return true;
}

bool MobileApplication::saveCurrentNote()
{
    const auto *editor = workspace_->editor();
    qCInfo(logMobilePersistence) << "Mobile checkpoint requested: editor=" << bool(editor)
                                 << "storage=" << (editor ? editor->storageId() : QString())
                                 << "noteIdPresent=" << (editor ? !editor->noteId().isEmpty() : false)
                                 << "draft=" << (editor ? editor->draftIdString() : QString())
                                 << "dirty=" << (editor ? editor->isDirty() : false);
    const bool saved = workspace_->saveCurrentNote();
    qCInfo(logMobilePersistence) << "Mobile checkpoint result:" << saved;
    return saved;
}

bool MobileApplication::closeCurrentNote()
{
    const auto *editor = workspace_->editor();
    qCInfo(logMobilePersistence) << "Mobile close-note requested: editor=" << bool(editor)
                                 << "storage=" << (editor ? editor->storageId() : QString())
                                 << "noteIdPresent=" << (editor ? !editor->noteId().isEmpty() : false)
                                 << "draft=" << (editor ? editor->draftIdString() : QString())
                                 << "dirty=" << (editor ? editor->isDirty() : false);
    const bool closed = workspace_->closeCurrentNote();
    qCInfo(logMobilePersistence) << "Mobile close-note result:" << closed;
    return closed;
}

QString MobileApplication::currentNoteTitle() const
{
    auto title = workspace_->currentTitle().trimmed();
    return title.isEmpty() ? tr("AnyKeep note") : title;
}

QString MobileApplication::currentNoteFileName(const QString &suffix) const
{
    auto name = currentNoteTitle();
    name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]")), QStringLiteral("_"));
    name = name.trimmed().left(96);
    if (name.isEmpty())
        name = QStringLiteral("anykeep-note");
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
    if (!platformServices_->exportData(currentNoteFileName(markdown ? QStringLiteral(".md") : QStringLiteral(".txt")),
                                       QStringLiteral("text/plain"), editor->text().toUtf8())) {
        emit operationFailed(tr("Could not open the system document exporter."));
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
    emit voiceInputStateChanged();
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
        qApp->requestPermission(permission, this, [this](const QPermission &result) {
            if (result.status() == Qt::PermissionStatus::Granted) {
                applyAndroidSpeechEnabled(true);
            } else {
                applyAndroidSpeechEnabled(false);
                emit operationFailed(tr("Microphone permission is required for voice input."));
            }
        });
        return;
    }
#else
    applyAndroidSpeechEnabled(false);
#endif
}

void MobileApplication::setMicrophoneMode(VoiceInputMode mode)
{
    if (mode != AndroidSpeech && mode != AudioRecording)
        return;
    if (microphoneMode_ == mode)
        return;
    if (speechController_)
        speechController_->cancel();
    microphoneMode_ = mode;
    QSettings().setValue(QStringLiteral("mobile.microphone-mode"), int(mode));
    emit voiceInputStateChanged();
}

bool MobileApplication::requestVoiceInput(int insertionRow)
{
    if (microphoneBusy())
        return false;
    if (effectiveVoiceInputMode() == AndroidSpeech)
        return requestSpeechRecognition();
    if (!speechController_)
        return false;
    speechController_->setMode(SpeechRecognitionController::AudioRecording);
    if (speechController_->recording()) {
        speechController_->finish();
        return true;
    }

    NoteEditor *expectedEditor = workspace_->editor();
#if defined(Q_OS_ANDROID) || (defined(Q_OS_IOS) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0))
    QMicrophonePermission permission;
    switch (qApp->checkPermission(permission)) {
    case Qt::PermissionStatus::Granted:
        return expectedEditor && speechController_->start(insertionRow);
    case Qt::PermissionStatus::Denied:
        emit operationFailed(tr("Microphone permission is required for audio recording."));
        return false;
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(
            permission, this,
            [this, expected = QPointer<NoteEditor>(expectedEditor), insertionRow](const QPermission &result) {
                if (!expected || expected != workspace_->editor())
                    return;
                if (result.status() == Qt::PermissionStatus::Granted)
                    speechController_->start(insertionRow);
                else
                    emit operationFailed(tr("Microphone permission is required for audio recording."));
            });
        return true;
    }
#else
    return expectedEditor && speechController_->start(insertionRow);
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

void MobileApplication::refreshSpeechProvider()
{
    SpeechRecognitionProviderInterface *selected = nullptr;
    for (const QString &pluginId : bundledPlugins_.pluginIds()) {
        QObject *instance = bundledPlugins_.instance(pluginId);
        auto    *provider = instance ? qobject_cast<SpeechRecognitionProviderInterface *>(instance) : nullptr;
        if (provider && provider->isSpeechRecognitionReady()) {
            selected = provider;
            break;
        }
    }
    speechController_->setProvider(selected);
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
    if (!url.isValid() || url.scheme() != QStringLiteral("anykeep") || url.host() != QStringLiteral("note"))
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

bool MobileApplication::moveStorage(int sourceRow, int destinationRow)
{
    return storages_.moveStorage(sourceRow, destinationRow);
}

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

void MobileApplication::setColorScheme(int value)
{
    value = qBound(0, value, 2);
    if (colorScheme_ == value)
        return;
    colorScheme_ = value;
    QSettings().setValue(QStringLiteral("mobile.color-scheme"), value);
    applyColorScheme();
    emit colorSchemeChanged();
}

void MobileApplication::applyColorScheme()
{
    const bool   dark = darkColorScheme();
    QPalette     palette;
    const QColor window   = dark ? QColor(QStringLiteral("#202124")) : QColor(QStringLiteral("#fafafa"));
    const QColor base     = dark ? QColor(QStringLiteral("#303030")) : QColor(Qt::white);
    const QColor text     = dark ? QColor(QStringLiteral("#f5f5f5")) : QColor(QStringLiteral("#202124"));
    const QColor muted    = dark ? QColor(QStringLiteral("#b8bec7")) : QColor(QStringLiteral("#667085"));
    const QColor border   = dark ? QColor(QStringLiteral("#555a63")) : QColor(QStringLiteral("#d0d5dd"));
    const QColor selected = dark ? QColor(QStringLiteral("#5f6368")) : QColor(QStringLiteral("#dfe3e8"));
    for (const auto group : { QPalette::Active, QPalette::Inactive, QPalette::Disabled }) {
        palette.setColor(group, QPalette::Window, window);
        palette.setColor(group, QPalette::Base, base);
        palette.setColor(group, QPalette::AlternateBase, window);
        palette.setColor(group, QPalette::Button, window);
        palette.setColor(group, QPalette::Text, text);
        palette.setColor(group, QPalette::WindowText, text);
        palette.setColor(group, QPalette::ButtonText, text);
        palette.setColor(group, QPalette::PlaceholderText, muted);
        palette.setColor(group, QPalette::Mid, border);
        palette.setColor(group, QPalette::Highlight, selected);
        palette.setColor(group, QPalette::HighlightedText, text);
    }
    QGuiApplication::setPalette(palette);
#ifdef Q_OS_ANDROID
    applyAndroidNavigationBar(window, dark);
#endif
}

} // namespace AnyKeep
