#ifndef ANYKEEP_MEDIACHUNKWIRESTREAM_H
#define ANYKEEP_MEDIACHUNKWIRESTREAM_H

#include "anykeep_export.h"
#include "mediachunkwire.h"

#include <QByteArray>
#include <QIODevice>
#include <memory>

namespace AnyKeep {

class MediaSource;
struct MediaReference;

/**
 * Seekable virtual encrypted object backed by an authenticated plaintext
 * MediaSource. HTTP upload can consume it sequentially while Jingle can seek to
 * an arbitrary byte range of the exact same deterministic wire object. At most
 * one generated wire record is retained between reads.
 */
class ANYKEEP_EXPORT MediaChunkWireStream final : public QIODevice {
    Q_OBJECT
public:
    MediaChunkWireStream(const MediaReference &reference, MediaChunkWireParameters parameters,
                         QObject *parent = nullptr);
    MediaChunkWireStream(std::unique_ptr<MediaSource> source, MediaChunkWireParameters parameters,
                         QObject *parent = nullptr);
    ~MediaChunkWireStream() override;

    bool open(OpenMode mode) override;
    void close() override;
    bool isSequential() const override { return false; }
    qint64 size() const override;
    qint64 bytesAvailable() const override;
    bool seek(qint64 position) override;

    const MediaChunkWireParameters &parameters() const;

    // Becomes available only after one uninterrupted sequential read from byte
    // zero through EOF. Seeking or otherwise changing the read sequence clears
    // the incremental hash state.
    QByteArray completedWireSha256() const;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace AnyKeep

#endif // ANYKEEP_MEDIACHUNKWIRESTREAM_H
