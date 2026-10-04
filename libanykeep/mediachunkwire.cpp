#include "mediachunkwire.h"

#include "secureenvelope.h"

#include <limits>

namespace AnyKeep {
namespace {

void appendU16(QByteArray &out, quint16 value)
{
    out.append(char((value >> 8) & 0xff));
    out.append(char(value & 0xff));
}

void appendU32(QByteArray &out, quint32 value)
{
    out.append(char((value >> 24) & 0xff));
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char(value & 0xff));
}

void appendU64(QByteArray &out, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.append(char((value >> shift) & 0xff));
}

MediaChunkWireResult invalid(const QString &message) { return { {}, message }; }

} // namespace

bool MediaChunkWireParameters::isValid() const
{
    return rootKey.size() == SecureEnvelope::MasterKeySize && noncePrefix.size() == 4 && plainChecksum.size() == 32
        && chunkSize > 0 && chunkSize <= MaxChunkSize;
}

quint64 MediaChunkWireParameters::chunkCount() const
{
    if (!isValid())
        return 0;
    if (plainSize == 0)
        return 1;
    return 1 + (plainSize - 1) / quint64(chunkSize);
}

std::optional<MediaChunkWireParameters> MediaChunkWire::generate(quint64 plainSize, const QByteArray &plainChecksum,
                                                                 quint32 chunkSize)
{
    MediaChunkWireParameters parameters;
    parameters.rootKey       = SecureEnvelope::generateMasterKey();
    parameters.noncePrefix   = SecureEnvelope::generateMasterKey().left(4);
    parameters.plainChecksum = plainChecksum;
    parameters.plainSize     = plainSize;
    parameters.chunkSize     = chunkSize;
    if (!parameters.isValid())
        return std::nullopt;
    return parameters;
}

std::optional<quint32> MediaChunkWire::plainChunkSize(const MediaChunkWireParameters &parameters, quint64 index)
{
    if (!parameters.isValid() || index >= parameters.chunkCount())
        return std::nullopt;
    if (parameters.plainSize == 0)
        return quint32(0);
    const quint64 offset = index * quint64(parameters.chunkSize);
    const quint64 left   = parameters.plainSize - offset;
    return quint32(qMin<quint64>(parameters.chunkSize, left));
}

std::optional<quint64> MediaChunkWire::wireChunkSize(const MediaChunkWireParameters &parameters, quint64 index)
{
    const auto plainSize = plainChunkSize(parameters, index);
    if (!plainSize)
        return std::nullopt;
    return quint64(*plainSize) + Overhead;
}

std::optional<quint64> MediaChunkWire::wireChunkOffset(const MediaChunkWireParameters &parameters, quint64 index)
{
    if (!parameters.isValid() || index >= parameters.chunkCount())
        return std::nullopt;
    const quint64 span = quint64(parameters.chunkSize) + Overhead;
    if (index > std::numeric_limits<quint64>::max() / span)
        return std::nullopt;
    return index * span;
}

std::optional<quint64> MediaChunkWire::wireSize(const MediaChunkWireParameters &parameters)
{
    if (!parameters.isValid())
        return std::nullopt;
    const quint64 count = parameters.chunkCount();
    if (count > (std::numeric_limits<quint64>::max() - parameters.plainSize) / Overhead)
        return std::nullopt;
    return parameters.plainSize + count * Overhead;
}

std::optional<quint64> MediaChunkWire::chunkIndexForWireOffset(const MediaChunkWireParameters &parameters,
                                                               quint64 wireOffset)
{
    const auto total = wireSize(parameters);
    if (!total || wireOffset >= *total)
        return std::nullopt;
    const quint64 span  = quint64(parameters.chunkSize) + Overhead;
    const quint64 index = wireOffset / span;
    if (index >= parameters.chunkCount())
        return std::nullopt;
    return index;
}

QByteArray MediaChunkWire::nonce(const MediaChunkWireParameters &parameters, quint64 index)
{
    QByteArray result = parameters.noncePrefix;
    result.reserve(SecureEnvelope::AeadNonceSize);
    appendU64(result, index);
    return result;
}

QByteArray MediaChunkWire::context(const MediaChunkWireParameters &parameters, quint64 index, quint32 actualPlainSize)
{
    QByteArray result;
    result.reserve(ContextSize);
    result.append("AKWC", 4);
    appendU16(result, Version);
    appendU16(result, 0);
    result.append(parameters.plainChecksum);
    appendU64(result, parameters.plainSize);
    appendU32(result, parameters.chunkSize);
    appendU64(result, index);
    appendU32(result, actualPlainSize);
    return result;
}

MediaChunkWireResult MediaChunkWire::encryptChunk(const MediaChunkWireParameters &parameters, quint64 index,
                                                  const QByteArray &plainChunk)
{
    const auto expected = plainChunkSize(parameters, index);
    if (!expected)
        return invalid(QStringLiteral("Invalid media wire chunk index or parameters"));
    if (plainChunk.size() != qsizetype(*expected))
        return invalid(QStringLiteral("Plain media chunk has an unexpected size"));

    const QByteArray expectedContext = context(parameters, index, *expected);
    if (expectedContext.size() != ContextSize)
        return invalid(QStringLiteral("Invalid media wire authentication context"));

    QByteArray authenticatedPlain = expectedContext;
    authenticatedPlain.append(plainChunk);
    const auto encrypted = SecureEnvelope::encryptAeadWithNonce(
        authenticatedPlain, parameters.rootKey, KeyDomain::RemoteMediaChunk, KeyDerivationProfile::PrivateNotes,
        nonce(parameters, index));
    if (!encrypted)
        return invalid(encrypted.error.message);
    if (encrypted.value.cipherText.size() != authenticatedPlain.size()
        || encrypted.value.tag.size() != SecureEnvelope::AeadTagSize) {
        return invalid(QStringLiteral("Encrypted media chunk has an unexpected size"));
    }

    QByteArray wire = encrypted.value.cipherText;
    wire.append(encrypted.value.tag);
    return { wire, {} };
}

MediaChunkWireResult MediaChunkWire::decryptChunk(const MediaChunkWireParameters &parameters, quint64 index,
                                                  const QByteArray &wireChunk)
{
    const auto expected = plainChunkSize(parameters, index);
    if (!expected)
        return invalid(QStringLiteral("Invalid media wire chunk index or parameters"));
    const quint64 expectedWireSize = quint64(*expected) + Overhead;
    if (expectedWireSize > quint64(std::numeric_limits<qsizetype>::max())
        || wireChunk.size() != qsizetype(expectedWireSize)) {
        return invalid(QStringLiteral("Encrypted media chunk has an unexpected size"));
    }

    AeadCiphertext encrypted;
    encrypted.nonce      = nonce(parameters, index);
    encrypted.cipherText = wireChunk.left(wireChunk.size() - SecureEnvelope::AeadTagSize);
    encrypted.tag        = wireChunk.right(SecureEnvelope::AeadTagSize);
    const auto opened    = SecureEnvelope::decryptAead(encrypted, parameters.rootKey, KeyDomain::RemoteMediaChunk,
                                                    KeyDerivationProfile::PrivateNotes);
    if (!opened)
        return invalid(opened.error.message);
    if (opened.value.size() != qsizetype(ContextSize + *expected))
        return invalid(QStringLiteral("Decrypted media chunk has an unexpected size"));

    const QByteArray expectedContext = context(parameters, index, *expected);
    if (opened.value.left(ContextSize) != expectedContext)
        return invalid(QStringLiteral("Media chunk authentication context mismatch"));
    return { opened.value.mid(ContextSize), {} };
}

} // namespace AnyKeep
