#ifndef NOTEFRAGMENTMEDIATRANSFER_H
#define NOTEFRAGMENTMEDIATRANSFER_H

#include "notefragment.h"
#include "anykeep_export.h"

#include <QList>
#include <QString>

namespace AnyKeep {

class LocalMediaStore;

// Prepares a copied fragment for a different destination note. It is separate
// from NoteTransferController because resolving blobs is storage I/O, while
// MIME conversion must remain side-effect free.
class ANYKEEP_EXPORT NoteFragmentMediaTransfer {
public:
    struct Result {
        NoteFragment          fragment;
        QList<MediaReference> importedMedia;
        QString               error;

        explicit operator bool() const { return error.isEmpty(); }
    };

    // `sourceStore` may be null only when every referenced media item embeds
    // data. Destination attachment UUIDs are fresh; blob storage may still
    // deduplicate the immutable bytes.
    static Result cloneForDestination(const NoteFragment &fragment, LocalMediaStore &destination,
                                      const LocalMediaStore *sourceStore = nullptr);
};

} // namespace AnyKeep

#endif // NOTEFRAGMENTMEDIATRANSFER_H
