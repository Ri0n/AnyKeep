#ifndef ANYKEEP_MEDIASOURCE_H
#define ANYKEEP_MEDIASOURCE_H

#include "anykeep_export.h"

#include <QtGlobal>
#include <memory>

class QString;

namespace AnyKeep {

class LocalMediaStore;
struct MediaReference;

// Random-access source for bytes that are already available and authenticated.
// Implementations must never return unauthenticated plaintext. Network range
// acquisition is asynchronous cache hydration and must not turn read() into a
// blocking remote fetch; MediaStream/cache orchestration retries after data
// becomes available.
class ANYKEEP_EXPORT MediaSource {
public:
    virtual ~MediaSource() = default;

    virtual bool   open(QString *error) = 0;
    virtual void   close() = 0;
    virtual qint64 size() const = 0;
    virtual qint64 read(qint64 offset, char *data, qint64 maxSize, QString *error) = 0;
};

// Transitional source for the current whole-object authenticated local store.
// Passing a store explicitly keeps the stream testable and leaves MediaStream
// independent of LocalMediaStore::instance().
ANYKEEP_EXPORT std::unique_ptr<MediaSource> createLocalMediaSource(const MediaReference &reference,
                                                                   LocalMediaStore *store = nullptr);

} // namespace AnyKeep

#endif // ANYKEEP_MEDIASOURCE_H
