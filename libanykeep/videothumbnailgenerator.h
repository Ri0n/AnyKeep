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

// Optional, best-effort video preview. It never changes the original media.
// The callback is invoked on the caller's Qt event-loop thread, at most once.
class ANYKEEP_EXPORT VideoThumbnailGenerator {
public:
    using Completion = std::function<void(qint64 positionMs, QByteArray jpeg)>;

    static void start(const MediaReference &reference, QObject *lifetime, Completion completion);
    static QVector<qint64> candidatePositions(qint64 durationMs);
    static QByteArray encodeTinyJpeg(const QImage &image);
};

} // namespace AnyKeep

#endif
