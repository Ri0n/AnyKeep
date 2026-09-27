#include "mediasource.h"

#include "localmediastore.h"
#include "mediareference.h"

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

} // namespace

std::unique_ptr<MediaSource> createLocalMediaSource(const MediaReference &reference, LocalMediaStore *store)
{
    return std::make_unique<LocalWholeBlobMediaSource>(reference, store);
}

} // namespace AnyKeep
