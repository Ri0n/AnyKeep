#ifndef ANYKEEP_PLUGINMETADATA_H
#define ANYKEEP_PLUGINMETADATA_H

#include "anykeep_export.h"

#include <QByteArray>
#include <QJsonObject>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantHash>
#include <QtGlobal>

namespace AnyKeep {

inline constexpr int PluginMetadataSchemaVersion = 2;

struct PluginMetadata {
    QByteArray   iconData;
    QString      iconMimeType;
    QString      id;
    QString      name;
    QString      description;
    QString      author;
    QString      version;
    QString      minVersion;
    QString      maxVersion;
    QUrl         homepage;
    QStringList  features;
    QStringList  desktopEnvironments;
    QVariantHash extra;
};

ANYKEEP_EXPORT QLocale pluginMetadataLocale();
ANYKEEP_EXPORT bool    compareSemanticVersions(const QString &left, const QString &right, int *result,
                                              QString *error = nullptr);
ANYKEEP_EXPORT bool    semanticVersionInRange(const QString &version, const QString &minimum, const QString &maximum,
                                             QString *error = nullptr);
ANYKEEP_EXPORT bool    pluginMetadataFromJson(const QJsonObject &loaderMetadata, const QLocale &locale,
                                             PluginMetadata *metadata, QString *error = nullptr);

} // namespace AnyKeep

#endif // ANYKEEP_PLUGINMETADATA_H
