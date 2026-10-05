#ifndef ANYKEEP_IRISCHUNKEDMEDIASOURCE_H
#define ANYKEEP_IRISCHUNKEDMEDIASOURCE_H

#include "mediachunkwire.h"

#include <iris/xmpp_file-sharing.h>

#include <QByteArray>
#include <QDomElement>
#include <QString>

namespace AnyKeep {

struct IrisChunkedMediaSourceResult;

/**
 * AnyKeep's independently authenticated remote-media representation.
 *
 * This is an XEP-0447 Source::Other profile. It is deliberately distinct from
 * XEP-0448: the secret representation parameters live only in the encrypted
 * Private Notes descriptor while the nested sources remain ordinary HTTP/Jingle
 * byte transports.
 */
struct IrisChunkedMediaSource {
    static const QString Namespace;
    static const QString ElementName;
    static constexpr quint16 Version = 1;

    MediaChunkWireParameters           parameters;
    QByteArray                         wireHash; // optional SHA-256 of the complete virtual wire object
    XMPP::StatelessFileSharing::Sources sources;

    QString invalidReason() const;
    bool    isValid() const { return invalidReason().isEmpty(); }

    QDomElement toXml(QDomDocument *document) const;
    XMPP::StatelessFileSharing::Source toSource() const;

    static IrisChunkedMediaSourceResult fromSource(const XMPP::StatelessFileSharing::Source &source,
                                                   quint64 plainSize, const QByteArray &plainChecksum);
};

struct IrisChunkedMediaSourceResult {
    IrisChunkedMediaSource value;
    QString                error;

    explicit operator bool() const { return error.isEmpty(); }
};

} // namespace AnyKeep

#endif // ANYKEEP_IRISCHUNKEDMEDIASOURCE_H
