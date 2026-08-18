#ifndef UPDATESESSIONMANAGER_H
#define UPDATESESSIONMANAGER_H

#include <QList>
#include <QRect>
#include <QString>
#include <QUuid>

#include <optional>

namespace AnyKeep {

struct UpdateSessionNoteWindow {
    QString storageId;
    QString noteId;
    QUuid   draftId;
    QRect   geometry;
};

struct UpdateSessionState {
    QString                        sourceVersion;
    QString                        targetVersion;
    QList<UpdateSessionNoteWindow> noteWindows;
    bool                           noteManagerVisible { false };
    QRect                          noteManagerGeometry;
    QString                        noteManagerStorageId;
    QString                        noteManagerNoteId;
    QUuid                          noteManagerDraftId;
    bool                           restoreNoteManagerNote { false };
    bool                           optionsVisible { false };
    QRect                          optionsGeometry;
};

class UpdateSessionManager final {
public:
    std::optional<UpdateSessionState> pendingSession() const;
    bool                              savePendingSession(const UpdateSessionState &session) const;
    void                              clearPendingSession() const;

    // Stores the version observed at normal application startup and returns
    // the previously recorded version. An empty return value means
    // there is no earlier run to compare with (for example the first install).
    QString recordStartedVersion(const QString &currentVersion) const;
};

} // namespace AnyKeep

#endif // UPDATESESSIONMANAGER_H
