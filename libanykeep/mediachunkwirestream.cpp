#include "mediachunkwirestream.h"

#include "mediareference.h"
#include "mediasource.h"

#include <QCryptographicHash>

#include <cstring>
#include <limits>
#include <utility>

namespace AnyKeep {

class MediaChunkWireStream::Impl {
public:
    Impl(std::unique_ptr<MediaSource> source, MediaChunkWireParameters parameters) :
        source(std::move(source)), parameters(std::move(parameters)), hash(QCryptographicHash::Sha256)
    {
    }

    bool ensureWireChunk(quint64 index, QString *error)
    {
        if (index == cachedChunkIndex)
            return true;
        if (!source) {
            if (error)
                *error = QStringLiteral("Chunked media wire stream has no source");
            return false;
        }

        const auto plainSize = MediaChunkWire::plainChunkSize(parameters, index);
        if (!plainSize) {
            if (error)
                *error = QStringLiteral("Chunked media wire chunk is out of range");
            return false;
        }
        const quint64 plainOffset64 = index * quint64(parameters.chunkSize);
        if (plainOffset64 > quint64(std::numeric_limits<qint64>::max())) {
            if (error)
                *error = QStringLiteral("Chunked media plaintext offset overflow");
            return false;
        }

        QByteArray plain(qsizetype(*plainSize), Qt::Uninitialized);
        if (*plainSize > 0) {
            QString readError;
            const qint64 read = source->read(qint64(plainOffset64), plain.data(), *plainSize, &readError);
            if (read != qint64(*plainSize)) {
                if (error)
                    *error = readError.isEmpty() ? QStringLiteral("Could not read authenticated media chunk")
                                                 : readError;
                return false;
            }
        }

        const auto encrypted = MediaChunkWire::encryptChunk(parameters, index, plain);
        if (!encrypted) {
            if (error)
                *error = encrypted.error;
            return false;
        }
        cachedChunkIndex = index;
        cachedWireChunk  = encrypted.value;
        if (error)
            error->clear();
        return true;
    }

    void resetHash()
    {
        hash.reset();
        hashPosition = 0;
        hashValid    = true;
        completedHash.clear();
    }

    void invalidateHash()
    {
        hash.reset();
        hashPosition = 0;
        hashValid    = false;
        completedHash.clear();
    }

    void observeRead(qint64 start, const char *data, qint64 count, qint64 totalWireSize)
    {
        if (!hashValid || start != hashPosition || count < 0)
            return;
        if (count > 0)
            hash.addData(data, count);
        hashPosition += count;
        if (hashPosition == totalWireSize)
            completedHash = hash.result();
    }

    std::unique_ptr<MediaSource> source;
    MediaChunkWireParameters     parameters;
    qint64                       wireSize { -1 };
    quint64                      cachedChunkIndex { std::numeric_limits<quint64>::max() };
    QByteArray                   cachedWireChunk;
    QCryptographicHash           hash;
    qint64                       hashPosition { 0 };
    bool                         hashValid { true };
    QByteArray                   completedHash;
};

MediaChunkWireStream::MediaChunkWireStream(const MediaReference &reference, MediaChunkWireParameters parameters,
                                           QObject *parent) :
    MediaChunkWireStream(createLocalMediaSource(reference), std::move(parameters), parent)
{
}

MediaChunkWireStream::MediaChunkWireStream(std::unique_ptr<MediaSource> source, MediaChunkWireParameters parameters,
                                           QObject *parent) :
    QIODevice(parent), impl_(std::make_unique<Impl>(std::move(source), std::move(parameters)))
{
}

MediaChunkWireStream::~MediaChunkWireStream() = default;

bool MediaChunkWireStream::open(OpenMode mode)
{
    if (mode != QIODevice::ReadOnly || !impl_->source || !impl_->parameters.isValid()) {
        setErrorString(QStringLiteral("MediaChunkWireStream is read-only and requires valid parameters"));
        return false;
    }
    const auto wireSize = MediaChunkWire::wireSize(impl_->parameters);
    if (!wireSize || *wireSize > quint64(std::numeric_limits<qint64>::max())) {
        setErrorString(QStringLiteral("Chunked media wire object is too large"));
        return false;
    }

    QString error;
    if (!impl_->source->open(&error)) {
        setErrorString(error);
        return false;
    }
    if (impl_->source->size() < 0 || quint64(impl_->source->size()) != impl_->parameters.plainSize) {
        impl_->source->close();
        setErrorString(QStringLiteral("Chunked media plaintext size does not match its descriptor"));
        return false;
    }

    impl_->wireSize = qint64(*wireSize);
    impl_->cachedChunkIndex = std::numeric_limits<quint64>::max();
    impl_->cachedWireChunk.clear();
    impl_->resetHash();
    setErrorString({});
    return QIODevice::open(mode);
}

void MediaChunkWireStream::close()
{
    if (impl_->source)
        impl_->source->close();
    impl_->wireSize = -1;
    impl_->cachedChunkIndex = std::numeric_limits<quint64>::max();
    impl_->cachedWireChunk.clear();
    impl_->invalidateHash();
    QIODevice::close();
}

qint64 MediaChunkWireStream::size() const { return impl_->wireSize < 0 ? 0 : impl_->wireSize; }

qint64 MediaChunkWireStream::bytesAvailable() const
{
    return qMax<qint64>(0, size() - pos()) + QIODevice::bytesAvailable();
}

bool MediaChunkWireStream::seek(qint64 position)
{
    if (position < 0 || position > size())
        return false;
    if (position != pos())
        impl_->invalidateHash();
    return QIODevice::seek(position);
}

const MediaChunkWireParameters &MediaChunkWireStream::parameters() const { return impl_->parameters; }

QByteArray MediaChunkWireStream::completedWireSha256() const { return impl_->completedHash; }

qint64 MediaChunkWireStream::readData(char *data, qint64 maxSize)
{
    if (!impl_->source || impl_->wireSize < 0 || maxSize < 0 || (!data && maxSize > 0)) {
        setErrorString(QStringLiteral("Invalid chunked media wire read"));
        return -1;
    }
    const qint64 start = pos();
    qint64 remaining   = qMin(maxSize, impl_->wireSize - start);
    if (remaining <= 0)
        return 0;

    qint64 cursor  = start;
    qint64 written = 0;
    while (remaining > 0) {
        const auto chunkIndex = MediaChunkWire::chunkIndexForWireOffset(impl_->parameters, quint64(cursor));
        if (!chunkIndex) {
            setErrorString(QStringLiteral("Chunked media wire offset is out of range"));
            return -1;
        }
        QString error;
        if (!impl_->ensureWireChunk(*chunkIndex, &error)) {
            setErrorString(error);
            return -1;
        }
        const auto chunkStart = MediaChunkWire::wireChunkOffset(impl_->parameters, *chunkIndex);
        if (!chunkStart || quint64(cursor) < *chunkStart) {
            setErrorString(QStringLiteral("Invalid chunked media wire geometry"));
            return -1;
        }
        const quint64 within64 = quint64(cursor) - *chunkStart;
        if (within64 >= quint64(impl_->cachedWireChunk.size())) {
            setErrorString(QStringLiteral("Invalid chunked media wire chunk offset"));
            return -1;
        }
        const qint64 within = qint64(within64);
        const qint64 count  = qMin<qint64>(remaining, impl_->cachedWireChunk.size() - within);
        std::memcpy(data + written, impl_->cachedWireChunk.constData() + within, static_cast<size_t>(count));
        cursor += count;
        written += count;
        remaining -= count;
    }

    impl_->observeRead(start, data, written, impl_->wireSize);
    setErrorString({});
    return written;
}

} // namespace AnyKeep
