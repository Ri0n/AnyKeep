#include "speechrecognitioncontroller.h"

#include "noteeditor.h"
#include "speechaudiorecorder.h"
#include "speechrecognitionprovider.h"

#include <QCryptographicHash>
#include <QLocale>

#include <utility>

namespace QtNote {

SpeechRecognitionController::SpeechRecognitionController(QObject *parent) :
    QObject(parent), recorder_(new SpeechAudioRecorder(this))
{
    connect(recorder_, &SpeechAudioRecorder::failed, this, &SpeechRecognitionController::operationFailed);
    connect(recorder_, &SpeechAudioRecorder::maxDurationReached, this, &SpeechRecognitionController::finish);
    connect(recorder_, &SpeechAudioRecorder::elapsedChanged, this, [this](qint64 elapsed, qint64 maximum) {
        if (maximum <= 0)
            return;
        const qint64 seconds = (qMax<qint64>(0, maximum - elapsed) + 999) / 1000;
        statusText_          = tr("%n second(s) left", nullptr, int(seconds));
        emit stateChanged();
    });
}

SpeechRecognitionController::~SpeechRecognitionController() { cancel(); }

void SpeechRecognitionController::setEditor(NoteEditor *editor)
{
    if (editor_ == editor)
        return;
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

bool SpeechRecognitionController::available() const
{
    if (!provider_ || !provider_->isSpeechRecognitionReady() || !recorder_->isAvailable())
        return false;
    const auto caps = provider_->speechRecognitionCapabilities();
    return !language().isEmpty() || caps.languages.isEmpty();
}

bool SpeechRecognitionController::recording() const { return recorder_ && recorder_->isRecording(); }

bool SpeechRecognitionController::start()
{
    if (busy_ || !available())
        return false;
    if (job_) {
        job_->cancel();
        job_->deleteLater();
        job_.clear();
    }
    const auto caps    = provider_->speechRecognitionCapabilities();
    const int  maximum = caps.maxOneShotDurationMs > 0 ? caps.maxOneShotDurationMs : 60000;
    if (!recorder_->start(maximum)) {
        emit operationFailed(recorder_->errorString());
        return false;
    }
    statusText_ = tr("Listening…");
    emit stateChanged();
    return true;
}

void SpeechRecognitionController::finish()
{
    if (!recording())
        return;
    const auto audio = recorder_->stop();
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
    if (recorder_)
        recorder_->cancel();
    if (job_) {
        job_->cancel();
        job_->deleteLater();
        job_.clear();
    }
    setBusy(false);
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

void SpeechRecognitionController::setBusy(bool busy, const QString &status)
{
    busy_       = busy;
    statusText_ = status;
    emit stateChanged();
}

} // namespace QtNote
