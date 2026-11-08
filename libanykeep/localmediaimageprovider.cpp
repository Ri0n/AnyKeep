#include "localmediaimageprovider.h"

#include "localmediastore.h"

#include <QLoggingCategory>
#include <QQmlEngine>
#include <QUrl>
#include <QUrlQuery>

namespace AnyKeep {

Q_LOGGING_CATEGORY(logLocalMediaImageProvider, "anykeep.media.imageprovider")

LocalMediaImageProvider::LocalMediaImageProvider() : LocalMediaImageProvider(nullptr) {}

LocalMediaImageProvider::LocalMediaImageProvider(LocalMediaStore *store) :
    QQuickImageProvider(QQuickImageProvider::Image), store_(store ? store : LocalMediaStore::instance())
{
}

QString LocalMediaImageProvider::imageUrl(const MediaReference &reference)
{
    QUrl url(QStringLiteral("image://anykeep-media/reference/%1").arg(reference.id.toString(QUuid::WithoutBraces)));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("size"), QString::number(reference.size));
    query.addQueryItem(QStringLiteral("blob"), QString::fromLatin1(reference.blobId.toHex()));
    query.addQueryItem(QStringLiteral("checksum"), QString::fromLatin1(reference.checksum.toHex()));
    url.setQuery(query);
    return url.toString();
}

QImage LocalMediaImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const auto decodeDigest = [](const QByteArray &encoded, QByteArray *decoded) {
        *decoded = QByteArray::fromHex(encoded);
        return encoded.size() == 64 && decoded->size() == 32 && decoded->toHex() == encoded.toLower();
    };
    LocalMediaDataResult loaded;
    bool                 valid = false;
    if (id.startsWith(QLatin1String("reference/"))) {
        const QUrl      url(QStringLiteral("image://anykeep-media/") + id);
        const QUrlQuery query(url);
        MediaReference  reference;
        reference.id           = QUuid(url.path().mid(QStringLiteral("/reference/").size()));
        reference.portableName = QStringLiteral("image");
        bool sizeOk            = false;
        reference.size         = query.queryItemValue(QStringLiteral("size")).toLongLong(&sizeOk);
        const auto blob        = query.queryItemValue(QStringLiteral("blob")).toLatin1();
        const auto checksum    = query.queryItemValue(QStringLiteral("checksum")).toLatin1();
        valid                  = !reference.id.isNull() && sizeOk && reference.size >= 0
            && (blob.isEmpty() || decodeDigest(blob, &reference.blobId))
            && (checksum.isEmpty() || decodeDigest(checksum, &reference.checksum));
        if (valid)
            loaded = store_->data(reference);
    } else {
        QByteArray blobId;
        valid = decodeDigest(id.toLatin1(), &blobId);
        if (valid)
            loaded = store_->data(blobId);
    }
    if (!valid) {
        qCWarning(logLocalMediaImageProvider) << "Rejected malformed local image blob id";
        if (size)
            *size = {};
        return {};
    }

    QImage image;
    if (!loaded) {
        qCWarning(logLocalMediaImageProvider) << "Failed to load local image" << loaded.error;
    } else if (!image.loadFromData(loaded.value)) {
        qCWarning(logLocalMediaImageProvider) << "Failed to decode local image";
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
