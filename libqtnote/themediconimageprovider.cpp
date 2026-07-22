#include "themediconimageprovider.h"

#include "iconutils.h"

#include <QIcon>
#include <QImage>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QUrl>

namespace QtNote {

namespace {
    constexpr auto ProviderId = "qtnoteicons";

    class ThemedIconImageProvider final : public QQuickImageProvider {
    public:
        ThemedIconImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) { }

        QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
        {
            const int slash = id.indexOf(QLatin1Char('/'));
            if (slash <= 0 || slash == id.size() - 1)
                return {};

            const QString themeName    = QUrl::fromPercentEncoding(id.left(slash).toUtf8());
            const QString fallbackName = QUrl::fromPercentEncoding(id.mid(slash + 1).toUtf8());
            auto          fallbackPath = QStringLiteral(":/svg/%1").arg(fallbackName);
            QIcon         icon;
            if (themeName == QStringLiteral("__bundled__"))
                icon = IconUtils::symbolicIcon(fallbackPath);
            else
                icon = IconUtils::themedIcon(themeName, fallbackPath);
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
