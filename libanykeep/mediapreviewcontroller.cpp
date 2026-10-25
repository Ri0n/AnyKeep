#include "mediapreviewcontroller.h"

#include "localmediastore.h"
#include "mediarangeservice.h"
#include "mediasource.h"
#include "mediastream.h"
#include "noteblockmodel.h"
#include "noteeditor.h"

#include <QBuffer>
#include <QDir>
#include <QHash>
#include <QImage>
#include <QTimer>
#include <algorithm>
#include <utility>

#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#include <QMediaPlayer>
#include <QVideoFrame>
#include <QVideoSink>
#endif

namespace AnyKeep {
class MediaPreviewController::Impl {
public:
    Impl(MediaPreviewController *owner, NoteEditor *editor, LocalMediaStore *store) :
        owner(owner), editor(editor), store(store ? store : LocalMediaStore::instance())
    {
        next.setSingleShot(true);
        timeout.setSingleShot(true);
        timeout.setInterval(10000);
        QObject::connect(&next, &QTimer::timeout, owner, [this] { startNext(); });
        QObject::connect(&timeout, &QTimer::timeout, owner, [this] { finish(); });
        QObject::connect(editor, &NoteEditor::mediaChanged, owner, [this] { prune(); });
        QObject::connect(editor->model(), &NoteBlockModel::contentsChanged, owner, [this] { prune(); });
    }

    ~Impl() { releaseDecoder(); }

    bool referenced(const MediaReference &reference) const
    {
        if (!editor->media().contains(reference))
            return false;
        const auto *model = editor->model();
        for (int row = 0; row < model->rowCount(); ++row)
            if (model->blockTypeAt(row) == NoteBlockModel::Media
                && model->data(model->index(row), NoteBlockModel::UrlRole).toString() == reference.uri())
                return true;
        return false;
    }

    void prune()
    {
        for (auto it = requested.begin(); it != requested.end();) {
            if (!referenced(it.value()))
                it = requested.erase(it);
            else
                ++it;
        }
        bool changed = false;
        for (auto it = posters.begin(); it != posters.end();) {
            if (!referenced(it->reference)) {
                requested.remove(it.key());
                it      = posters.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        posterOrder.removeIf([this](const QString &uri) { return !posters.contains(uri); });
        queue.removeIf([this](const MediaReference &reference) {
            if (referenced(reference))
                return false;
            requested.remove(reference.uri());
            return true;
        });
        if (!active.id.isNull() && !referenced(active)) {
            requested.remove(active.uri());
            finish();
        }
        if (changed) {
            ++revision;
            emit owner->changed();
        }
    }

    void request(const QString &uri, bool retry)
    {
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (active.uri() == uri) {
            retryActive = retryActive || retry;
            return;
        }
        if (posters.contains(uri) || (!retry && requested.contains(uri)))
            return;
        const auto media = editor->media();
        const auto it    = std::find_if(media.cbegin(), media.cend(), [&](const MediaReference &reference) {
            return reference.uri() == uri && reference.mediaType.startsWith(QLatin1String("video/"));
        });
        if (it == media.cend() || !referenced(*it) || queue.contains(*it))
            return;
        requested.insert(uri, *it);
        queue.append(*it);
        if (active.id.isNull())
            next.start(0);
#else
        Q_UNUSED(uri)
        Q_UNUSED(retry)
#endif
    }

    void releaseDecoder()
    {
        timeout.stop();
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (player) {
            QObject::disconnect(player.get(), nullptr, owner, nullptr);
            QObject::disconnect(sink.get(), nullptr, owner, nullptr);
            player->stop();
            player->setSource(QUrl());
            player.reset();
            sink.reset();
        }
#endif
        stream.reset();
        if (!remoteUrl.isEmpty())
            MediaRangeService::releaseUrl(std::exchange(remoteUrl, {}));
    }

    void finish(const QImage &image = {})
    {
        const auto completed = active;
        const bool retry     = std::exchange(retryActive, false);
        ++probeGeneration;
        releaseDecoder();
        active = {};
        if (!image.isNull() && referenced(completed)) {
            QByteArray bytes;
            QBuffer    buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            if (image.save(&buffer, "PNG")) {
                constexpr int MaxPosters = 32;
                while (posters.size() >= MaxPosters) {
                    const auto oldest = posterOrder.takeFirst();
                    posters.remove(oldest);
                    requested.remove(oldest);
                }
                posters.insert(
                    completed.uri(),
                    { completed,
                      QUrl(QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64())) });
                posterOrder.append(completed.uri());
                ++revision;
                emit owner->changed();
            }
        }
        if (retry && image.isNull())
            request(completed.uri(), true);
        if (!queue.isEmpty())
            next.start(0);
    }

    void startNext()
    {
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
        if (!active.id.isNull() || queue.isEmpty())
            return;
        active = queue.takeFirst();
        if (!referenced(active)) {
            finish();
            return;
        }
        // One bounded, silent probe at a time. Use the same authenticated
        // source/range adapter as playback; never create plaintext temp files.
        if (!store->contains(active))
            remoteUrl = MediaRangeService::urlFor(active);
        if (remoteUrl.isEmpty()) {
            stream = std::make_unique<MediaStream>(createLocalMediaSource(active, store));
            if (!stream->open(QIODevice::ReadOnly)) {
                finish();
                return;
            }
        }
        sink   = std::make_unique<QVideoSink>();
        player = std::make_unique<QMediaPlayer>();
        player->setVideoSink(sink.get());
        // Deliberately omit QAudioOutput: extracting a poster is silent.
        const auto generation = ++probeGeneration;
        QObject::connect(sink.get(), &QVideoSink::videoFrameChanged, owner,
                         [this, generation](const QVideoFrame &frame) {
                             if (generation != probeGeneration || !frame.isValid() || completing)
                                 return;
                             auto image = frame.toImage();
                             if (image.isNull())
                                 return;
                             if (image.width() > 640 || image.height() > 480)
                                 image = image.scaled(640, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                             completing = true;
                             QTimer::singleShot(0, owner, [this, generation, image] {
                                 if (generation == probeGeneration)
                                     finish(image);
                             });
                         });
        QObject::connect(player.get(), &QMediaPlayer::errorOccurred, owner, [this, generation] {
            QTimer::singleShot(0, owner, [this, generation] {
                if (generation == probeGeneration)
                    finish();
            });
        });
        completing = false;
        timeout.start();
        if (!remoteUrl.isEmpty()) {
            player->setSource(remoteUrl);
        } else {
            player->setSourceDevice(stream.get(),
                                    QUrl::fromLocalFile(QDir(QDir::tempPath()).filePath(active.portableName)));
        }
        player->play();
#endif
    }

    struct Poster {
        MediaReference reference;
        QUrl           url;
    };
    MediaPreviewController        *owner;
    NoteEditor                    *editor;
    LocalMediaStore               *store;
    QHash<QString, Poster>         posters;
    QHash<QString, MediaReference> requested;
    QStringList                    posterOrder;
    QList<MediaReference>          queue;
    MediaReference                 active;
    QTimer                         next, timeout;
    QUrl                           remoteUrl;
    std::unique_ptr<MediaStream>   stream;
    int                            revision { 0 };
    quint64                        probeGeneration { 0 };
    bool                           completing { false };
    bool                           retryActive { false };
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    std::unique_ptr<QMediaPlayer> player;
    std::unique_ptr<QVideoSink>   sink;
#endif
};

MediaPreviewController::MediaPreviewController(NoteEditor *editor, QObject *parent, LocalMediaStore *store) :
    QObject(parent), impl_(std::make_unique<Impl>(this, editor, store))
{
}
MediaPreviewController::~MediaPreviewController() = default;
int  MediaPreviewController::revision() const { return impl_->revision; }
QUrl MediaPreviewController::posterForUri(const QString &uri) const { return impl_->posters.value(uri).url; }
void MediaPreviewController::requestPreview(const QString &uri, bool retry) { impl_->request(uri, retry); }
} // namespace AnyKeep
