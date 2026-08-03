#ifndef STORAGEICONIMAGEPROVIDER_H
#define STORAGEICONIMAGEPROVIDER_H

#include "anykeep_export.h"

#include <QString>

class QQmlEngine;

namespace AnyKeep {

ANYKEEP_EXPORT void    installStorageIconImageProvider(QQmlEngine *engine);
ANYKEEP_EXPORT QString storageIconSource(const QString &storageId, bool noteIcon = false);

} // namespace AnyKeep

#endif // STORAGEICONIMAGEPROVIDER_H
