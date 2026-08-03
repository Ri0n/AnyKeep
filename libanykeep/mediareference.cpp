#include "mediareference.h"

#include <QUrl>

namespace AnyKeep {

QString MediaReference::uri() const
{
    return QStringLiteral("anykeep-media:/%1/%2")
        .arg(id.toString(QUuid::WithoutBraces), QString::fromUtf8(QUrl::toPercentEncoding(portableName)));
}

} // namespace AnyKeep
