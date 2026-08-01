#include "audioattachmentrecorder.h"

#include "localmediastore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <QUrl>

#include <utility>

#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#include <QAudioDevice>
#include <QAudioInput>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QTemporaryDir>
#endif

namespace QtNote {

class AudioAttachmentRecorder::Impl {
public:
    explicit Impl(AudioAttachmentRecorder *q) : owner(q)
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        audioInput = std::make_unique<QAudioInput>();
        recorder   = std::make_unique<QMediaRecorder>();
        session    = std::make_unique<QMediaCaptureSession>();
        session->setAudioInput(audioInput.get());
        session->setRecorder(recorder.get());

        QObject::connect(recorder.get(), &QMediaRecorder::durationChanged, owner, [this](qint64 value) {
            durationMs = qMax<qint64>(0, value);
            emit owner->stateChanged();
        });
        QObject::connect(recorder.get(), &QMediaRecorder::recorderStateChanged, owner,
                         [this](QMediaRecorder::RecorderState state) {
                             if (state == QMediaRecorder::RecordingState) {
                                 startRequested = false;
                                 emit owner->stateChanged();
                                 if (stopRequested) {
                                     QTimer::singleShot(0, owner, [this] {
                                         if (recorder && stopRequested
                                             && recorder->recorderState() == QMediaRecorder::RecordingState) {
                                             recorder->stop();
                                         }
                                     });
                                 }
                                 return;
                             }

                             emit owner->stateChanged();
                             if (state != QMediaRecorder::StoppedState)
                                 return;
                             if (cancelRequested) {
                                 startRequested = false;
                                 cleanup();
                                 return;
                             }
                             if (stopRequested && !startRequested)
                                 QTimer::singleShot(0, owner, [this] { finalize(); });
                         });
        QObject::connect(recorder.get(), &QMediaRecorder::errorOccurred, owner,
                         [this](QMediaRecorder::Error, const QString &message) {
                             if (cancelRequested)
                                 return;
                             fail(message.isEmpty() ? AudioAttachmentRecorder::tr("Audio recording failed.") : message);
                         });
#endif
    }

    ~Impl()
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        // Close the native encoder before QTemporaryDir removes its plaintext
        // output. This ordering also matters during application shutdown.
        session.reset();
        recorder.reset();
        audioInput.reset();
        temporaryDir.reset();
#endif
    }

#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    bool resolvePortableFormat(QMediaFormat *result) const
    {
        if (!recorder || !recorder->isAvailable())
            return false;

        const QMediaFormat::FileFormat containers[] = { QMediaFormat::Mpeg4Audio, QMediaFormat::MPEG4 };
        for (const auto container : containers) {
            QMediaFormat candidate(container);
            candidate.setAudioCodec(QMediaFormat::AudioCodec::AAC);
            candidate.resolveForEncoding(QMediaFormat::NoFlags);
            if (candidate.fileFormat() == container && candidate.audioCodec() == QMediaFormat::AudioCodec::AAC) {
                if (result)
                    *result = candidate;
                return true;
            }
        }
        return false;
    }
#endif

    bool portableFormatAvailable() const
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        return resolvePortableFormat(nullptr);
#else
        return false;
#endif
    }

    bool start()
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        QMediaFormat format;
        if (recording() || finalizing)
            return false;
        if (!resolvePortableFormat(&format)) {
            fail(AudioAttachmentRecorder::tr(
                "AAC audio recording in an M4A container is not available on this system."));
            return false;
        }
        if (QMediaDevices::defaultAudioInput().isNull()) {
            fail(AudioAttachmentRecorder::tr("No audio input device is available."));
            return false;
        }

        cleanup();
        error.clear();
        durationMs      = 0;
        startRequested  = false;
        stopRequested   = false;
        cancelRequested = false;
        finalizing      = false;

        temporaryDir = std::make_unique<QTemporaryDir>(QDir::tempPath() + QStringLiteral("/qtnote-audio-XXXXXX"));
        if (!temporaryDir->isValid()) {
            fail(AudioAttachmentRecorder::tr("Could not create a temporary recording directory."));
            return false;
        }
        QFile::setPermissions(temporaryDir->path(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        outputPath = temporaryDir->filePath(QStringLiteral("recording.m4a"));

        recorder->setMediaFormat(format);
        recorder->setAudioChannelCount(1);
        recorder->setAudioSampleRate(48000);
        recorder->setAudioBitRate(64000);
        recorder->setEncodingMode(QMediaRecorder::AverageBitRateEncoding);
        recorder->setQuality(QMediaRecorder::NormalQuality);
        recorder->setOutputLocation(QUrl::fromLocalFile(outputPath));
        startRequested = true;
        recorder->record();
        if (recorder->recorderState() == QMediaRecorder::RecordingState)
            startRequested = false;
        if (recorder->error() != QMediaRecorder::NoError) {
            fail(recorder->errorString());
            return false;
        }
        emit owner->stateChanged();
        return true;
#else
        return false;
#endif
    }

    void stop()
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if ((!startRequested && !recording()) || stopRequested)
            return;
        stopRequested = true;
        finalizing    = true;
        emit owner->stateChanged();
        if (recorder->recorderState() == QMediaRecorder::RecordingState) {
            startRequested = false;
            recorder->stop();
            if (recorder->recorderState() == QMediaRecorder::StoppedState)
                QTimer::singleShot(0, owner, [this] { finalize(); });
        }
#endif
    }

    void cancel()
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        cancelRequested = true;
        startRequested  = false;
        stopRequested   = false;
        finalizing      = false;
        durationMs      = 0;
        if (recorder && recorder->recorderState() != QMediaRecorder::StoppedState) {
            recorder->stop();
        } else {
            cleanup();
        }
#else
        finalizing = false;
        durationMs = 0;
        cleanup();
#endif
        emit owner->stateChanged();
    }

    bool recording() const
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        return recorder && !finalizing && !cancelRequested
            && (startRequested || recorder->recorderState() == QMediaRecorder::RecordingState);
#else
        return false;
#endif
    }

    void finalize()
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (!stopRequested || cancelRequested || !finalizing)
            return;
        startRequested = false;
        stopRequested  = false;

        const QMediaFormat actual = recorder->mediaFormat();
        const bool         portableContainer
            = actual.fileFormat() == QMediaFormat::Mpeg4Audio || actual.fileFormat() == QMediaFormat::MPEG4;
        if (!portableContainer || actual.audioCodec() != QMediaFormat::AudioCodec::AAC) {
            fail(AudioAttachmentRecorder::tr(
                "The audio backend substituted a format that is not portable between QtNote platforms."));
            return;
        }

        QString path = recorder->actualLocation().toLocalFile();
        if (path.isEmpty())
            path = outputPath;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            fail(file.errorString());
            return;
        }
        const QByteArray encoded = file.readAll();
        file.close();
        if (encoded.isEmpty()) {
            fail(AudioAttachmentRecorder::tr("The audio backend produced an empty recording."));
            return;
        }

        const QString name = QStringLiteral("Audio_%1.m4a")
                                 .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
        auto imported = LocalMediaStore::instance()->importData(encoded, name, QStringLiteral("audio/mp4"));
        if (!imported) {
            fail(imported.error);
            return;
        }

        finalizing                 = false;
        durationMs                 = qMax(durationMs, recorder->duration());
        const qint64 readyDuration = durationMs;
        cleanup();
        emit owner->stateChanged();
        emit owner->recordingReady(imported.value, readyDuration);
#endif
    }

    void fail(QString message)
    {
        if (message.isEmpty())
            message = AudioAttachmentRecorder::tr("Audio recording failed.");
        error           = std::move(message);
        startRequested  = false;
        stopRequested   = false;
        cancelRequested = true;
        finalizing      = false;
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (recorder && recorder->recorderState() != QMediaRecorder::StoppedState)
            recorder->stop();
        else
            cleanup();
#else
        cleanup();
#endif
        emit owner->stateChanged();
        emit owner->failed(error);
    }

    void cleanup()
    {
        outputPath.clear();
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        temporaryDir.reset();
#endif
    }

    AudioAttachmentRecorder *owner;
    QString                  outputPath;
    QString                  error;
    qint64                   durationMs { 0 };
    bool                     startRequested { false };
    bool                     stopRequested { false };
    bool                     cancelRequested { false };
    bool                     finalizing { false };
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    std::unique_ptr<QAudioInput>          audioInput;
    std::unique_ptr<QMediaRecorder>       recorder;
    std::unique_ptr<QMediaCaptureSession> session;
    std::unique_ptr<QTemporaryDir>        temporaryDir;
#endif
};

AudioAttachmentRecorder::AudioAttachmentRecorder(QObject *parent) : QObject(parent), impl_(std::make_unique<Impl>(this))
{
}
AudioAttachmentRecorder::~AudioAttachmentRecorder() = default;

bool    AudioAttachmentRecorder::available() const { return impl_->portableFormatAvailable(); }
bool    AudioAttachmentRecorder::recording() const { return impl_->recording(); }
bool    AudioAttachmentRecorder::finalizing() const { return impl_->finalizing; }
qint64  AudioAttachmentRecorder::duration() const { return impl_->durationMs; }
QString AudioAttachmentRecorder::errorString() const { return impl_->error; }
bool    AudioAttachmentRecorder::start() { return impl_->start(); }
void    AudioAttachmentRecorder::stop() { impl_->stop(); }
void    AudioAttachmentRecorder::cancel() { impl_->cancel(); }

} // namespace QtNote
