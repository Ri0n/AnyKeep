#include "mediastream.h"

#include "mediasource.h"
#include "mediareference.h"

#include <utility>

namespace AnyKeep {

class MediaStream::Impl {
public:
    explicit Impl(std::unique_ptr<MediaSource> source) : source(std::move(source)) {}
    std::unique_ptr<MediaSource> source;
};

MediaStream::MediaStream(const MediaReference &reference, QObject *parent) :
    MediaStream(createLocalMediaSource(reference), parent)
{
}

MediaStream::MediaStream(std::unique_ptr<MediaSource> source, QObject *parent) :
    QIODevice(parent), impl_(std::make_unique<Impl>(std::move(source)))
{
}
MediaStream::~MediaStream() = default;

bool MediaStream::open(OpenMode mode)
{
    if (mode != QIODevice::ReadOnly || !impl_->source) {
        setErrorString(QStringLiteral("MediaStream is read-only"));
        return false;
    }
    QString error;
    if (!impl_->source->open(&error)) {
        setErrorString(error);
        return false;
    }
    setErrorString({});
    return QIODevice::open(mode);
}

void MediaStream::close()
{
    if (impl_->source)
        impl_->source->close();
    QIODevice::close();
}

qint64 MediaStream::size() const { return impl_->source ? impl_->source->size() : 0; }

qint64 MediaStream::bytesAvailable() const
{
    return qMax<qint64>(0, size() - pos()) + QIODevice::bytesAvailable();
}

bool MediaStream::seek(qint64 position)
{
    if (position < 0 || position > size())
        return false;
    return QIODevice::seek(position);
}


qint64 MediaStream::readData(char *data, qint64 maxSize)
{
    if (!impl_->source)
        return -1;
    QString error;
    const qint64 count = impl_->source->read(pos(), data, maxSize, &error);
    if (count < 0)
        setErrorString(error);
    return count;
}

} // namespace AnyKeep
