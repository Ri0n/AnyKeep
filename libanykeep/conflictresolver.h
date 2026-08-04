#ifndef CONFLICTRESOLVER_H
#define CONFLICTRESOLVER_H

#include "anykeep_export.h"
#include "draftstore.h"

#include <functional>

namespace AnyKeep {

/** Context available to a note conflict resolution policy. */
struct ANYKEEP_EXPORT NoteConflict {
    DraftRecord localDraft;
    Note        remoteNote;
    QString     message;
};

/** Decision returned by a ConflictResolver. */
struct ANYKEEP_EXPORT ConflictResolution {
    enum Action {
        CreateCopy, ///< Publish the local draft as a new note with a new ID.
        KeepDraft,  ///< Preserve the draft for a future/manual decision.
        Discard     ///< Accept the remote version and discard the local draft.
    };

    Action  action { KeepDraft };
    QString copyTitle;
    QString notification;
};

/**
 * @brief Policy interface separating conflict detection from user-visible resolution.
 *
 * Implementations may complete synchronously or asynchronously. The callback
 * must be invoked exactly once while the resolver remains alive.
 */
class ANYKEEP_EXPORT ConflictResolver {
public:
    using Completion = std::function<void(ConflictResolution)>;

    virtual ~ConflictResolver()                                        = default;
    virtual void resolve(NoteConflict conflict, Completion completion) = 0;
};

/** Default lossless policy: retain the remote note and publish local text as a copy. */
class ANYKEEP_EXPORT CopyConflictResolver final : public ConflictResolver {
public:
    void resolve(NoteConflict conflict, Completion completion) override;
};

} // namespace AnyKeep

#endif // CONFLICTRESOLVER_H
