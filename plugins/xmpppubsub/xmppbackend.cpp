#include "xmppbackend.h"

#include <QDateTime>

#include <utility>

namespace AnyKeep {

XmppRemoteNote XmppBackend::makeIndexUpdate(XmppRemoteNote current, const XmppRemoteNote &requested,
                                            QString newRevision, QString originId)
{
    current.folderPath     = requested.folderPath;
    current.parentRevision = current.revision;
    current.revision       = std::move(newRevision);
    current.originId       = std::move(originId);
    current.modified = requested.preserveModified && requested.modified.isValid() ? requested.modified.toUTC()
                                                                                  : QDateTime::currentDateTimeUtc();
    current.format   = QStringLiteral("markdown");
    current.contentPresent = false;
    return current;
}

} // namespace AnyKeep
