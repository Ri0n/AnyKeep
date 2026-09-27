#include "localmediaimageprovider.h"

#include "localmediastore.h"

#include <QLoggingCategory>
#include <QFile>
#include <QQmlEngine>

namespace AnyKeep {

Q_LOGGING_CATEGORY(logLocalMediaImageProvider, "anykeep.media.imageprovider")

LocalMediaImageProvider::LocalMediaImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage LocalMediaImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const bool posterRequest = id.endsWith(QLatin1String("/poster"));
    const QString blobPart   = posterRequest ? id.left(id.size() - 7) : id;
    const auto encodedId     = blobPart.toLatin1();
    const auto blobId        = QByteArray::fromHex(encodedId);
    if (encodedId.size() != 64 || blobId.size() != 32 || blobId.toHex() != encodedId.toLower()) {
        qCWarning(logLocalMediaImageProvider) << "Rejected malformed local image blob id";
        if (size)
            *size = {};
        return {};
    }

    QImage image;
    if (posterRequest) {
        QFile poster(LocalMediaStore::instance()->derivedPosterPath(blobId));
        if (poster.open(QIODevice::ReadOnly))
            image.load(&poster, "JPEG");
        if (image.isNull()) {
            if (size)
                *size = {};
            return {};
        }
    } else {
        const auto loaded = LocalMediaStore::instance()->data(blobId);
        if (!loaded) {
            qCWarning(logLocalMediaImageProvider)
                << "Failed to load local image blob" << QString::fromLatin1(blobId.toHex().left(12)) << loaded.error;
        } else if (!image.loadFromData(loaded.value)) {
            qCWarning(logLocalMediaImageProvider)
                << "Failed to decode local image blob" << QString::fromLatin1(blobId.toHex().left(12));
        }
    }
    if (size)
            *size = {};
        return {};
    }

    QImage image;
    if (id.endsWith(QLatin1String("/poster"))) {
        QFile poster(LocalMediaStore::instance()->derivedPosterPath(blobId));
        if (poster.open(QIODevice::ReadOnly))
            image.load(&poster, "JPEG");
    }
    const auto loaded = image.isNull() ? LocalMediaStore::instance()->data(blobId) : LocalMediaDataResult{};
    if (!loaded) {
        qCWarning(logLocalMediaImageProvider)
            << "Failed to load local image blob" << QString::fromLatin1(blobId.toHex().left(12)) << loaded.error;
    } else if (!image.loadFromData(loaded.value)) {
        qCWarning(logLocalMediaImageProvider)
            << "Failed to decode local image blob" << QString::fromLatin1(blobId.toHex().left(12));
    }
    if (size)
        *size = image.size();
    if (!image.isNull() && requestedSize.isValid())
        image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

void installLocalMediaImageProvider(QQmlEngine *engine)
{
    if (!engine)
        return;

    QString error;
    if (!LocalMediaStore::instance()->initialize(&error))
        qCWarning(logLocalMediaImageProvider) << "Failed to initialize the local media image provider:" << error;
    engine->addImageProvider(QStringLiteral("anykeep-media"), new LocalMediaImageProvider);
}

} // namespace AnyKeep
