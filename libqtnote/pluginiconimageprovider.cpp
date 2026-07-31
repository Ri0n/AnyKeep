#include "pluginiconimageprovider.h"

#include <QBuffer>
#include <QCache>
#include <QFile>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QSvgRenderer>
#include <QUrl>

namespace QtNote {
namespace {
    constexpr auto ProviderId = "qtnote-plugin-icon";

    struct Repository {
        QMutex                            mutex;
        QHash<QString, PluginIconPayload> icons;
        QHash<QString, QString>           storagePlugins;
        QCache<QString, QImage>           rendered { 256 };
    };

    Repository &repository()
    {
        static Repository value;
        return value;
    }

    QString payloadFileName(const QString &source)
    {
        if (source.isEmpty())
            return {};
        const QUrl url(source);
        if (url.isLocalFile())
            return url.toLocalFile();
        if (source.startsWith(QLatin1String("qrc:/")))
            return QStringLiteral(":/") + source.mid(5);
        return source;
    }

    QByteArray payloadBytes(const PluginIconPayload &payload)
    {
        if (!payload.data.isEmpty())
            return payload.data;
        QFile file(payloadFileName(payload.source));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    PluginIconPayload normalizedPayload(PluginIconPayload payload)
    {
        if (payload.data.isEmpty() && !payload.source.isEmpty()) {
            payload.data = payloadBytes(payload);
            if (!payload.data.isEmpty())
                payload.source.clear();
        }
        return payload;
    }

    const PluginIconPayload &fallbackPayload()
    {
        static const PluginIconPayload payload = [] {
            PluginIconPayload value;
            value.source = QStringLiteral(":/icons/plugin");
            return normalizedPayload(value);
        }();
        return payload;
    }

    bool isSvg(const PluginIconPayload &payload, const QByteArray &bytes)
    {
        if (payload.mimeType.compare(QStringLiteral("image/svg+xml"), Qt::CaseInsensitive) == 0)
            return true;
        const auto trimmed = bytes.trimmed();
        return trimmed.startsWith("<svg") || (trimmed.startsWith("<?xml") && trimmed.contains("<svg"));
    }

    QImage renderPayload(const PluginIconPayload &payload, QSize requestedSize)
    {
        QByteArray bytes = payloadBytes(payload);
        if (bytes.isEmpty())
            return {};

        requestedSize = requestedSize.isValid() ? requestedSize : QSize(24, 24);
        requestedSize.setWidth(qMax(1, requestedSize.width()));
        requestedSize.setHeight(qMax(1, requestedSize.height()));

        if (isSvg(payload, bytes)) {
            QSvgRenderer renderer(bytes);
            if (!renderer.isValid())
                return {};
            QImage image(requestedSize, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(requestedSize)));
            return image;
        }

        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        if (reader.supportsOption(QImageIOHandler::Size)) {
            const QSize sourceSize = reader.size();
            if (sourceSize.isValid())
                reader.setScaledSize(sourceSize.scaled(requestedSize, Qt::KeepAspectRatio));
        }
        return reader.read();
    }

    QImage renderedPluginIcon(const QString &pluginId, const QSize &requestedSize)
    {
        if (pluginId.isEmpty())
            return {};
        const QSize   target   = requestedSize.isValid() ? requestedSize : QSize(24, 24);
        const QString cacheKey = pluginId + QLatin1Char('@') + QString::number(target.width()) + QLatin1Char('x')
            + QString::number(target.height());

        PluginIconPayload payload;
        {
            auto        &repo = repository();
            QMutexLocker locker(&repo.mutex);
            if (auto *cached = repo.rendered.object(cacheKey))
                return *cached;
            payload = repo.icons.value(pluginId);
        }

        QImage image = renderPayload(payload, target);
        if (image.isNull()) {
            image = renderPayload(fallbackPayload(), target);
        }
        if (!image.isNull()) {
            auto        &repo = repository();
            QMutexLocker locker(&repo.mutex);
            repo.rendered.insert(cacheKey, new QImage(image), qMax(1, int(image.sizeInBytes() / 4096)));
        }
        return image;
    }

    class PluginIconImageProvider final : public QQuickImageProvider {
    public:
        PluginIconImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) { }

        QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
        {
            const QString pluginId = QUrl::fromPercentEncoding(id.toLatin1());
            QImage        image    = renderedPluginIcon(pluginId, requestedSize);
            if (size)
                *size = image.size();
            return image;
        }
    };
} // namespace

void registerPluginIcon(const QString &pluginId, const PluginIconPayload &payload)
{
    if (pluginId.isEmpty())
        return;
    const PluginIconPayload normalized = normalizedPayload(payload);
    auto                   &repo       = repository();
    QMutexLocker            locker(&repo.mutex);
    repo.icons.insert(pluginId, normalized);
    repo.rendered.clear();
}

void unregisterPluginIcon(const QString &pluginId)
{
    auto        &repo = repository();
    QMutexLocker locker(&repo.mutex);
    repo.icons.remove(pluginId);
    repo.rendered.clear();
}

void bindStorageIconToPlugin(const QString &storageId, const QString &pluginId)
{
    if (storageId.isEmpty() || pluginId.isEmpty())
        return;
    auto        &repo = repository();
    QMutexLocker locker(&repo.mutex);
    repo.storagePlugins.insert(storageId, pluginId);
}

void unbindStorageIcon(const QString &storageId)
{
    auto        &repo = repository();
    QMutexLocker locker(&repo.mutex);
    repo.storagePlugins.remove(storageId);
}

QString pluginIdForStorageIcon(const QString &storageId)
{
    auto        &repo = repository();
    QMutexLocker locker(&repo.mutex);
    return repo.storagePlugins.value(storageId);
}

void installPluginIconImageProvider(QQmlEngine *engine)
{
    if (engine)
        engine->addImageProvider(QLatin1String(ProviderId), new PluginIconImageProvider);
}

QString pluginIconSource(const QString &pluginId)
{
    if (pluginId.isEmpty())
        return {};
    return QStringLiteral("image://%1/%2")
        .arg(QLatin1String(ProviderId), QString::fromLatin1(QUrl::toPercentEncoding(pluginId)));
}

QImage pluginIconImage(const QString &pluginId, const QSize &requestedSize)
{
    return renderedPluginIcon(pluginId, requestedSize);
}

} // namespace QtNote
