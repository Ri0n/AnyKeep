#ifndef ANYKEEP_MEDIASOURCE_H
#define ANYKEEP_MEDIASOURCE_H

#include "anykeep_export.h"

#include <QtGlobal>
#include <memory>

class QString;

namespace AnyKeep {

class LocalMediaStore;
struct MediaReference;

// Synchronous random-access byte source consumed by MediaStream. Implementations
// must never return unauthenticated plaintext. A remote implementation may
// block while obtaining and authenticating the requested range.
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
