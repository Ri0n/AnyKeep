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
            const auto fallbackPath    = QStringLiteral(":/svg/%1").arg(fallbackName);
            const bool recolorFallback = fallbackMode != QStringLiteral("original");
            const auto tint            = requestedTint(fallbackMode);

            QSize target = requestedSize.isValid() ? requestedSize : QSize(20, 20);
            target.setWidth(qMax(1, target.width()));
            target.setHeight(qMax(1, target.height()));

            QIcon icon;
            if (themeName != QStringLiteral("__bundled__"))
                icon = QIcon::fromTheme(themeName);

            bool usingFallback = icon.isNull();
            if (usingFallback)
                icon = QIcon(fallbackPath);
            if (icon.isNull())
                return {};

            // Render the SVG fallback directly at the final requested size.
            // Building a tinted QIcon first would turn it into a small set of
            // raster pixmaps; fractional display scaling could then select a
            // neighbouring size and resample it a second time.
            QImage image = icon.pixmap(target, QIcon::Normal, QIcon::Off).toImage();
            if (image.isNull() && !usingFallback) {
                icon          = QIcon(fallbackPath);
                usingFallback = true;
                image         = icon.pixmap(target, QIcon::Normal, QIcon::Off).toImage();
            }
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
