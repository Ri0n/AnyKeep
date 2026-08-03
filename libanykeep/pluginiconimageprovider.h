#ifndef PLUGINICONIMAGEPROVIDER_H
#define PLUGINICONIMAGEPROVIDER_H

#include "anykeep_export.h"

#include <QByteArray>
#include <QSize>
#include <QString>

class QImage;
class QQmlEngine;

namespace AnyKeep {

struct ANYKEEP_EXPORT PluginIconPayload {
    QByteArray data;
    QString    mimeType;
    QString    source;

    bool isEmpty() const { return data.isEmpty() && source.isEmpty(); }
};

ANYKEEP_EXPORT void    registerPluginIcon(const QString &pluginId, const PluginIconPayload &payload);
ANYKEEP_EXPORT void    unregisterPluginIcon(const QString &pluginId);
ANYKEEP_EXPORT void    bindStorageIconToPlugin(const QString &storageId, const QString &pluginId);
ANYKEEP_EXPORT void    unbindStorageIcon(const QString &storageId);
ANYKEEP_EXPORT QString pluginIdForStorageIcon(const QString &storageId);
ANYKEEP_EXPORT void    installPluginIconImageProvider(QQmlEngine *engine);
ANYKEEP_EXPORT QString pluginIconSource(const QString &pluginId);
ANYKEEP_EXPORT QImage  pluginIconImage(const QString &pluginId, const QSize &requestedSize = QSize(24, 24));

} // namespace AnyKeep

#endif // PLUGINICONIMAGEPROVIDER_H
