#include "conflictresolver.h"

#include <QCoreApplication>
#include <QLocale>

#include <utility>

namespace AnyKeep {

void CopyConflictResolver::resolve(NoteConflict conflict, Completion completion)
{
    const auto timestamp = QLocale().toString(conflict.localDraft.updatedAt.toLocalTime(), QLocale::ShortFormat);
    ConflictResolution resolution;
    resolution.action    = ConflictResolution::CreateCopy;
    resolution.copyTitle = QCoreApplication::translate("CopyConflictResolver", "%1 (conflict %2)")
                               .arg(conflict.localDraft.title, timestamp);
    resolution.notification
        = QCoreApplication::translate(
              "CopyConflictResolver",
              "The note was changed on another device. Your version will be saved as a separate copy: %1")
              .arg(resolution.copyTitle);
    completion(std::move(resolution));
}

} // namespace AnyKeep
