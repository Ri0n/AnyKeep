#include "mediareference.h"

#include <QBuffer>
#include <QImageReader>
#include <QUrl>

namespace AnyKeep {

QString MediaReference::uri() const
{
    return QStringLiteral("anykeep-media:/%1/%2")
        .arg(id.toString(QUuid::WithoutBraces), QString::fromUtf8(QUrl::toPercentEncoding(portableName)));
}

bool MediaReference::isValidVideoThumbnail(const QByteArray &bytes)
{
    if (bytes.size() < 4 || bytes.size() > MediaReference::MaxVideoThumbnailBytes)
        return false;
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return false;
    QImageReader reader(&buffer, "JPEG");
    const auto   size = reader.size();
    return size.width() > 0 && size.height() > 0 && size.width() <= 240 && size.height() <= 240
        && !reader.read().isNull();
}

} // namespace AnyKeep
