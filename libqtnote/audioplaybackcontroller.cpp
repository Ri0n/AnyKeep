#include "audioplaybackcontroller.h"

#include "localmediastore.h"
#include "noteblockmodel.h"
#include "noteeditor.h"

#include <QBuffer>
#include <QDir>
#include <QUrl>

#include <algorithm>

#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#include <QAudioOutput>
#include <QMediaPlayer>
#endif

namespace QtNote {

class AudioPlaybackController::Impl {
public:
    Impl(AudioPlaybackController *q, NoteEditor *editor) : owner(q), editor(editor)
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        buffer.setBuffer(&bytes);
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
#endif
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
    }

    bool sourceStillReferenced() const
    {
        if (!editor || sourceUri.isEmpty() || !editor->model())
            return false;
        const auto *model = editor->model();
        for (int row = 0; row < model->rowCount(); ++row) {
            const QModelIndex index = model->index(row, 0);
            if (model->data(index, NoteBlockModel::TypeRole).toInt() == NoteBlockModel::Audio
                && model->data(index, NoteBlockModel::UrlRole).toString() == sourceUri) {
                return true;
            }
        }
        return false;
    }

    bool load(const QString &uri)
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (!editor || !player || uri.isEmpty())
            return false;
        const auto media = editor->media();
        const auto it    = std::find_if(media.cbegin(), media.cend(), [&uri](const MediaReference &item) {
            return item.uri() == uri && item.mediaType.startsWith(QLatin1String("audio/"));
        });
        if (it == media.cend()) {
            error = AudioPlaybackController::tr("The audio attachment is not present in this note.");
            emit owner->stateChanged();
            return false;
        }
        const auto decrypted = LocalMediaStore::instance()->data(it->blobId);
        if (!decrypted) {
            error = decrypted.error;
            emit owner->stateChanged();
            return false;
        }

        player->stop();
        buffer.close();
        bytes = decrypted.value;
        if (!buffer.open(QIODevice::ReadOnly)) {
            error = buffer.errorString();
            emit owner->stateChanged();
            return false;
        }
        sourceUri = uri;
        error.clear();
        positionMs            = 0;
        durationMs            = 0;
        const QUrl formatHint = QUrl::fromLocalFile(QDir(QDir::tempPath()).filePath(it->portableName));
        player->setSourceDevice(&buffer, formatHint);
        emit owner->stateChanged();
        return true;
#else
        Q_UNUSED(uri)
        return false;
#endif
    }

    bool play(const QString &uri)
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (sourceUri != uri && !load(uri))
            return false;
        if (player->mediaStatus() == QMediaPlayer::EndOfMedia)
            player->setPosition(0);
        player->play();
        return true;
#else
        Q_UNUSED(uri)
        return false;
#endif
    }

    void pause()
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (player)
            player->pause();
#endif
    }

    void stop()
    {
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (player) {
            player->stop();
            player->setSource(QUrl());
        }
#endif
        buffer.close();
        bytes.clear();
        sourceUri.clear();
        positionMs = 0;
        durationMs = 0;
        error.clear();
        emit owner->stateChanged();
    }

    AudioPlaybackController *owner;
    NoteEditor              *editor;
    QString                  sourceUri;
    QString                  error;
    QByteArray               bytes;
    QBuffer                  buffer;
    qint64                   positionMs { 0 };
    qint64                   durationMs { 0 };
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    std::unique_ptr<QAudioOutput> audioOutput;
    std::unique_ptr<QMediaPlayer> player;
#endif
};

AudioPlaybackController::AudioPlaybackController(NoteEditor *editor, QObject *parent) :
    QObject(parent), impl_(std::make_unique<Impl>(this, editor))
{
}
AudioPlaybackController::~AudioPlaybackController() = default;

bool AudioPlaybackController::available() const
{
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    return true;
#else
    return false;
#endif
}
QString AudioPlaybackController::currentSourceUri() const { return impl_->sourceUri; }
bool    AudioPlaybackController::playing() const
{
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    return impl_->player && impl_->player->playbackState() == QMediaPlayer::PlayingState;
#else
    return false;
#endif
}
bool AudioPlaybackController::loading() const
{
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    if (!impl_->player)
        return false;
    const auto status = impl_->player->mediaStatus();
    return status == QMediaPlayer::LoadingMedia || status == QMediaPlayer::BufferingMedia
        || status == QMediaPlayer::StalledMedia;
#else
    return false;
#endif
}
qint64  AudioPlaybackController::position() const { return impl_->positionMs; }
qint64  AudioPlaybackController::duration() const { return impl_->durationMs; }
QString AudioPlaybackController::errorString() const { return impl_->error; }
bool    AudioPlaybackController::play(const QString &sourceUri) { return impl_->play(sourceUri); }
bool    AudioPlaybackController::toggle(const QString &sourceUri)
{
    if (playing() && impl_->sourceUri == sourceUri) {
        pause();
        return true;
    }
    return play(sourceUri);
}
void AudioPlaybackController::pause() { impl_->pause(); }
void AudioPlaybackController::stop() { impl_->stop(); }
bool AudioPlaybackController::seek(const QString &sourceUri, qint64 positionMs)
{
#if defined(QTNOTE_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
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

} // namespace QtNote
