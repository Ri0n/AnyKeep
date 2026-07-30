#ifndef QTNOTE_PLUGINMETADATA_H
#define QTNOTE_PLUGINMETADATA_H

#include "qtnote_export.h"

#include <QIcon>
#include <QJsonObject>
#include <QLocale>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantHash>
#include <QtGlobal>

namespace QtNote {

inline constexpr int PluginMetadataSchemaVersion = 2;

struct PluginMetadata {
    QIcon        icon;
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

QTNOTE_EXPORT QLocale pluginMetadataLocale();
QTNOTE_EXPORT bool    compareSemanticVersions(const QString &left, const QString &right, int *result,
                                              QString *error = nullptr);
QTNOTE_EXPORT bool    semanticVersionInRange(const QString &version, const QString &minimum, const QString &maximum,
                                             QString *error = nullptr);
QTNOTE_EXPORT bool    pluginMetadataFromJson(const QJsonObject &loaderMetadata, const QLocale &locale,
                                             PluginMetadata *metadata, QString *error = nullptr);

} // namespace QtNote

#endif // QTNOTE_PLUGINMETADATA_H
