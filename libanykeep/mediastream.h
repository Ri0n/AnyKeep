#ifndef ANYKEEP_MEDIASTREAM_H
#define ANYKEEP_MEDIASTREAM_H

#include "anykeep_export.h"

#include <QIODevice>
#include <memory>

namespace AnyKeep {

class MediaSource;
struct MediaReference;

class ANYKEEP_EXPORT MediaStream final : public QIODevice {
    Q_OBJECT
public:
    explicit MediaStream(const MediaReference &reference, QObject *parent = nullptr);
    explicit MediaStream(std::unique_ptr<MediaSource> source, QObject *parent = nullptr);
    ~MediaStream() override;

    bool open(OpenMode mode) override;
    void close() override;
    bool isSequential() const override { return false; }
    qint64 size() const override;
    qint64 bytesAvailable() const override;
    bool seek(qint64 position) override;

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *, qint64) override { return -1; }

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace AnyKeep
#endif
