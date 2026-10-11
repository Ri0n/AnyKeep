#ifndef NEXTCLOUDMEDIALINKS_H
#define NEXTCLOUDMEDIALINKS_H

#include <QHash>
#include <QString>
#include <QStringList>

namespace AnyKeep {

// Translation at the provider boundary; canonical Markdown may contain
// anykeep-media: links but Nextcloud Notes must only receive native relative
// links. This codec does not upload or download files.
class NextcloudMediaLinks {
public:
    // Rewrite only Markdown/HTML link targets, never incidental prose or code.
    // Mapping keys are canonical anykeep-media: URIs; values are Notes-relative
    // server paths returned by the attachment API.
    static QString toNative(const QString &markdown, const QHash<QString, QString> &paths, QString *error = nullptr);

    // Convert known native-relative links back to stable anykeep-media URIs.
    // Unrecognized external/native links are preserved without modification.
    static QString toCanonical(const QString &markdown, const QHash<QString, QString> &canonicalUris);

    // Enumerate native relative paths referenced by Markdown and HTML media
    // tags, preserving order but discarding duplicates and unsafe URLs.
    static QStringList referencedNativePaths(const QString &markdown);

    static bool isSafeNativePath(const QString &path);
};

} // namespace AnyKeep

#endif // NEXTCLOUDMEDIALINKS_H
