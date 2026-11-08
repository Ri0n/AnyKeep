#include "videothumbnailgenerator.h"

#include "localmediastore.h"
#include "mediareference.h"
#include "mediasource.h"
#include "mediastream.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QQueue>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#include <QMediaPlayer>
#include <QVideoFrame>
#include <QVideoSink>
#endif

namespace AnyKeep {
namespace {

    double frameQuality(const QImage &source)
    {
        if (source.isNull())
            return -std::numeric_limits<double>::infinity();

        const QImage sample = source.scaled(QSize(96, 54), Qt::KeepAspectRatio, Qt::FastTransformation)
                                  .convertToFormat(QImage::Format_RGB32);
        if (sample.isNull())
            return -std::numeric_limits<double>::infinity();

        double    sum        = 0.0;
        double    sumSquares = 0.0;
        double    edges      = 0.0;
        const int pixels     = sample.width() * sample.height();
        for (int y = 0; y < sample.height(); ++y) {
            for (int x = 0; x < sample.width(); ++x) {
                const int luminance = qGray(sample.pixel(x, y));
                sum += luminance;
                sumSquares += luminance * luminance;
                if (x)
                    edges += std::abs(luminance - qGray(sample.pixel(x - 1, y)));
            }
        }
        const double mean     = sum / pixels;
        const double variance = std::max(0.0, sumSquares / pixels - mean * mean);
        const double detail   = edges / pixels;

        // Blank intros and fades score below informative frames. An entirely
        // dark video still retains its best *valid* frame.
        const double nearlyBlankPenalty = mean < 18.0 || mean > 245.0 ? 1000.0 : 0.0;
        return variance + detail * 8.0 - nearlyBlankPenalty;
    }

#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    class ThumbnailJob final : public QObject {
    public:
        ThumbnailJob(MediaReference reference, LocalMediaStore *store, VideoThumbnailGenerator::Completion completion,
                     QObject *parent) :
            QObject(parent), reference_(std::move(reference)), store_(store), completion_(std::move(completion))
        {
            stepTimer_.setSingleShot(true);
            connect(&stepTimer_, &QTimer::timeout, this, [this] { nextCandidate(); });
            connect(&player_, &QMediaPlayer::durationChanged, this, [this](qint64) { maybeStart(); });
            connect(&player_, &QMediaPlayer::seekableChanged, this, [this](bool) { maybeStart(); });
            connect(&player_, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::InvalidMedia || status == QMediaPlayer::NoMedia) {
                    finish();
                    return;
                }
                if (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia)
                    maybeStart();
            });
            connect(&sink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
                if (finished_ || !sampling_ || frameCaptured_ || !frame.isValid())
                    return;
                const auto   timestamp = frame.startTime() >= 0 ? frame.startTime() / 1000 : player_.position();
                const qint64 target    = candidates_.at(index_);
                if (target > 0 && std::abs(timestamp - target) > 1250)
                    return;
                if (timestamp < 0 || (player_.duration() > 0 && timestamp > player_.duration() / 2)
                    || frame.width() > 4096 || frame.height() > 4096)
                    return;
                QImage image = frame.toImage();
                if (image.isNull() || image.width() > 4096 || image.height() > 4096)
                    return;
                const double score = frameQuality(image);
                if (score > bestScore_) {
                    bestScore_   = score;
                    bestImage_   = std::move(image);
                    bestFrameMs_ = timestamp >= 0 ? timestamp : target;
                }
                frameCaptured_ = true;
                stepTimer_.stop();
                player_.pause();
                QTimer::singleShot(0, this, [this] { nextCandidate(); });
            });
        }

        ~ThumbnailJob() override
        {
            // Cancellation and decoder shutdown never invoke the completion.
            finished_ = true;
            QObject::disconnect(&player_, nullptr, this, nullptr);
            QObject::disconnect(&sink_, nullptr, this, nullptr);
            player_.stop();
            player_.setSource(QUrl());
            player_.setVideoSink(nullptr);
        }

        void begin()
        {
            stream_ = std::make_unique<MediaStream>(createLocalMediaSource(reference_, store_));
            if (!stream_->open(QIODevice::ReadOnly)) {
                finish();
                return;
            }
            player_.setVideoSink(&sink_);
            const auto nameHint = QUrl::fromLocalFile(QDir(QDir::tempPath()).filePath(reference_.portableName));
            player_.setSourceDevice(stream_.get(), nameHint);
            // Metadata/decoder failures must never hold an editor or a publication.
            QTimer::singleShot(9000, this, [this] { finish(); });
        }

    private:
        void maybeStart()
        {
            if (finished_ || sampling_)
                return;
            const auto status = player_.mediaStatus();
            if (status != QMediaPlayer::LoadedMedia && status != QMediaPlayer::BufferedMedia)
                return;
            // On GStreamer, LoadedMedia can precede both duration and seekability.
            // Give the backend a brief opportunity to expose the actual timeline.
            if ((player_.duration() <= 0 || !player_.isSeekable()) && !metadataFallbackScheduled_) {
                metadataFallbackScheduled_ = true;
                QTimer::singleShot(700, this, [this] {
                    if (!finished_ && !sampling_)
                        beginSampling();
                });
                return;
            }
            if (player_.duration() > 0 && player_.isSeekable())
                beginSampling();
        }

        void beginSampling()
        {
            if (finished_ || sampling_)
                return;
            candidates_ = player_.duration() > 0 && player_.isSeekable()
                ? VideoThumbnailGenerator::candidatePositions(player_.duration())
                : QVector<qint64> { 0 };
            sampling_   = true;
            index_      = -1;
            nextCandidate();
        }

        void nextCandidate()
        {
            if (finished_ || !sampling_)
                return;
            if (++index_ >= candidates_.size()) {
                finish();
                return;
            }
            frameCaptured_ = false;
            player_.setPosition(candidates_.at(index_));
            player_.play();
            stepTimer_.start(1800);
        }

        void finish()
        {
            if (finished_)
                return;
            finished_ = true;
            stepTimer_.stop();
            player_.stop();
            const auto tiny   = VideoThumbnailGenerator::encodeTinyJpeg(bestImage_);
            const auto poster = tiny.isEmpty() ? QByteArray {} : VideoThumbnailGenerator::encodePosterJpeg(bestImage_);
            QByteArray posterBlobId;
            if (!poster.isEmpty()) {
                const auto stored
                    = store_->importData(poster, QStringLiteral("video-poster.jpg"), QStringLiteral("image/jpeg"));
                if (stored)
                    posterBlobId = stored.value.blobId;
            }
            if (completion_)
                completion_(tiny.isEmpty() ? -1 : bestFrameMs_, tiny, posterBlobId);
            deleteLater();
        }

        MediaReference                      reference_;
        LocalMediaStore                    *store_;
        VideoThumbnailGenerator::Completion completion_;
        std::unique_ptr<MediaStream>        stream_;
        QVideoSink                          sink_;
        QMediaPlayer                        player_;
        QTimer                              stepTimer_;
        QVector<qint64>                     candidates_;
        QImage                              bestImage_;
        double                              bestScore_                 = -std::numeric_limits<double>::infinity();
        qint64                              bestFrameMs_               = -1;
        qsizetype                           index_                     = -1;
        bool                                sampling_                  = false;
        bool                                metadataFallbackScheduled_ = false;
        bool                                frameCaptured_             = false;
        bool                                finished_                  = false;
    };

    class ThumbnailQueue final : public QObject {
    public:
        explicit ThumbnailQueue(QObject *parent) : QObject(parent) { decoderThread_.start(); }

        ~ThumbnailQueue() override
        {
            decoderThread_.quit();
            decoderThread_.wait();
        }

        void enqueue(MediaReference reference, QObject *lifetime, VideoThumbnailGenerator::Completion completion,
                     LocalMediaStore *store)
        {
            pending_.enqueue({ std::move(reference), QPointer<QObject>(lifetime), std::move(completion), store });
            pump();
        }

    private:
        struct Request {
            MediaReference                      reference;
            QPointer<QObject>                   lifetime;
            VideoThumbnailGenerator::Completion completion;
            LocalMediaStore                    *store;
        };

        void pump()
        {
            // Keep decoding independent of foreground playback and avoid a burst
            // of simultaneous decoders when several videos are inserted together.
            if (active_ >= 1)
                return;
            while (!pending_.isEmpty()) {
                auto request = pending_.dequeue();
                if (!request.lifetime)
                    continue;
                ++active_;
                auto *context = new QObject;
                context->moveToThread(&decoderThread_);
                connect(request.lifetime, &QObject::destroyed, context, &QObject::deleteLater);
                connect(&decoderThread_, &QThread::finished, context, &QObject::deleteLater);
                connect(context, &QObject::destroyed, this, [this] {
                    --active_;
                    QTimer::singleShot(0, this, [this] { pump(); });
                });
                QTimer::singleShot(0, context, [this, context, request = std::move(request)]() mutable {
                    // Construct the player and all backend-owned decoder objects on
                    // the worker too; moving only QMediaPlayer can leave private
                    // backend objects on the UI thread.
                    auto *job = new ThumbnailJob(
                        std::move(request.reference), request.store,
                        [this, guard = request.lifetime, done = std::move(request.completion)](
                            qint64 frameMs, QByteArray tiny, QByteArray posterBlobId) {
                            QMetaObject::invokeMethod(
                                this,
                                [guard, done, frameMs, tiny, posterBlobId] {
                                    if (guard)
                                        done(frameMs, tiny, posterBlobId);
                                },
                                Qt::QueuedConnection);
                        },
                        context);
                    connect(job, &QObject::destroyed, context, &QObject::deleteLater);
                    job->begin();
                });
                return;
            }
        }

        QQueue<Request> pending_;
        int             active_ = 0;
        QThread         decoderThread_;
    };
#endif

} // namespace

QVector<qint64> VideoThumbnailGenerator::candidatePositions(qint64 durationMs)
{
    if (durationMs <= 0)
        return { 0 };
    const qint64    maximum = durationMs / 2;
    QVector<qint64> positions;
    for (const qint64 preferred : { qint64(2000), qint64(4000), qint64(500), qint64(5000) }) {
        const qint64 bounded = std::min(preferred, maximum);
        if (!positions.contains(bounded))
            positions.append(bounded);
    }
    return positions;
}

QByteArray VideoThumbnailGenerator::encodeTinyJpeg(const QImage &image)
{
    if (image.isNull())
        return {};
    for (const int width : { 240, 200, 160, 120, 96 }) {
        const auto scaled
            = image.scaled(image.size().boundedTo(QSize(width, width)), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        for (const int quality : { 75, 60, 45, 30 }) {
            QByteArray encoded;
            QBuffer    buffer(&encoded);
            if (!buffer.open(QIODevice::WriteOnly) || !scaled.save(&buffer, "JPEG", quality))
                return {};
            if (encoded.size() <= MediaReference::MaxVideoThumbnailBytes)
                return encoded;
        }
    }
    return {};
}

QByteArray VideoThumbnailGenerator::encodePosterJpeg(const QImage &image)
{
    if (image.isNull())
        return {};
    // Keep an independent, good-quality poster in the encrypted media store.
    // Bound both the dimensions and encoded bytes to avoid oversized sidecars.
    for (const int width : { 640, 480, 360, 240 }) {
        const auto scaled
            = image.scaled(image.size().boundedTo(QSize(width, width)), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        for (const int quality : { 85, 75, 60, 45 }) {
            QByteArray encoded;
            QBuffer    buffer(&encoded);
            if (!buffer.open(QIODevice::WriteOnly) || !scaled.save(&buffer, "JPEG", quality))
                return {};
            if (encoded.size() <= 128 * 1024)
                return encoded;
        }
    }
    return {};
}

void VideoThumbnailGenerator::start(const MediaReference &reference, QObject *lifetime, Completion completion,
                                    LocalMediaStore *store)
{
    if (!lifetime || !completion)
        return;
#if defined(ANYKEEP_MULTIMEDIA_AVAILABLE) && QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    auto *resolvedStore = store ? store : LocalMediaStore::instance();
    if (!resolvedStore->initialize()) {
        completion(-1, {}, {});
        return;
    }
    static ThumbnailQueue *queue = new ThumbnailQueue(QCoreApplication::instance());
    queue->enqueue(reference, lifetime, std::move(completion), resolvedStore);
#else
    Q_UNUSED(reference)
    Q_UNUSED(store)
    completion(-1, {}, {});
#endif
}

} // namespace AnyKeep
