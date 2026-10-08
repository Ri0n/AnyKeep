#ifndef ANYKEEP_MEDIARANGESERVICE_H
#define ANYKEEP_MEDIARANGESERVICE_H

#include "anykeep_export.h"
#include "mediareference.h"
#include <QObject>
#include <QUrl>
#include <functional>

namespace AnyKeep {
// An asynchronous adapter for decoders with HTTP Range support. Readers return
// only authenticated plaintext; a missing range remains pending rather than EOF.
class ANYKEEP_EXPORT MediaRangeService {
public:
    using Completion = std::function<void(QByteArray, QString)>;
    using Reader     = std::function<void(MediaReference, qint64, qint64, Completion)>;
    using Accepts    = std::function<bool(const MediaReference &)>;
    // Register from either thread; accepts captures immutable routing data.
    // Reader runs in owner's thread, completion returns to the adapter thread.
    // urlFor/releaseUrl are called from the application thread.
    static void registerResolver(QObject *owner, Accepts accepts, Reader reader);
    static QUrl urlFor(const MediaReference &reference);
    static void releaseUrl(const QUrl &url);
};
}
#endif
