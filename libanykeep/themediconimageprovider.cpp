#include "themediconimageprovider.h"

#include <QColor>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QStringList>
#include <QUrl>

namespace AnyKeep {

namespace {
    constexpr auto ProviderId = "anykeepicons";

    QColor requestedTint(const QString &mode)
    {
        if (mode.isEmpty() || mode == QStringLiteral("auto"))
            return {};
        if (mode == QStringLiteral("light"))
            return QColor(Qt::white);
        if (mode == QStringLiteral("dark"))
            return QColor(QStringLiteral("#202124"));
        const QColor color(mode);
        return color.isValid() ? color : QColor();
    }

    class ThemedIconImageProvider final : public QQuickImageProvider {
    public:
        ThemedIconImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

        QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
        {
            const QStringList parts = id.split(QLatin1Char('/'));
            if (parts.size() < 2 || parts.at(0).isEmpty() || parts.at(1).isEmpty())
                return {};

            const QString themeName    = QUrl::fromPercentEncoding(parts.at(0).toUtf8());
            const QString fallbackName = QUrl::fromPercentEncoding(parts.at(1).toUtf8());
            const QString fallbackMode
                = parts.size() >= 3 ? QUrl::fromPercentEncoding(parts.at(2).toUtf8()) : QStringLiteral("original");
            const QString genericThemeName
                = parts.size() >= 5 ? QUrl::fromPercentEncoding(parts.at(4).toUtf8()) : QString();
            const auto fallbackPath    = QStringLiteral(":/svg/%1").arg(fallbackName);
            const bool recolorFallback = fallbackMode != QStringLiteral("original");
            const auto tint            = requestedTint(fallbackMode);

            QSize target = requestedSize.isValid() ? requestedSize : QSize(20, 20);
            target.setWidth(qMax(1, target.width()));
            target.setHeight(qMax(1, target.height()));

            // Keep theme colors: only bundled symbolic fallbacks need tinting.
            // Check the actual pixmap, as a theme engine can return an icon
            // object for which the requested size is unavailable.
            QImage image;
            for (const QString &name : { themeName, genericThemeName }) {
                if (name.isEmpty() || name == QStringLiteral("__bundled__"))
                    continue;
                const QIcon icon = QIcon::fromTheme(name);
                if (!icon.isNull())
                    image = icon.pixmap(target, QIcon::Normal, QIcon::Off).toImage();
                if (!image.isNull())
                    break;
            }
            const bool usingFallback = image.isNull();
            if (usingFallback)
                image = QIcon(fallbackPath).pixmap(target, QIcon::Normal, QIcon::Off).toImage();
            if (image.isNull())
                return {};
            if (usingFallback && recolorFallback) {
                const QColor effectiveTint
                    = tint.isValid() ? tint : QGuiApplication::palette().color(QPalette::WindowText);
                QPainter painter(&image);
                painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                painter.fillRect(image.rect(), effectiveTint);
            }
            if (size)
                *size = image.size();
            return image;
        }
    };
}

void installThemedIconImageProvider(QQmlEngine *engine)
{
    if (!engine)
        return;
    engine->addImageProvider(QLatin1String(ProviderId), new ThemedIconImageProvider);
}

} // namespace AnyKeep
