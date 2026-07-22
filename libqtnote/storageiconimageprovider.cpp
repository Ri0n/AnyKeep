#include "storageiconimageprovider.h"

#include "notemanager.h"

#include <QIcon>
#include <QImage>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QUrl>

namespace QtNote {

namespace {
    constexpr auto ProviderId = "qtnote-storage-icon";

    class StorageIconImageProvider final : public QQuickImageProvider {
    public:
        StorageIconImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) { }

        QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
        {
            const int slash = id.indexOf(QLatin1Char('/'));
            if (slash <= 0)
                return {};

            const QString kind      = id.left(slash);
            const QString storageId = QUrl::fromPercentEncoding(id.mid(slash + 1).toLatin1());
            const auto    storage   = NoteManager::instance()->storage(storageId);
            if (!storage)
                return {};

            const QIcon icon = kind == QLatin1String("note") ? storage->noteIcon() : storage->storageIcon();
            if (icon.isNull())
                return {};

            QSize target = requestedSize.isValid() ? requestedSize : QSize(24, 24);
            target.setWidth(qMax(1, target.width()));
            target.setHeight(qMax(1, target.height()));
            QImage image = icon.pixmap(target).toImage();
            if (size)
                *size = image.size();
            return image;
        }
    };
}

void installStorageIconImageProvider(QQmlEngine *engine)
{
    if (!engine)
        return;
    engine->addImageProvider(QLatin1String(ProviderId), new StorageIconImageProvider);
}

QString storageIconSource(const QString &storageId, bool noteIcon)
{
    if (storageId.isEmpty())
        return {};
    return QStringLiteral("image://%1/%2/%3")
        .arg(QLatin1String(ProviderId), noteIcon ? QStringLiteral("note") : QStringLiteral("storage"),
             QString::fromLatin1(QUrl::toPercentEncoding(storageId)));
}

} // namespace QtNote
