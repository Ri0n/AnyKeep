#ifndef ANYKEEP_MEDIACHUNKWIRE_H
#define ANYKEEP_MEDIACHUNKWIRE_H

#include "anykeep_export.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace AnyKeep {

struct ANYKEEP_EXPORT MediaChunkWireParameters {
    static constexpr quint32 DefaultChunkSize = 1024 * 1024;
    static constexpr quint32 MaxChunkSize     = 16 * 1024 * 1024;

    QByteArray rootKey;
    QByteArray noncePrefix;
    QByteArray plainChecksum;
    quint64    plainSize { 0 };
    quint32    chunkSize { DefaultChunkSize };

    bool    isValid() const;
    quint64 chunkCount() const;
};

struct ANYKEEP_EXPORT MediaChunkWireResult {
    QByteArray value;
    QString    error;

    explicit operator bool() const { return error.isEmpty(); }
};

/**
 * Portable independently-authenticated chunk representation for progressive
 * remote media. The wire object is the concatenation of deterministic records;
 * it has no Qt framing and is intentionally distinct from XEP-0448.
 */
class ANYKEEP_EXPORT MediaChunkWire {
public:
    static constexpr quint16 Version     = 1;
    static constexpr quint32 ContextSize = 64;
    static constexpr quint32 TagSize     = 16;
    static constexpr quint32 Overhead    = ContextSize + TagSize;

    static std::optional<MediaChunkWireParameters> generate(quint64 plainSize, const QByteArray &plainChecksum,
                                                            quint32 chunkSize = MediaChunkWireParameters::DefaultChunkSize);

    static std::optional<quint32> plainChunkSize(const MediaChunkWireParameters &parameters, quint64 index);
    static std::optional<quint64> wireChunkSize(const MediaChunkWireParameters &parameters, quint64 index);
    static std::optional<quint64> wireChunkOffset(const MediaChunkWireParameters &parameters, quint64 index);
    static std::optional<quint64> wireSize(const MediaChunkWireParameters &parameters);
    static std::optional<quint64> chunkIndexForWireOffset(const MediaChunkWireParameters &parameters,
                                                          quint64 wireOffset);

    static MediaChunkWireResult encryptChunk(const MediaChunkWireParameters &parameters, quint64 index,
                                             const QByteArray &plainChunk);
    static MediaChunkWireResult decryptChunk(const MediaChunkWireParameters &parameters, quint64 index,
                                             const QByteArray &wireChunk);

private:
    static QByteArray nonce(const MediaChunkWireParameters &parameters, quint64 index);
    static QByteArray context(const MediaChunkWireParameters &parameters, quint64 index, quint32 actualPlainSize);
};

} // namespace AnyKeep

#endif // ANYKEEP_MEDIACHUNKWIRE_H
