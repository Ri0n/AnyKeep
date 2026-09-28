#include "mediasource.h"

#include "localmediastore.h"
#include "mediareference.h"

#include <QCryptographicHash>
#include <QFile>

#include <cstring>
#include <utility>

namespace AnyKeep {
namespace {

class LocalWholeBlobMediaSource final : public MediaSource {
public:
    LocalWholeBlobMediaSource(MediaReference reference, LocalMediaStore *store) :
        reference_(std::move(reference)), store_(store ? store : LocalMediaStore::instance())
    {
    }

    bool open(QString *error) override
    {
        if (!store_ || !reference_.isValid()) {
            if (error)
                *error = QStringLiteral("Invalid local media source");
            return false;
        }
        const auto loaded = store_->data(reference_.blobId);
        if (!loaded) {
            if (error)
                *error = loaded.error;
            return false;
        }
        bytes_ = loaded.value;
        if (error)
            error->clear();
        return true;
    }

    void close() override { bytes_.clear(); }

    qint64 size() const override { return bytes_.size(); }

    qint64 read(qint64 offset, char *data, qint64 maxSize, QString *error) override
    {
        if (offset < 0 || maxSize < 0 || (!data && maxSize > 0)) {
            if (error)
                *error = QStringLiteral("Invalid media source read");
            return -1;
        }
        const qint64 available = qMax<qint64>(0, size() - offset);
        const qint64 count     = qMin(maxSize, available);
        if (count <= 0)
            return 0;
        std::memcpy(data, bytes_.constData() + offset, static_cast<size_t>(count));
        if (error)
            error->clear();
        return count;
    }

private:
    MediaReference   reference_;
    LocalMediaStore *store_ = nullptr;
    QByteArray       bytes_;
};

class ExternalFileMediaSource final : public MediaSource {
public:
    ExternalFileMediaSource(MediaReference reference, LocalMediaStore *store) :
        reference_(std::move(reference)), store_(store ? store : LocalMediaStore::instance())
    {
    }

    bool open(QString *error) override
    {
        if (!store_ || !reference_.isValid()) {
            if (error)
                *error = QStringLiteral("Invalid external media source");
            return false;
        }
        const auto source = store_->externalSource(reference_);
        if (!source) {
            if (error)
                *error = source.error;
            return false;
        }
        source_ = source.value;
        file_.setFileName(source_.fileName);
        if (!file_.open(QIODevice::ReadOnly)) {
            if (error)
                *error = file_.errorString();
            return false;
        }
        verifiedChunkIndex_ = -1;
        verifiedChunk_.clear();
        if (error)
            error->clear();
        return true;
    }

    void close() override
    {
        file_.close();
        verifiedChunkIndex_ = -1;
        verifiedChunk_.clear();
        source_ = {};
    }

    qint64 size() const override { return source_.size; }

    qint64 read(qint64 offset, char *data, qint64 maxSize, QString *error) override
    {
        if (offset < 0 || maxSize < 0 || (!data && maxSize > 0) || offset > size()) {
            if (error)
                *error = QStringLiteral("Invalid media source read");
            return -1;
        }
        qint64 remaining = qMin(maxSize, size() - offset);
        qint64 written   = 0;
        while (remaining > 0) {
            const qint64 chunkIndex = offset / qint64(source_.chunkSize);
            if (!ensureChunk(chunkIndex, error))
                return -1;
            const qint64 chunkStart  = chunkIndex * qint64(source_.chunkSize);
            const qint64 chunkOffset = offset - chunkStart;
            const qint64 count       = qMin<qint64>(remaining, verifiedChunk_.size() - chunkOffset);
            if (count <= 0) {
                if (error)
                    *error = QStringLiteral("External media source chunk is invalid");
                return -1;
            }
            std::memcpy(data + written, verifiedChunk_.constData() + chunkOffset, static_cast<size_t>(count));
            offset += count;
            written += count;
            remaining -= count;
        }
        if (error)
            error->clear();
        return written;
    }

private:
    bool ensureChunk(qint64 index, QString *error)
    {
        if (index == verifiedChunkIndex_)
            return true;
        if (index < 0 || index >= source_.chunkHashes.size()) {
            if (error)
                *error = QStringLiteral("External media source chunk is out of range");
            return false;
        }

        const qint64 chunkStart = index * qint64(source_.chunkSize);
        const qint64 expected   = qMin<qint64>(source_.chunkSize, source_.size - chunkStart);
        if (!file_.seek(chunkStart)) {
            if (error)
                *error = file_.errorString();
            return false;
        }
        QByteArray chunk = file_.read(expected);
        if (chunk.size() != expected) {
            if (error)
                *error = file_.errorString().isEmpty() ? QStringLiteral("Could not read external media source")
                                                       : file_.errorString();
            return false;
        }
        if (QCryptographicHash::hash(chunk, QCryptographicHash::Sha256) != source_.chunkHashes.at(index)) {
            if (error)
                *error = QStringLiteral("The external media source changed since it was linked");
            return false;
        }
        verifiedChunkIndex_ = index;
        verifiedChunk_      = std::move(chunk);
        return true;
    }

    MediaReference           reference_;
    LocalMediaStore         *store_ = nullptr;
    LocalMediaExternalSource source_;
    QFile                    file_;
    qint64                   verifiedChunkIndex_ { -1 };
    QByteArray               verifiedChunk_;
};

} // namespace

std::unique_ptr<MediaSource> createLocalMediaSource(const MediaReference &reference, LocalMediaStore *store)
{
    auto *resolvedStore = store ? store : LocalMediaStore::instance();
    if (resolvedStore && !resolvedStore->containsManagedBlob(reference.blobId)) {
        const auto external = resolvedStore->externalSource(reference);
        if (external)
            return std::make_unique<ExternalFileMediaSource>(reference, resolvedStore);
    }
    return std::make_unique<LocalWholeBlobMediaSource>(reference, resolvedStore);
}

} // namespace AnyKeep
