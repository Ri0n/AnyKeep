#ifndef ANYKEEP_FOLDERS_PRIVATE_H
#define ANYKEEP_FOLDERS_PRIVATE_H

#include "foldercatalog.h"

namespace AnyKeep::FolderCatalogPrivate {

inline FolderCatalogError error(FolderCatalogError::Code code, const QString &message) { return { code, message }; }

inline QString folderKey(const QUuid &parentId, const QString &name)
{
    return parentId.toString(QUuid::WithoutBraces) + QChar(0x1f) + name;
}

inline QString assignmentKey(const QString &storageId, const QString &noteId)
{
    return QString::number(storageId.size()) + QChar(0x1f) + storageId + noteId;
}

} // namespace AnyKeep::FolderCatalogPrivate

#endif // ANYKEEP_FOLDERS_PRIVATE_H
