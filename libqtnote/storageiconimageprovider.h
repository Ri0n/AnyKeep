#ifndef STORAGEICONIMAGEPROVIDER_H
#define STORAGEICONIMAGEPROVIDER_H

#include "qtnote_export.h"

#include <QString>

class QQmlEngine;

namespace QtNote {

QTNOTE_EXPORT void    installStorageIconImageProvider(QQmlEngine *engine);
QTNOTE_EXPORT QString storageIconSource(const QString &storageId, bool noteIcon = false);

} // namespace QtNote

#endif // STORAGEICONIMAGEPROVIDER_H
