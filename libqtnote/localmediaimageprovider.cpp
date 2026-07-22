#include "localmediaimageprovider.h"

#include "localmediastore.h"

#include <QQmlEngine>

namespace QtNote {

LocalMediaImageProvider::LocalMediaImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) { }

QImage LocalMediaImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const auto loaded = LocalMediaStore::instance()->data(QByteArray::fromHex(id.toLatin1()));
    QImage     image;
    if (loaded)
        image.loadFromData(loaded.value);
    if (size)
        *size = image.size();
    if (!image.isNull() && requestedSize.isValid())
        image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

void installLocalMediaImageProvider(QQmlEngine *engine)
{
    if (engine)
        engine->addImageProvider(QStringLiteral("qtnote-media"), new LocalMediaImageProvider);
}

} // namespace QtNote
