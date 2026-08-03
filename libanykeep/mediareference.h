#ifndef MEDIAREFERENCE_H
#define MEDIAREFERENCE_H

#include "anykeep_export.h"

#include <QByteArray>
#include <QString>
#include <QUuid>
#include <QVariantMap>

namespace AnyKeep {

struct ANYKEEP_EXPORT MediaReference {
    QUuid       id;
    QByteArray  blobId;
    QString     originalName;
    QString     portableName;
    QString     mediaType;
    qint64      size { 0 };
    QByteArray  checksum;
    QVariantMap remoteData;

    QString uri() const;
    bool    isValid() const { return !id.isNull() && !blobId.isEmpty() && !portableName.isEmpty(); }

    bool operator==(const MediaReference &other) const
    {
        return id == other.id && blobId == other.blobId && originalName == other.originalName
            && portableName == other.portableName && mediaType == other.mediaType && size == other.size
            && checksum == other.checksum && remoteData == other.remoteData;
    }
};

} // namespace AnyKeep

#endif // MEDIAREFERENCE_H
