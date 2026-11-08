#ifndef ANYKEEP_VIDEO_THUMBNAIL_GENERATOR_H
#define ANYKEEP_VIDEO_THUMBNAIL_GENERATOR_H

#include "anykeep_export.h"

#include <QByteArray>
#include <QImage>
#include <QVector>

#include <functional>

class QObject;

namespace AnyKeep {

struct MediaReference;
class LocalMediaStore;

// Optional, best-effort video preview. It never changes the original media.
// The callback is invoked on the caller's Qt event-loop thread, at most once.
class ANYKEEP_EXPORT VideoThumbnailGenerator {
public:
    using Completion = std::function<void(qint64 positionMs, QByteArray tinyJpeg, QByteArray posterBlobId)>;

    // An explicit store must outlive the request; normal callers use the
    // profile-wide store. Local byte reads and derivation run off the UI thread.
    static void            start(const MediaReference &reference, QObject *lifetime, Completion completion,
                                 LocalMediaStore *store = nullptr);
    static QVector<qint64> candidatePositions(qint64 durationMs);
    static QByteArray      encodeTinyJpeg(const QImage &image);
    static QByteArray      encodePosterJpeg(const QImage &image);
};

} // namespace AnyKeep

#endif
