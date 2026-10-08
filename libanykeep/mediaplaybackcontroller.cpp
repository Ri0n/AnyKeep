#include "mediaplaybackcontroller.h"

#include "localmediastore.h"
#include "mediarangeservice.h"
#include "mediastream.h"
#include "noteblockmodel.h"
#include "noteeditor.h"

#include <QDir>
#include <QUrl>

#include <algorithm>

#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#include <QAudioOutput>
#include <QMediaPlayer>
#endif

namespace AnyKeep {

class MediaPlaybackController::Impl {
public:
    Impl(MediaPlaybackController *q, NoteEditor *editor) : owner(q), editor(editor)
    {
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (editor) {
            QObject::connect(editor, &NoteEditor::mediaChanged, owner, [this](const QList<MediaReference> &media) {
                if (sourceUri.isEmpty())
                    return;
                const bool exists = std::any_of(media.cbegin(), media.cend(),
                                                [this](const MediaReference &item) { return item.uri() == sourceUri; });
                if (!exists)
                    stop();
            });
            if (editor->model()) {
                QObject::connect(editor->model(), &NoteBlockModel::contentsChanged, owner, [this] {
                    if (!sourceUri.isEmpty() && !sourceStillReferenced())
                        stop();
                });
            }
        }
#endif
    }

#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    bool ensurePlayer()
    {
        if (player)
            return true;

        audioOutput = std::make_unique<QAudioOutput>();
        player      = std::make_unique<QMediaPlayer>();
        player->setAudioOutput(audioOutput.get());
        QObject::connect(player.get(), &QMediaPlayer::positionChanged, owner, [this](qint64 value) {
            positionMs = value;
            emit owner->stateChanged();
        });
        QObject::connect(player.get(), &QMediaPlayer::durationChanged, owner, [this](qint64 value) {
            durationMs = value;
            emit owner->stateChanged();
        });
        QObject::connect(player.get(), &QMediaPlayer::playbackStateChanged, owner,
                         [this](QMediaPlayer::PlaybackState) { emit owner->stateChanged(); });
        QObject::connect(player.get(), &QMediaPlayer::mediaStatusChanged, owner,
                         [this](QMediaPlayer::MediaStatus) { emit owner->stateChanged(); });
        QObject::connect(player.get(), &QMediaPlayer::errorOccurred, owner,
                         [this](QMediaPlayer::Error, const QString &message) {
                             error = message;
                             emit owner->stateChanged();
                         });
        return true;
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

    bool sourceStillReferenced() const
    {
        if (!editor || sourceUri.isEmpty() || !editor->model())
            return false;
        const auto *model = editor->model();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex index = model->index(row, 0);
            if (model->data(index, NoteBlockModel::TypeRole).toInt() == NoteBlockModel::Media
                && model->data(index, NoteBlockModel::UrlRole).toString() == sourceUri) {
                return true;
            }
        }
        return false;
    }

    bool load(const QString &uri)
    {
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (!editor || uri.isEmpty() || !ensurePlayer())
            return false;
        const auto media = editor->media();
        const auto it    = std::find_if(media.cbegin(), media.cend(), [&uri](const MediaReference &item) {
            return item.uri() == uri
                && (item.mediaType.startsWith(QLatin1String("audio/"))
                    || item.mediaType.startsWith(QLatin1String("video/")));
        });
        if (it == media.cend()) {
            error = MediaPlaybackController::tr("The timed media is not present in this note.");
            emit owner->stateChanged();
            return false;
        }
        player->stop();
        player->setSource(QUrl());
        stream.reset();
        if (!remoteUrl.isEmpty())
            MediaRangeService::releaseUrl(remoteUrl);
        remoteUrl = QUrl();
        if (!LocalMediaStore::instance()->contains(*it)) {
            const auto url = MediaRangeService::urlFor(*it);
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
        stream = std::make_unique<MediaStream>(*it);
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
        const QUrl formatHint = QUrl::fromLocalFile(QDir(QDir::tempPath()).filePath(it->portableName));
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
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (player)
            player->pause();
#endif
    }

    void stop()
    {
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
        positionMs = 0;
        durationMs = 0;
        error.clear();
        emit owner->stateChanged();
    }

    MediaPlaybackController     *owner;
    NoteEditor                  *editor;
    QString                      sourceUri;
    QUrl                         remoteUrl;
    QString                      error;
    std::unique_ptr<MediaStream> stream;
    qint64                       positionMs { 0 };
    qint64                       durationMs { 0 };
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    std::unique_ptr<QAudioOutput> audioOutput;
    std::unique_ptr<QMediaPlayer> player;
#endif
};

MediaPlaybackController::MediaPlaybackController(NoteEditor *editor, QObject *parent) :
    QObject(parent), impl_(std::make_unique<Impl>(this, editor))
{
}
MediaPlaybackController::~MediaPlaybackController() = default;

void MediaPlaybackController::attachVideoOutput(QObject *output)
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    if (output && impl_->ensurePlayer())
        impl_->player->setVideoOutput(output);
#else
    Q_UNUSED(output)
#endif
}

void MediaPlaybackController::detachVideoOutput(QObject *output)
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    if (impl_->player && impl_->player->videoOutput() == output)
        impl_->player->setVideoOutput(nullptr);
#else
    Q_UNUSED(output)
#endif
}

bool MediaPlaybackController::available() const
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    return true;
#else
    return false;
#endif
}
QString MediaPlaybackController::currentSourceUri() const { return impl_->sourceUri; }
bool    MediaPlaybackController::playing() const
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    return impl_->player && impl_->player->playbackState() == QMediaPlayer::PlayingState;
#else
    return false;
#endif
}
bool MediaPlaybackController::loading() const
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
qint64  MediaPlaybackController::position() const { return impl_->positionMs; }
qint64  MediaPlaybackController::duration() const { return impl_->durationMs; }
QString MediaPlaybackController::errorString() const { return impl_->error; }
bool    MediaPlaybackController::play(const QString &sourceUri) { return impl_->play(sourceUri); }
bool    MediaPlaybackController::toggle(const QString &sourceUri)
{
    if (playing() && impl_->sourceUri == sourceUri) {
        pause();
        return true;
    }
    return play(sourceUri);
}
void MediaPlaybackController::pause() { impl_->pause(); }
void MediaPlaybackController::stop() { impl_->stop(); }
bool MediaPlaybackController::seek(const QString &sourceUri, qint64 positionMs)
{
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    if (impl_->sourceUri != sourceUri && !impl_->load(sourceUri))
        return false;
    impl_->player->setPosition(qMax<qint64>(0, positionMs));
    return true;
#else
    Q_UNUSED(sourceUri)
    Q_UNUSED(positionMs)
    return false;
#endif
}

} // namespace AnyKeep
