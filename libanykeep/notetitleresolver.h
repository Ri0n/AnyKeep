#ifndef NOTETITLERESOLVER_H
#define NOTETITLERESOLVER_H

#include "anykeep_export.h"
#include "note.h"

#include <QString>

namespace AnyKeep::NoteTitleResolver {

inline constexpr auto CachedDisplayTitleBackendKey = "anykeep.displayTitle";

ANYKEEP_EXPORT QString displayTitle(const QString &title, const QString &body, Note::Format format);

} // namespace AnyKeep::NoteTitleResolver

#endif // NOTETITLERESOLVER_H
