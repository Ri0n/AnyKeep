#ifndef PLUGINICONIMAGEPROVIDER_H
#define PLUGINICONIMAGEPROVIDER_H

#include "qtnote_export.h"

#include <QByteArray>
#include <QSize>
#include <QString>

class QImage;
class QQmlEngine;

namespace QtNote {

struct QTNOTE_EXPORT PluginIconPayload {
    QByteArray data;
    QString    mimeType;
    QString    source;

    bool isEmpty() const { return data.isEmpty() && source.isEmpty(); }
};

QTNOTE_EXPORT void    registerPluginIcon(const QString &pluginId, const PluginIconPayload &payload);
QTNOTE_EXPORT void    unregisterPluginIcon(const QString &pluginId);
QTNOTE_EXPORT void    bindStorageIconToPlugin(const QString &storageId, const QString &pluginId);
QTNOTE_EXPORT void    unbindStorageIcon(const QString &storageId);
QTNOTE_EXPORT QString pluginIdForStorageIcon(const QString &storageId);
QTNOTE_EXPORT void    installPluginIconImageProvider(QQmlEngine *engine);
QTNOTE_EXPORT QString pluginIconSource(const QString &pluginId);
QTNOTE_EXPORT QImage  pluginIconImage(const QString &pluginId, const QSize &requestedSize = QSize(24, 24));

} // namespace QtNote

#endif // PLUGINICONIMAGEPROVIDER_H
