#include "speechrecognitioncontroller.h"

#include "audioattachmentrecorder.h"
#include "localmediastore.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "speechaudiorecorder.h"
#include "speechrecognitionprovider.h"

#include <QCryptographicHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace AnyKeep {

SpeechRecognitionController::SpeechRecognitionController(QObject *parent) :
    QObject(parent), speechRecorder_(new SpeechAudioRecorder(this)), audioRecorder_(new AudioAttachmentRecorder(this))
{
    mode_ = InputMode(qBound(int(SpeechToText),
                             QSettings().value(QStringLiteral("editor.microphone-mode"), int(SpeechToText)).toInt(),
                             int(AudioRecording)));

    connect(speechRecorder_, &SpeechAudioRecorder::failed, this, &SpeechRecognitionController::operationFailed);
    connect(speechRecorder_, &SpeechAudioRecorder::maxDurationReached, this, &SpeechRecognitionController::finish);
    connect(speechRecorder_, &SpeechAudioRecorder::elapsedChanged, this, [this](qint64 elapsed, qint64 maximum) {
        if (maximum <= 0 || effectiveMode() != SpeechToText)
            return;
        const qint64 seconds = (qMax<qint64>(0, maximum - elapsed) + 999) / 1000;
        statusText_          = tr("%n second(s) left", nullptr, int(seconds));
        emit stateChanged();
    });

    connect(audioRecorder_, &AudioAttachmentRecorder::stateChanged, this, [this] {
        if (audioRecorder_->recording()) {
            starting_ = false;
            if (!busy_) {
                const qint64 seconds = audioRecorder_->duration() / 1000;
                statusText_ = tr("Recording %1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
            }
            if (stopAfterStart_) {
                stopAfterStart_ = false;
                scheduleAudioStop();
            }
        } else if (audioRecorder_->starting()) {
            statusText_ = tr("Initializing microphone…");
        } else if (!audioRecorder_->finalizing() && !busy_) {
            starting_ = false;
            statusText_.clear();
        }
        emit stateChanged();
    });
    connect(audioRecorder_, &AudioAttachmentRecorder::failed, this, [this](const QString &message) {
        audioEditor_.clear();
        pendingAudioRow_  = -1;
        starting_         = false;
        audioStopPending_ = false;
        setBusy(false);
        emit operationFailed(message);
    });
    connect(audioRecorder_, &AudioAttachmentRecorder::recordingReady, this,
            [this](const MediaReference &reference, qint64 durationMs) {
                const QPointer<NoteEditor> destination = audioEditor_;
                const int                  row         = pendingAudioRow_;
                audioEditor_.clear();
                pendingAudioRow_  = -1;
                starting_         = false;
                audioStopPending_ = false;
                setBusy(false);
                if (!destination || !destination->insertAudio(reference, durationMs, row, tr("Audio recording")))
                    emit operationFailed(tr("Could not insert the audio recording into this note."));
            });
}

SpeechRecognitionController::~SpeechRecognitionController() { cancel(); }

void SpeechRecognitionController::setEditor(NoteEditor *editor)
{
    if (editor_ == editor)
        return;
    cancel();
    editor_ = editor;
    localContextId_.clear();
    emit stateChanged();
}

void SpeechRecognitionController::setProvider(SpeechRecognitionProviderInterface *provider)
{
    if (provider_ == provider)
        return;
    cancel();
    provider_ = provider;
    emit stateChanged();
}

bool SpeechRecognitionController::speechAvailable() const
{
    if (!provider_ || !provider_->isSpeechRecognitionReady() || !speechRecorder_->isAvailable())
        return false;
    const auto caps = provider_->speechRecognitionCapabilities();
    return !language().isEmpty() || caps.languages.isEmpty();
}

bool SpeechRecognitionController::audioRecordingAvailable() const
{
    return editor_ && editor_->supportsMedia() && audioRecorder_ && audioRecorder_->available();
}

bool SpeechRecognitionController::audioTranscriptionAvailable() const
{
    return editor_ && provider_ && provider_->isSpeechRecognitionReady()
        && !provider_->speechRecognitionCapabilities().encodedAudioMediaTypes.isEmpty();
}

bool SpeechRecognitionController::modeSwitchVisible() const { return speechAvailable() && audioRecordingAvailable(); }

SpeechRecognitionController::InputMode SpeechRecognitionController::effectiveMode() const
{
    if (mode_ == AudioRecording && audioRecordingAvailable())
        return AudioRecording;
    if (mode_ == SpeechToText && speechAvailable())
        return SpeechToText;
    return audioRecordingAvailable() ? AudioRecording : SpeechToText;
}

bool SpeechRecognitionController::available() const
{
    return effectiveMode() == AudioRecording ? audioRecordingAvailable() : speechAvailable();
}

bool SpeechRecognitionController::recording() const
{
    return effectiveMode() == AudioRecording ? audioRecorder_->recording() : speechRecorder_->isRecording();
}

bool SpeechRecognitionController::busy() const { return busy_ || starting_; }

void SpeechRecognitionController::setMode(InputMode mode)
{
    if (mode != SpeechToText && mode != AudioRecording)
        return;
    if (mode_ == mode)
        return;
    cancel();
    mode_ = mode;
    QSettings().setValue(QStringLiteral("editor.microphone-mode"), int(mode_));
    emit stateChanged();
}

bool SpeechRecognitionController::start(int insertionRow)
{
    if (busy() || !available())
        return false;
    const InputMode requestedMode = effectiveMode();
    if (requestedMode == AudioRecording) {
        if (!editor_ || !editor_->supportsMedia())
            return false;
        audioEditor_     = editor_;
        pendingAudioRow_ = insertionRow;
    }
    starting_                = true;
    stopAfterStart_          = false;
    statusText_              = tr("Initializing microphone…");
    const quint64 generation = ++startGeneration_;
    emit          stateChanged();

    // Let QML render the busy indicator before Qt Multimedia loads its native
    // backend and probes encoders. The audio recorder keeps `starting` true
    // until QMediaRecorder reports the real RecordingState.
    QTimer::singleShot(30, this, [this, generation, requestedMode] {
        if (generation != startGeneration_ || !starting_)
            return;
        if (requestedMode == AudioRecording) {
            if (!audioRecorder_->start()) {
                audioEditor_.clear();
                pendingAudioRow_ = -1;
                starting_        = false;
                statusText_.clear();
                emit stateChanged();
                return;
            }
            if (audioRecorder_->recording()) {
                starting_   = false;
                statusText_ = tr("Recording…");
            }
            emit stateChanged();
        } else {
            if (job_) {
                job_->cancel();
                job_->deleteLater();
                job_.clear();
            }
            const auto caps    = provider_->speechRecognitionCapabilities();
            const int  maximum = caps.maxOneShotDurationMs > 0 ? caps.maxOneShotDurationMs : 60000;
            if (!speechRecorder_->start(maximum)) {
                starting_ = false;
                statusText_.clear();
                emit stateChanged();
                emit operationFailed(speechRecorder_->errorString());
                return;
            }
            starting_   = false;
            statusText_ = tr("Listening…");
            emit stateChanged();
        }

        if (!stopAfterStart_)
            return;
        stopAfterStart_ = false;
        if (requestedMode == AudioRecording) {
            starting_ = false;
            scheduleAudioStop();
        } else {
            finish();
        }
    });
    return true;
}

void SpeechRecognitionController::finish()
{
    if (starting_) {
        stopAfterStart_ = true;
        return;
    }
    if (effectiveMode() == AudioRecording) {
        if (!audioRecorder_->starting() && !audioRecorder_->recording())
            return;
        scheduleAudioStop();
        return;
    }

    if (!speechRecorder_->isRecording())
        return;
    const auto audio = speechRecorder_->stop();
    if (audio.data.isEmpty() || !provider_ || !provider_->isSpeechRecognitionReady()) {
        statusText_.clear();
        emit stateChanged();
        return;
    }

    SpeechRecognitionRequest request;
    request.contextId = contextId();
    request.language  = language();
    request.locale    = request.language.isEmpty() ? QLocale() : QLocale(request.language);
    job_              = provider_->recognizeSpeech(audio, request);
    if (!job_) {
        emit operationFailed(tr("Speech recognition provider did not start a recognition job."));
        statusText_.clear();
        emit stateChanged();
        return;
    }

    setBusy(true, tr("Recognizing speech…"));
    const auto guard = job_;
    connect(job_, &SpeechRecognitionJob::finished, this, [this, guard](const QString &text) {
        if (guard != job_)
            return;
        job_->deleteLater();
        job_.clear();
        setBusy(false);
        const QString value = text.trimmed();
        if (!value.isEmpty())
            emit recognizedText(value);
    });
    connect(job_, &SpeechRecognitionJob::failed, this, [this, guard](const QString &error) {
        if (guard != job_)
            return;
        job_->deleteLater();
        job_.clear();
        setBusy(false);
        emit operationFailed(error);
    });
}

void SpeechRecognitionController::cancel()
{
    ++startGeneration_;
    ++finishGeneration_;
    starting_         = false;
    stopAfterStart_   = false;
    audioStopPending_ = false;
    if (speechRecorder_)
        speechRecorder_->cancel();
    if (audioRecorder_)
        audioRecorder_->cancel();
    audioEditor_.clear();
    pendingAudioRow_ = -1;
    transcriptionEditor_.clear();
    transcriptionIndex_ = QPersistentModelIndex();
    if (job_) {
        job_->cancel();
        job_->deleteLater();
        job_.clear();
    }
    setBusy(false);
}

bool SpeechRecognitionController::transcribeAudio(int row, const QString &sourceUri, qint64 durationMs)
{
    if (busy() || !audioTranscriptionAvailable() || !editor_ || row < 0 || sourceUri.isEmpty())
        return false;

    const QModelIndex modelIndex = editor_->model()->index(row, 0);
    if (!modelIndex.isValid()
        || editor_->model()->data(modelIndex, NoteBlockModel::TypeRole).toInt() != NoteBlockModel::Audio
        || editor_->model()->data(modelIndex, NoteBlockModel::UrlRole).toString() != sourceUri) {
        emit operationFailed(tr("The selected audio block is no longer available."));
        return false;
    }

    const auto media     = editor_->media();
    const auto reference = std::find_if(media.cbegin(), media.cend(), [&sourceUri](const MediaReference &item) {
        return item.isValid() && item.uri() == sourceUri;
    });
    static const QRegularExpression safeAudioType(QStringLiteral(R"(^audio/[A-Za-z0-9.+-]+$)"));
    if (reference == media.cend() || !safeAudioType.match(reference->mediaType).hasMatch()) {
        emit operationFailed(tr("The audio recording is not available locally."));
        return false;
    }
    const auto acceptedTypes = provider_->speechRecognitionCapabilities().encodedAudioMediaTypes;
    const bool accepted = std::any_of(acceptedTypes.cbegin(), acceptedTypes.cend(), [reference](const QString &type) {
        return type.compare(reference->mediaType, Qt::CaseInsensitive) == 0
            || (type.endsWith(QLatin1String("/*"))
                && reference->mediaType.startsWith(type.left(type.size() - 1), Qt::CaseInsensitive));
    });
    if (!accepted) {
        emit operationFailed(tr("The selected speech recognition provider does not accept this audio format."));
        return false;
    }
    const auto loaded = LocalMediaStore::instance()->data(reference->blobId);
    if (!loaded) {
        emit operationFailed(tr("Could not read the audio recording: %1").arg(loaded.error));
        return false;
    }

    if (job_) {
        job_->cancel();
        job_->deleteLater();
        job_.clear();
    }
    SpeechRecognitionAudio audio;
    audio.data       = loaded.value;
    audio.format     = QStringLiteral("encoded");
    audio.mediaType  = reference->mediaType;
    audio.fileName   = reference->originalName.isEmpty() ? reference->portableName : reference->originalName;
    audio.durationMs = qMax<qint64>(0, durationMs);

    SpeechRecognitionRequest request;
    request.contextId = contextId();
    request.language  = language();
    request.locale    = request.language.isEmpty() ? QLocale() : QLocale(request.language);
    job_              = provider_->recognizeSpeech(audio, request);
    if (!job_) {
        emit operationFailed(tr("Speech recognition provider did not start a recognition job."));
        return false;
    }

    transcriptionEditor_ = editor_;
    transcriptionIndex_  = QPersistentModelIndex(modelIndex);
    setBusy(true, tr("Transcribing audio…"));
    const auto guard = job_;
    connect(job_, &SpeechRecognitionJob::finished, this, [this, guard](const QString &text) {
        if (guard != job_)
            return;
        job_->deleteLater();
        job_.clear();
        const QPointer<NoteEditor>  destination      = transcriptionEditor_;
        const QPersistentModelIndex destinationIndex = transcriptionIndex_;
        transcriptionEditor_.clear();
        transcriptionIndex_ = QPersistentModelIndex();
        setBusy(false);
        const QString value       = text.trimmed();
        const bool    targetValid = destination && destinationIndex.isValid()
            && destinationIndex.model() == destination->model()
            && destination->model()->data(destinationIndex, NoteBlockModel::TypeRole).toInt() == NoteBlockModel::Audio;
        if (!value.isEmpty() && (!targetValid || !destination->setAudioTranscript(destinationIndex.row(), value)))
            emit operationFailed(tr("Could not attach the transcript to the audio recording."));
    });
    connect(job_, &SpeechRecognitionJob::failed, this, [this, guard](const QString &error) {
        if (guard != job_)
            return;
        job_->deleteLater();
        job_.clear();
        transcriptionEditor_.clear();
        transcriptionIndex_ = QPersistentModelIndex();
        setBusy(false);
        emit operationFailed(error);
    });
    return true;
}

QString SpeechRecognitionController::language() const
{
    if (!provider_)
        return {};
    auto        caps      = provider_->speechRecognitionCapabilities();
    QStringList supported = caps.languages;
    for (auto &item : supported)
        item = normalizeLanguage(item);
    supported.removeAll(QString());
    supported.removeDuplicates();

    const QString system = normalizeLanguage(QLocale().name());
    if (supported.isEmpty())
        return system;
    for (const auto &item : std::as_const(supported)) {
        if (item.compare(system, Qt::CaseInsensitive) == 0)
            return item;
    }
    const QString languageOnly = system.section(QLatin1Char('-'), 0, 0);
    for (const auto &item : std::as_const(supported)) {
        if (item.section(QLatin1Char('-'), 0, 0).compare(languageOnly, Qt::CaseInsensitive) == 0)
            return item;
    }
    const QString preferred = normalizeLanguage(caps.preferredLanguage);
    for (const auto &item : std::as_const(supported)) {
        if (item.compare(preferred, Qt::CaseInsensitive) == 0)
            return item;
    }
    return supported.constFirst();
}

QString SpeechRecognitionController::normalizeLanguage(const QString &value) const
{
    QString normalized = value.trimmed();
    normalized.replace(QLatin1Char('_'), QLatin1Char('-'));
    auto parts = normalized.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {};
    parts[0] = parts[0].toLower();
    if (parts.size() > 1)
        parts[1] = parts[1].toUpper();
    return parts.join(QLatin1Char('-'));
}

QString SpeechRecognitionController::contextId() const
{
    if (editor_ && !editor_->noteId().isEmpty()) {
        const QByteArray key = editor_->storageId().toUtf8() + '\0' + editor_->noteId().toUtf8();
        return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
    }
    if (localContextId_.isEmpty())
        const_cast<SpeechRecognitionController *>(this)->localContextId_
            = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return localContextId_;
}

void SpeechRecognitionController::scheduleAudioStop()
{
    if (audioStopPending_ || (!audioRecorder_->starting() && !audioRecorder_->recording()))
        return;

    // Native capture pipelines commonly deliver their final microphone
    // buffers a little after the user releases the button. Keep recording for
    // a short tail so the last syllable reaches QMediaRecorder before stop().
    audioStopPending_        = true;
    const quint64 generation = ++finishGeneration_;
    setBusy(true, tr("Finishing audio recording…"));
    QTimer::singleShot(300, this, [this, generation] {
        if (generation != finishGeneration_ || !audioStopPending_)
            return;
        audioStopPending_ = false;
        if (!audioRecorder_->starting() && !audioRecorder_->recording()) {
            setBusy(false);
            return;
        }
        setBusy(true, tr("Saving audio recording…"));
        audioRecorder_->stop();
    });
}

void SpeechRecognitionController::setBusy(bool busy, const QString &status)
{
    busy_       = busy;
    statusText_ = status;
    emit stateChanged();
}

} // namespace AnyKeep
