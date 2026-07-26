#include "themediconimageprovider.h"

#include "iconutils.h"

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QStringList>
#include <QUrl>

namespace QtNote {

namespace {
    constexpr auto ProviderId = "qtnoteicons";

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
        ThemedIconImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) { }

        QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
        {
            const QStringList parts = id.split(QLatin1Char('/'));
            if (parts.size() < 2 || parts.at(0).isEmpty() || parts.at(1).isEmpty())
                return {};

            const QString themeName    = QUrl::fromPercentEncoding(parts.at(0).toUtf8());
            const QString fallbackName = QUrl::fromPercentEncoding(parts.at(1).toUtf8());
            const QString tintMode
                = parts.size() >= 3 ? QUrl::fromPercentEncoding(parts.at(2).toUtf8()) : QStringLiteral("auto");
            const auto   fallbackPath = QStringLiteral(":/svg/%1").arg(fallbackName);
            const QColor tint         = requestedTint(tintMode);

            QIcon icon;
            if (themeName == QStringLiteral("__bundled__")) {
                icon = tint.isValid() ? IconUtils::tintedSymbolicIcon(fallbackPath, tint)
                                      : IconUtils::symbolicIcon(fallbackPath);
            } else {
                icon = tint.isValid() ? IconUtils::themedIcon(themeName, fallbackPath, tint)
                                      : IconUtils::themedIcon(themeName, fallbackPath);
            }
            if (icon.isNull())
                return {};

            QSize target = requestedSize.isValid() ? requestedSize : QSize(20, 20);
            target.setWidth(qMax(1, target.width()));
            target.setHeight(qMax(1, target.height()));

            const QImage image = icon.pixmap(target, QIcon::Normal, QIcon::Off).toImage();
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

} // namespace QtNote
