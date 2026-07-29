#include "nextcloudcategory.h"

#include <utility>

namespace QtNote::NextcloudCategory {
namespace {

    void setError(QString *error, const QString &message)
    {
        if (error)
            *error = message;
    }

} // namespace

bool decode(const QString &category, QStringList *path, QString *error)
{
    if (path)
        path->clear();
    if (error)
        error->clear();

    const auto normalized = category.trimmed();
    if (normalized.isEmpty())
        return true;

    const auto  segments = normalized.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    QStringList decoded;
    decoded.reserve(segments.size());
    for (const auto &segment : segments) {
        const auto trimmed = segment.trimmed();
        if (trimmed.isEmpty()) {
            setError(error, QStringLiteral("The Nextcloud category contains an empty path segment"));
            return false;
        }
        decoded.append(trimmed);
    }
    if (path)
        *path = std::move(decoded);
    return true;
}

bool encode(const QStringList &path, QString *category, QString *error)
{
    if (category)
        category->clear();
    if (error)
        error->clear();

    QStringList encoded;
    encoded.reserve(path.size());
    for (const auto &segment : path) {
        const auto trimmed = segment.trimmed();
        if (trimmed.isEmpty()) {
            setError(error, QStringLiteral("A Nextcloud folder path cannot contain an empty segment"));
            return false;
        }
        if (trimmed.contains(QLatin1Char('/'))) {
            setError(error, QStringLiteral("Nextcloud folder names cannot contain '/'"));
            return false;
        }
        encoded.append(trimmed);
    }
    if (category)
        *category = encoded.join(QLatin1Char('/'));
    return true;
}

} // namespace QtNote::NextcloudCategory
