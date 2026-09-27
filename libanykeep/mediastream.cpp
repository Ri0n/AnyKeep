#include "mediastream.h"

#include "localmediastore.h"

#include <cstring>
#include <utility>

namespace AnyKeep {

class MediaStream::Impl {
public:
    explicit Impl(MediaReference reference) : reference(std::move(reference)) {}
    MediaReference reference;
    QByteArray bytes;
    QString error;
};

MediaStream::MediaStream(const MediaReference &reference, QObject *parent) :
    QIODevice(parent), impl_(std::make_unique<Impl>(reference))
{
}
MediaStream::~MediaStream() = default;

bool MediaStream::open(OpenMode mode)
{
    if (mode != QIODevice::ReadOnly) {
        impl_->error = QStringLiteral("MediaStream is read-only");
        return false;
    }
    // Whole-object authenticated local blobs are the compatibility source.
    // The QIODevice boundary is intentionally independent of that storage
    // representation so a chunk-authenticated ranged source can replace it.
    const auto loaded = LocalMediaStore::instance()->data(impl_->reference.blobId);
    if (!loaded) {
        impl_->error = loaded.error;
        return false;
    }
    impl_->bytes = loaded.value;
    impl_->error.clear();
    return QIODevice::open(mode);
}

void MediaStream::close()
{
    impl_->bytes.clear();
    QIODevice::close();
}

qint64 MediaStream::size() const { return impl_->bytes.size(); }

bool MediaStream::seek(qint64 position)
{
    if (position < 0 || position > size())
        return false;
    return QIODevice::seek(position);
}

QString MediaStream::errorString() const { return impl_->error; }

qint64 MediaStream::readData(char *data, qint64 maxSize)
{
    const qint64 available = qMax<qint64>(0, size() - pos());
    const qint64 count = qMin(maxSize, available);
    if (count <= 0)
        return 0;
    std::memcpy(data, impl_->bytes.constData() + pos(), static_cast<size_t>(count));
    return count;
}

} // namespace AnyKeep
