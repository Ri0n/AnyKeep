#include "mediaplaybacksession.h"

#include "localmediastore.h"
#include "mediarangeservice.h"
#include "mediastream.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QHash>
#include <QPointer>
#include <QUrl>

#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QVideoFrame>
#include <QVideoSink>
#endif

namespace AnyKeep {

class MediaPlaybackSession::Impl {
public:
    Impl(MediaPlaybackSession *q, const MediaReference &value) : owner(q), reference(value) {}

#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    bool ensurePlayer()
    {
        if (player)
            return true;

        audioOutput = std::make_unique<QAudioOutput>();
        player      = std::make_unique<QMediaPlayer>();
        player->setAudioOutput(audioOutput.get());
        QObject::connect(player.get(), &QMediaPlayer::positionChanged, owner, [this](qint64 value) {
            if (reloadAfterSuspend)
                return;
            positionMs = value;
            emit owner->stateChanged();
        });
        QObject::connect(player.get(), &QMediaPlayer::durationChanged, owner, [this](qint64 value) {
            if (reloadAfterSuspend)
                return;
            durationMs = value;
            emit owner->stateChanged();
        });
        QObject::connect(player.get(), &QMediaPlayer::playbackStateChanged, owner,
                         [this](QMediaPlayer::PlaybackState) { emit owner->stateChanged(); });
        QObject::connect(player.get(), &QMediaPlayer::mediaStatusChanged, owner, [this](QMediaPlayer::MediaStatus) {
            const auto status = player->mediaStatus();
            if (status == QMediaPlayer::NoMedia || status == QMediaPlayer::InvalidMedia
                || status == QMediaPlayer::EndOfMedia)
                pendingSeekPosition = -1;
            resumeWhenLoaded();
            emit owner->stateChanged();
        });
        QObject::connect(player.get(), &QMediaPlayer::seekableChanged, owner, [this](bool) { resumeWhenLoaded(); });
        QObject::connect(player.get(), &QMediaPlayer::errorOccurred, owner,
                         [this](QMediaPlayer::Error, const QString &message) {
                             error               = message;
                             pendingSeekPosition = -1;
                             emit owner->stateChanged();
                         });
        return true;
    }

    void watchVideoSink()
    {
        QObject::disconnect(videoFrameConnection);
        videoFrameAvailable = false;
        if (auto *sink = player->videoSink()) {
            videoFrameAvailable = sink->videoFrame().isValid();
            videoFrameConnection
                = QObject::connect(sink, &QVideoSink::videoFrameChanged, owner, [this](const QVideoFrame &frame) {
                      bool changed        = videoFrameAvailable != frame.isValid();
                      videoFrameAvailable = frame.isValid();
                      // setPosition updates the playhead before decoding finishes.
                      // Buffered frames from the old position must not dismiss the spinner.
                      if (pendingSeekPosition >= 0 && frame.isValid()
                          && (frame.startTime() < 0 || qAbs(frame.startTime() / 1000 - pendingSeekPosition) <= 1000)) {
                          pendingSeekPosition = -1;
                          changed             = true;
                      }
                      if (changed)
                          emit owner->stateChanged();
                  });
        } else {
            pendingSeekPosition = -1;
        }
        emit owner->stateChanged();
    }

    void resumeWhenLoaded()
    {
        if (pendingResumePosition < 0 || !player)
            return;
        const auto status = player->mediaStatus();
        if (status != QMediaPlayer::LoadedMedia && status != QMediaPlayer::BufferedMedia)
            return;
        // LoadedMedia can precede seekableChanged with Qt 6.4/GStreamer.
        // QMediaPlayer silently ignores setPosition until seeking is enabled.
        if (pendingResumePosition > 0 && !player->isSeekable())
            return;
        const qint64 resumePosition = pendingResumePosition;
        pendingResumePosition       = -1;
        player->setPosition(resumePosition);
        player->play();
    }
#endif

    ~Impl()
    {
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (player)
            player->setSource(QUrl());
#endif
        if (!remoteUrl.isEmpty())
            MediaRangeService::releaseUrl(remoteUrl);
    }

    bool load(const QString &uri)
    {
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (uri.isEmpty() || !ensurePlayer())
            return false;
        reloadAfterSuspend    = false;
        pendingResumePosition = -1;
        pendingSeekPosition   = -1;
        if (reference.uri() != uri
            || !(reference.mediaType.startsWith(QLatin1String("audio/"))
                 || reference.mediaType.startsWith(QLatin1String("video/")))) {
            error = MediaPlaybackSession::tr("The media source is not available.");
            emit owner->stateChanged();
            return false;
        }
        player->stop();
        player->setSource(QUrl());
        videoFrameAvailable = false;
        stream.reset();
        if (!remoteUrl.isEmpty())
            MediaRangeService::releaseUrl(remoteUrl);
        remoteUrl = QUrl();
        if (!LocalMediaStore::instance()->contains(reference)) {
            const auto url = MediaRangeService::urlFor(reference);
            if (!url.isEmpty()) {
                sourceUri = uri;
                error.clear();
                positionMs = durationMs = 0;
                remoteUrl               = url;
                player->setSource(url);
                emit owner->stateChanged();
                return true;
            }
        }
        stream = std::make_unique<MediaStream>(reference);
        if (!stream->open(QIODevice::ReadOnly)) {
            error = stream->errorString();
            stream.reset();
            emit owner->stateChanged();
            return false;
        }
        sourceUri = uri;
        error.clear();
        positionMs            = 0;
        durationMs            = 0;
        const QUrl formatHint = QUrl::fromLocalFile(QDir(QDir::tempPath()).filePath(reference.portableName));
        player->setSourceDevice(stream.get(), formatHint);
        emit owner->stateChanged();
        return true;
#else
        Q_UNUSED(uri)
        return false;
#endif
    }

    bool play(const QString &uri)
    {
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (reloadAfterSuspend && sourceUri == uri) {
            const qint64 resumePosition = positionMs;
            if (!load(uri))
                return false;
            pendingResumePosition = resumePosition;
            resumeWhenLoaded();
            return true;
        }
        if (sourceUri != uri && !load(uri))
            return false;
        // Some multimedia backends leave EndOfMedia after the player loses
        // focus, while retaining the final position.  Stopped playback with
        // a non-zero position is still a completed/explicitly stopped run and
        // must restart; PausedState intentionally resumes in place.
        if (player->mediaStatus() == QMediaPlayer::EndOfMedia
            || (player->playbackState() == QMediaPlayer::StoppedState && player->position() > 0)) {
            player->setPosition(0);
        }
        player->play();
        return true;
#else
        Q_UNUSED(uri)
        return false;
#endif
    }

    void pause()
    {
        if (pendingResumePosition >= 0) {
            suspend();
            return;
        }
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (player)
            player->pause();
#endif
    }

    void suspend()
    {
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (!player || sourceUri.isEmpty() || reloadAfterSuspend)
            return;
        if (remoteUrl.isEmpty()) {
            pause();
            return;
        }
        // A suspended process can lose XMPP and the decoder's HTTP connection.
        // Reopening on Play uses the authenticated cache and a fresh request.
        const qint64 savedPosition = pendingResumePosition >= 0 ? pendingResumePosition : positionMs;
        const qint64 savedDuration = durationMs;
        pendingResumePosition      = -1;
        pendingSeekPosition        = -1;
        reloadAfterSuspend         = true;
        player->stop();
        player->setSource(QUrl());
        videoFrameAvailable = false;
        positionMs          = savedPosition;
        durationMs          = savedDuration;
        emit owner->stateChanged();
#endif
    }

    void stop()
    {
        videoFrameAvailable   = false;
        pendingResumePosition = -1;
        pendingSeekPosition   = -1;
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (player) {
            player->stop();
            player->setSource(QUrl());
        }
#endif
        stream.reset();
        if (!remoteUrl.isEmpty())
            MediaRangeService::releaseUrl(remoteUrl);
        remoteUrl = QUrl();
        sourceUri.clear();
        reloadAfterSuspend = false;
        positionMs         = 0;
        durationMs         = 0;
        error.clear();
        emit owner->stateChanged();
    }

    MediaPlaybackSession        *owner;
    MediaReference               reference;
    QList<QPointer<QObject>>     outputs;
    QString                      sourceUri;
    QUrl                         remoteUrl;
    QString                      error;
    std::unique_ptr<MediaStream> stream;
    qint64                       positionMs { 0 };
    qint64                       durationMs { 0 };
    bool                         reloadAfterSuspend { false };
    bool                         videoFrameAvailable { false };
    qint64                       pendingResumePosition { -1 };
    qint64                       pendingSeekPosition { -1 };
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    std::unique_ptr<QAudioOutput> audioOutput;
    std::unique_ptr<QMediaPlayer> player;
    QMetaObject::Connection       videoFrameConnection;
#endif
};

namespace {
    QByteArray sourceKey(const MediaReference &reference)
    {
        QByteArray  bytes;
        QDataStream out(&bytes, QIODevice::WriteOnly);
        out << reference.id << reference.blobId << reference.checksum << reference.size << reference.mediaType
            << reference.portableName << reference.remoteData;
        return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    }
    QHash<QByteArray, QPointer<MediaPlaybackSession>> &sessions()
    {
        static QHash<QByteArray, QPointer<MediaPlaybackSession>> registry;
        return registry;
    }
}

MediaPlaybackSession::MediaPlaybackSession(const MediaReference &reference) :
    QObject(QCoreApplication::instance()), impl_(std::make_unique<Impl>(this, reference))
{
}
MediaPlaybackSession::~MediaPlaybackSession() = default;

MediaPlaybackSession *MediaPlaybackSession::acquire(const MediaReference &reference, QObject *borrower)
{
    if (!borrower || !reference.isValid())
        return nullptr;
    const auto key     = sourceKey(reference);
    auto      *session = sessions().value(key).data();
    if (!session) {
        session = new MediaPlaybackSession(reference);
        sessions().insert(key, session);
    }
    if (!session->borrowers_.contains(borrower)) {
        session->borrowers_.insert(borrower);
        QObject::connect(borrower, &QObject::destroyed, session, [session, borrower] { session->release(borrower); });
    }
    return session;
}

void MediaPlaybackSession::release(QObject *borrower)
{
    if (!borrowers_.remove(borrower))
        return;
    QObject::disconnect(borrower, nullptr, this, nullptr);
    if (borrowers_.isEmpty()) {
        const auto key = sourceKey(impl_->reference);
        if (sessions().value(key) == this)
            sessions().remove(key);
        impl_->stop();
        deleteLater();
    }
}

bool MediaPlaybackSession::matches(const MediaReference &reference) const
{
    return sourceKey(impl_->reference) == sourceKey(reference);
}

void MediaPlaybackSession::attachVideoOutput(QObject *output)
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    if (output && impl_->ensurePlayer()) {
        impl_->outputs.removeAll(output);
        impl_->outputs.append(output);
        QObject::disconnect(output, nullptr, this, nullptr);
        connect(output, &QObject::destroyed, this, [this, output] { detachVideoOutput(output); });
        impl_->player->setVideoOutput(output);
        impl_->watchVideoSink();
    }
#else
    Q_UNUSED(output)
#endif
}

void MediaPlaybackSession::detachVideoOutput(QObject *output)
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    impl_->outputs.removeIf([output](const auto &item) { return !item || item == output; });
    auto *target = impl_->outputs.isEmpty() ? nullptr : impl_->outputs.last().data();
    if (impl_->player && impl_->player->videoOutput() != target) {
        impl_->player->setVideoOutput(target);
        impl_->watchVideoSink();
    }
#else
    Q_UNUSED(output)
#endif
}

bool MediaPlaybackSession::available() const
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    return true;
#else
    return false;
#endif
}
QString MediaPlaybackSession::currentSourceUri() const { return impl_->sourceUri; }
bool    MediaPlaybackSession::playing() const
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    return impl_->player && impl_->player->playbackState() == QMediaPlayer::PlayingState;
#else
    return false;
#endif
}
bool MediaPlaybackSession::loading() const
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    if (!impl_->player)
        return false;
    const auto status = impl_->player->mediaStatus();
    return status == QMediaPlayer::LoadingMedia || status == QMediaPlayer::BufferingMedia
        || status == QMediaPlayer::StalledMedia;
#else
    return false;
#endif
}
qint64  MediaPlaybackSession::position() const { return impl_->positionMs; }
bool    MediaPlaybackSession::seeking() const { return impl_->pendingSeekPosition >= 0; }
bool    MediaPlaybackSession::hasVideoFrame() const { return impl_->videoFrameAvailable; }
qint64  MediaPlaybackSession::duration() const { return impl_->durationMs; }
QString MediaPlaybackSession::errorString() const { return impl_->error; }
bool    MediaPlaybackSession::play(const QString &sourceUri) { return impl_->play(sourceUri); }
bool    MediaPlaybackSession::toggle(const QString &sourceUri)
{
    if (playing() && impl_->sourceUri == sourceUri) {
        pause();
        return true;
    }
    return play(sourceUri);
}
void MediaPlaybackSession::pause() { impl_->pause(); }
void MediaPlaybackSession::suspend() { impl_->suspend(); }
void MediaPlaybackSession::stop() { impl_->stop(); }
bool MediaPlaybackSession::seek(const QString &sourceUri, qint64 positionMs)
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    if ((impl_->reloadAfterSuspend || impl_->pendingResumePosition >= 0) && impl_->sourceUri == sourceUri) {
        impl_->positionMs = qMax<qint64>(0, positionMs);
        if (impl_->pendingResumePosition >= 0)
            impl_->pendingResumePosition = impl_->positionMs;
        emit stateChanged();
        return true;
    }
    if (impl_->sourceUri != sourceUri && !impl_->load(sourceUri))
        return false;
    const auto target = impl_->player->duration() > 0 ? qBound<qint64>(0, positionMs, impl_->player->duration())
                                                      : qMax<qint64>(0, positionMs);
    if (target == impl_->player->position())
        return true;
    if (impl_->player->videoSink() && impl_->player->isSeekable()) {
        impl_->pendingSeekPosition = target;
        emit stateChanged();
    }
    impl_->player->setPosition(target);
    return true;
#else
    Q_UNUSED(sourceUri)
    Q_UNUSED(positionMs)
    return false;
#endif
}

} // namespace AnyKeep
