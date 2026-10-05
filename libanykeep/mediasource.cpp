#include "mediasource.h"

#include "localmediastore.h"
#include "mediareference.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

#include <cstring>
#include <utility>

namespace AnyKeep {
namespace {

class LocalManagedMediaSource final : public MediaSource {
public:
    LocalManagedMediaSource(MediaReference reference, LocalMediaStore *store) :
        reference_(std::move(reference)), store_(store ? store : LocalMediaStore::instance())
    {
    }

    bool open(QString *error) override
    {
        if (!store_ || !reference_.isValid() || reference_.blobId.isEmpty()) {
            if (error)
                *error = QStringLiteral("Invalid local media source");
            return false;
        }

        chunked_ = store_->isChunkedManagedBlob(reference_.blobId);
        if (chunked_) {
            const auto probe = store_->readManagedRange(reference_.blobId, 0, 0);
            if (!probe) {
                if (error)
                    *error = probe.error;
                return false;
            }
            if (probe.totalSize != reference_.size) {
                if (error)
                    *error = QStringLiteral("Managed media source size does not match its reference");
                return false;
            }
            size_ = probe.totalSize;
            verifiedChunkIndex_ = -1;
            verifiedChunk_.clear();
            bytes_.clear();
        } else {
            const auto loaded = store_->data(reference_.blobId);
            if (!loaded) {
                if (error)
                    *error = loaded.error;
                return false;
            }
            if (loaded.value.size() != reference_.size
                || (reference_.checksum.size() == 32
                    && QCryptographicHash::hash(loaded.value, QCryptographicHash::Sha256) != reference_.checksum)) {
                if (error)
                    *error = QStringLiteral("Managed media source failed its integrity check");
                return false;
            }
            bytes_ = loaded.value;
            size_  = bytes_.size();
        }
        if (error)
            error->clear();
        return true;
    }

    void close() override
    {
        chunked_ = false;
        size_ = 0;
        verifiedChunkIndex_ = -1;
        verifiedChunk_.clear();
        bytes_.clear();
    }

    qint64 size() const override { return size_; }

    qint64 read(qint64 offset, char *data, qint64 maxSize, QString *error) override
    {
        if (offset < 0 || maxSize < 0 || (!data && maxSize > 0) || offset > size_) {
            if (error)
                *error = QStringLiteral("Invalid media source read");
            return -1;
        }
        qint64 remaining = qMin(maxSize, size_ - offset);
        if (remaining <= 0) {
            if (error)
                error->clear();
            return 0;
        }

        if (!chunked_) {
            std::memcpy(data, bytes_.constData() + offset, static_cast<size_t>(remaining));
            if (error)
                error->clear();
            return remaining;
        }

        qint64 written = 0;
        while (remaining > 0) {
            const qint64 chunkIndex = offset / LocalMediaStore::ManagedChunkSize;
            if (!ensureChunk(chunkIndex, error))
                return -1;
            const qint64 chunkStart  = chunkIndex * LocalMediaStore::ManagedChunkSize;
            const qint64 chunkOffset = offset - chunkStart;
            const qint64 count       = qMin<qint64>(remaining, verifiedChunk_.size() - chunkOffset);
            if (count <= 0) {
                if (error)
                    *error = QStringLiteral("Managed media source chunk is invalid");
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
        if (index < 0) {
            if (error)
                *error = QStringLiteral("Managed media source chunk is out of range");
            return false;
        }

        const qint64 chunkStart = index * LocalMediaStore::ManagedChunkSize;
        if (chunkStart < 0 || chunkStart >= size_) {
            if (error)
                *error = QStringLiteral("Managed media source chunk is out of range");
            return false;
        }
        const qint64 expected = qMin(LocalMediaStore::ManagedChunkSize, size_ - chunkStart);
        const auto loaded = store_->readManagedRange(reference_.blobId, chunkStart, expected);
        if (!loaded) {
            if (error)
                *error = loaded.error;
            return false;
        }
        if (loaded.totalSize != size_ || loaded.value.size() != expected) {
            if (error)
                *error = QStringLiteral("Managed media source chunk does not match its reference");
            return false;
        }
        verifiedChunkIndex_ = index;
        verifiedChunk_      = loaded.value;
        return true;
    }

    MediaReference   reference_;
    LocalMediaStore *store_ = nullptr;
    bool              chunked_ { false };
    qint64            size_ { 0 };
    qint64            verifiedChunkIndex_ { -1 };
    QByteArray        verifiedChunk_;
    QByteArray        bytes_;
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
        const qint64 chunkCount = source_.size == 0
            ? 0
            : (source_.size + qint64(source_.chunkSize) - 1) / qint64(source_.chunkSize);
        if (index < 0 || index >= chunkCount) {
            if (error)
                *error = QStringLiteral("External media source chunk is out of range");
            return false;
        }

        // SourceRevision is the cheap guard for a cached fingerprint. It also
        // protects lazy, not-yet-fingerprinted playback from silently switching
        // to a different file while the stream is open.
        const QFileInfo current(source_.fileName);
        if (!current.isFile() || current.size() != source_.size
            || current.lastModified().toMSecsSinceEpoch() != source_.modifiedMsecsSinceEpoch) {
            if (error)
                *error = QStringLiteral("The external media source changed since it was linked");
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
        if (source_.hasFingerprint()
            && QCryptographicHash::hash(chunk, QCryptographicHash::Sha256) != source_.chunkHashes.at(index)) {
            if (error)
                *error = QStringLiteral("The external media source changed since it was fingerprinted");
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
    if (resolvedStore && !reference.blobId.isEmpty() && resolvedStore->containsManagedBlob(reference.blobId))
        return std::make_unique<LocalManagedMediaSource>(reference, resolvedStore);
    if (resolvedStore) {
        const auto external = resolvedStore->externalSource(reference);
        if (external)
            return std::make_unique<ExternalFileMediaSource>(reference, resolvedStore);
    }
    return std::make_unique<LocalManagedMediaSource>(reference, resolvedStore);
}

} // namespace AnyKeep