#ifndef NEXTCLOUDCATEGORY_H
#define NEXTCLOUDCATEGORY_H

#include <QString>
#include <QStringList>

namespace AnyKeep::NextcloudCategory {

/**
 * Decode a Nextcloud Notes category into the provider's slash-separated path.
 * An empty category denotes the virtual Unsorted folder. Empty path segments
 * are rejected rather than silently changing the hierarchy.
 */
bool decode(const QString &category, QStringList *path, QString *error = nullptr);

/**
 * Encode a global folder path for the Nextcloud Notes category field.
 * Nextcloud uses '/' as a hierarchy separator, so a global folder segment
 * containing it is deliberately rejected instead of being lossy encoded.
 */
bool encode(const QStringList &path, QString *category, QString *error = nullptr);

} // namespace AnyKeep::NextcloudCategory

#endif // NEXTCLOUDCATEGORY_H
