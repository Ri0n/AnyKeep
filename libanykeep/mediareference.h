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
    // A tiny derived poster inside encrypted note metadata. Never video bytes.
    // -1 means that the source frame position is unknown.
    qint64     posterFrameMs { -1 };
    QByteArray videoThumbnailJpeg;
    // Local encrypted derived poster. This is not a second user-visible
    // attachment and is never exposed as an XMPP source identifier.
    QByteArray posterBlobId;
    static constexpr qsizetype MaxVideoThumbnailBytes = 6 * 1024;

    QString uri() const;
    // The attachment UUID and portable name are the stable document identity.
    // External files may be linked before their content fingerprint has been
    // computed, so blobId/checksum are deliberately allowed to be empty.
    bool isValid() const { return !id.isNull() && !portableName.isEmpty(); }
    bool hasContentFingerprint() const { return blobId.size() == 32 && checksum.size() == 32; }

    bool operator==(const MediaReference &other) const
    {
        return id == other.id && blobId == other.blobId && originalName == other.originalName
            && portableName == other.portableName && mediaType == other.mediaType && size == other.size
            && checksum == other.checksum && remoteData == other.remoteData
            && posterFrameMs == other.posterFrameMs && videoThumbnailJpeg == other.videoThumbnailJpeg
            && posterBlobId == other.posterBlobId;
    }
};

} // namespace AnyKeep

#endif // MEDIAREFERENCE_H