#include "pluginmetadata.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonValue>
#include <QPair>
#include <QSettings>
#include <QStringView>
#include <QVector>

#include <algorithm>
#include <utility>

namespace QtNote {

namespace {

    struct SemanticVersion {
        QString          major;
        QString          minor;
        QString          patch;
        QVector<QString> prerelease;
    };

    bool isAsciiAlphaNumericOrHyphen(QChar ch)
    {
        const auto code = ch.unicode();
        return (code >= '0' && code <= '9') || (code >= 'A' && code <= 'Z') || (code >= 'a' && code <= 'z')
            || code == '-';
    }

    bool isNumericIdentifier(QStringView value)
    {
        if (value.isEmpty())
            return false;
        for (const auto ch : value) {
            if (!ch.isDigit() || ch.unicode() > '9')
                return false;
        }
        return true;
    }

    bool isValidIdentifier(QStringView value)
    {
        if (value.isEmpty())
            return false;
        for (const auto ch : value) {
            if (!isAsciiAlphaNumericOrHyphen(ch))
                return false;
        }
        return true;
    }

    bool parseCoreIdentifier(QStringView value, QString *destination, QString *error, const QString &component)
    {
        if (!isNumericIdentifier(value)) {
            if (error)
                *error = QStringLiteral("SemVer %1 is not numeric").arg(component);
            return false;
        }
        if (value.size() > 1 && value.front() == QLatin1Char('0')) {
            if (error)
                *error = QStringLiteral("SemVer %1 has a leading zero").arg(component);
            return false;
        }
        *destination = value.toString();
        return true;
    }

    bool parseSemanticVersion(const QString &text, SemanticVersion *version, QString *error)
    {
        if (!version) {
            if (error)
                *error = QStringLiteral("semantic version destination is null");
            return false;
        }
        if (text.isEmpty() || text != text.trimmed()) {
            if (error)
                *error = QStringLiteral("semantic version is empty or contains surrounding whitespace");
            return false;
        }

        const auto firstPlus = text.indexOf(QLatin1Char('+'));
        if (firstPlus >= 0 && text.indexOf(QLatin1Char('+'), firstPlus + 1) >= 0) {
            if (error)
                *error = QStringLiteral("semantic version contains more than one build separator");
            return false;
        }

        const auto precedence = firstPlus < 0 ? QStringView(text) : QStringView(text).left(firstPlus);
        if (firstPlus >= 0) {
            const auto build = QStringView(text).sliced(firstPlus + 1);
            const auto parts = build.split(QLatin1Char('.'));
            for (const auto part : parts) {
                if (!isValidIdentifier(part)) {
                    if (error)
                        *error = QStringLiteral("semantic version contains invalid build metadata");
                    return false;
                }
            }
        }

        const auto firstDash = precedence.indexOf(QLatin1Char('-'));
        const auto core      = firstDash < 0 ? precedence : precedence.left(firstDash);
        const auto coreParts = core.split(QLatin1Char('.'));
        if (coreParts.size() != 3) {
            if (error)
                *error = QStringLiteral("semantic version core must contain major.minor.patch");
            return false;
        }

        SemanticVersion parsed;
        if (!parseCoreIdentifier(coreParts[0], &parsed.major, error, QStringLiteral("major"))
            || !parseCoreIdentifier(coreParts[1], &parsed.minor, error, QStringLiteral("minor"))
            || !parseCoreIdentifier(coreParts[2], &parsed.patch, error, QStringLiteral("patch"))) {
            return false;
        }

        if (firstDash >= 0) {
            const auto prerelease = precedence.sliced(firstDash + 1);
            const auto parts      = prerelease.split(QLatin1Char('.'));
            for (const auto part : parts) {
                if (!isValidIdentifier(part)) {
                    if (error)
                        *error = QStringLiteral("semantic version contains invalid prerelease metadata");
                    return false;
                }
                if (isNumericIdentifier(part) && part.size() > 1 && part.front() == QLatin1Char('0')) {
                    if (error)
                        *error = QStringLiteral("numeric prerelease identifier has a leading zero");
                    return false;
                }
                parsed.prerelease.append(part.toString());
            }
        }

        *version = std::move(parsed);
        if (error)
            error->clear();
        return true;
    }

    int compareNumericIdentifiers(QStringView left, QStringView right)
    {
        if (left.size() != right.size())
            return left.size() < right.size() ? -1 : 1;
        const auto comparison = left.compare(right, Qt::CaseSensitive);
        return comparison < 0 ? -1 : comparison > 0 ? 1 : 0;
    }

    int compareParsedVersions(const SemanticVersion &left, const SemanticVersion &right)
    {
        for (const auto pair : { qMakePair(QStringView(left.major), QStringView(right.major)),
                                 qMakePair(QStringView(left.minor), QStringView(right.minor)),
                                 qMakePair(QStringView(left.patch), QStringView(right.patch)) }) {
            const auto comparison = compareNumericIdentifiers(pair.first, pair.second);
            if (comparison)
                return comparison;
        }

        if (left.prerelease.isEmpty() != right.prerelease.isEmpty())
            return left.prerelease.isEmpty() ? 1 : -1;

        const auto count = std::min(left.prerelease.size(), right.prerelease.size());
        for (qsizetype index = 0; index < count; ++index) {
            const auto leftValue   = QStringView(left.prerelease[index]);
            const auto rightValue  = QStringView(right.prerelease[index]);
            const auto leftNumber  = isNumericIdentifier(leftValue);
            const auto rightNumber = isNumericIdentifier(rightValue);
            if (leftNumber != rightNumber)
                return leftNumber ? -1 : 1;
            const auto comparison = leftNumber ? compareNumericIdentifiers(leftValue, rightValue)
                                               : leftValue.compare(rightValue, Qt::CaseSensitive);
            if (comparison)
                return comparison < 0 ? -1 : 1;
        }
        if (left.prerelease.size() == right.prerelease.size())
            return 0;
        return left.prerelease.size() < right.prerelease.size() ? -1 : 1;
    }

    QString normalizedLocaleName(QString name) { return name.trimmed().replace(QLatin1Char('-'), QLatin1Char('_')); }

    QString localizedString(const QJsonValue &value, const QLocale &locale)
    {
        if (value.isString())
            return value.toString();
        if (!value.isObject())
            return {};

        const auto  object = value.toObject();
        QStringList candidates;
        const auto  localeName = normalizedLocaleName(locale.name());
        if (!localeName.isEmpty() && localeName != QLatin1String("C")) {
            candidates.append(localeName);
            const auto language = localeName.section(QLatin1Char('_'), 0, 0);
            if (!language.isEmpty() && language != localeName)
                candidates.append(language);
        }
        candidates.append(QStringLiteral("en"));

        for (const auto &candidate : std::as_const(candidates)) {
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
                if (normalizedLocaleName(it.key()).compare(candidate, Qt::CaseInsensitive) == 0) {
                    const auto text = it.value().toString().trimmed();
                    if (!text.isEmpty())
                        return text;
                }
            }
        }

        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            const auto text = it.value().toString().trimmed();
            if (!text.isEmpty())
                return text;
        }
        return {};
    }

    bool fail(QString *error, const QString &message)
    {
        if (error)
            *error = message;
        return false;
    }

    bool stringArray(const QJsonValue &value, const QString &fieldName, QStringList *destination, QString *error)
    {
        Q_ASSERT(destination);
        destination->clear();
        if (value.isUndefined() || value.isNull())
            return true;
        if (!value.isArray())
            return fail(error, QStringLiteral("plugin metadata field %1 must be an array of strings").arg(fieldName));

        for (const auto &item : value.toArray()) {
            if (!item.isString())
                return fail(error, QStringLiteral("plugin metadata field %1 must contain only strings").arg(fieldName));
            const auto text = item.toString().trimmed().toLower();
            if (text.isEmpty())
                return fail(error, QStringLiteral("plugin metadata field %1 contains an empty value").arg(fieldName));
            if (!destination->contains(text))
                destination->append(text);
        }
        return true;
    }

} // namespace

bool compareSemanticVersions(const QString &left, const QString &right, int *result, QString *error)
{
    if (!result)
        return fail(error, QStringLiteral("semantic version comparison destination is null"));

    SemanticVersion leftVersion;
    SemanticVersion rightVersion;
    QString         parseError;
    if (!parseSemanticVersion(left, &leftVersion, &parseError))
        return fail(error, QStringLiteral("invalid left semantic version %1: %2").arg(left, parseError));
    if (!parseSemanticVersion(right, &rightVersion, &parseError))
        return fail(error, QStringLiteral("invalid right semantic version %1: %2").arg(right, parseError));

    *result = compareParsedVersions(leftVersion, rightVersion);
    if (error)
        error->clear();
    return true;
}

bool semanticVersionInRange(const QString &version, const QString &minimum, const QString &maximum, QString *error)
{
    int comparison = 0;
    if (!compareSemanticVersions(version, minimum, &comparison, error))
        return false;
    if (comparison < 0)
        return false;
    if (!compareSemanticVersions(version, maximum, &comparison, error))
        return false;
    return comparison <= 0;
}

QLocale pluginMetadataLocale()
{
    const auto configured = QSettings().value(QStringLiteral("language")).toString().trimmed();
    return configured.isEmpty() || configured == QLatin1String("auto") ? QLocale::system() : QLocale(configured);
}

bool pluginMetadataFromJson(const QJsonObject &loaderMetadata, const QLocale &locale, PluginMetadata *metadata,
                            QString *error)
{
    if (!metadata)
        return fail(error, QStringLiteral("metadata destination is null"));

    const auto object = loaderMetadata.value(QStringLiteral("MetaData")).toObject();
    if (object.isEmpty())
        return fail(error, QStringLiteral("missing MetaData object"));
    if (object.value(QStringLiteral("schemaVersion")).toInt() != PluginMetadataSchemaVersion)
        return fail(error, QStringLiteral("unsupported metadata schema version"));

    PluginMetadata parsed;
    parsed.id              = object.value(QStringLiteral("id")).toString().trimmed();
    parsed.name            = localizedString(object.value(QStringLiteral("name")), locale);
    parsed.description     = localizedString(object.value(QStringLiteral("description")), locale);
    parsed.author          = object.value(QStringLiteral("author")).toString();
    parsed.version         = object.value(QStringLiteral("version")).toString().trimmed();
    parsed.minVersion      = object.value(QStringLiteral("minVersion")).toString().trimmed();
    parsed.maxVersion      = object.value(QStringLiteral("maxVersion")).toString().trimmed();
    parsed.homepage        = QUrl(object.value(QStringLiteral("homepage")).toString());
    const auto extraObject = object.value(QStringLiteral("extra")).toObject();
    parsed.extra           = extraObject.toVariantHash();

    const auto desktopEnvironments = object.contains(QStringLiteral("desktopEnvironments"))
        ? object.value(QStringLiteral("desktopEnvironments"))
        : extraObject.value(QStringLiteral("de")); // schema-v2 compatibility with early metadata files
    if (!stringArray(desktopEnvironments, QStringLiteral("desktopEnvironments"), &parsed.desktopEnvironments, error))
        return false;

    const auto features = object.value(QStringLiteral("features")).toArray();
    for (const auto &feature : features) {
        const auto name = feature.toString().trimmed();
        if (!name.isEmpty())
            parsed.features.append(name);
    }

    const auto iconObject = object.value(QStringLiteral("icon")).toObject();
    const auto iconBase64 = iconObject.value(QStringLiteral("base64")).toString().trimmed();
    if (!iconBase64.isEmpty()) {
        const auto decodedIcon = QByteArray::fromBase64Encoding(
            iconBase64.toLatin1(), QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        if (!decodedIcon || decodedIcon.decoded.isEmpty())
            return fail(error, QStringLiteral("failed to decode plugin icon Base64 data"));

        parsed.iconMimeType = iconObject.value(QStringLiteral("mimeType")).toString().trimmed().toLower();
        if (parsed.iconMimeType.isEmpty())
            parsed.iconMimeType = QStringLiteral("application/octet-stream");
        parsed.iconSource = QStringLiteral("data:%1;base64,%2").arg(parsed.iconMimeType, iconBase64);
    }

    if (parsed.id.isEmpty())
        return fail(error, QStringLiteral("plugin id is empty"));
    if (parsed.name.isEmpty())
        return fail(error, QStringLiteral("plugin name is empty"));
    for (const auto &versionField : { qMakePair(QStringLiteral("version"), parsed.version),
                                      qMakePair(QStringLiteral("minVersion"), parsed.minVersion),
                                      qMakePair(QStringLiteral("maxVersion"), parsed.maxVersion) }) {
        SemanticVersion ignored;
        QString         versionError;
        if (!parseSemanticVersion(versionField.second, &ignored, &versionError)) {
            return fail(error,
                        QStringLiteral("plugin %1 is not valid SemVer: %2").arg(versionField.first, versionError));
        }
    }

    int rangeOrder = 0;
    if (!compareSemanticVersions(parsed.minVersion, parsed.maxVersion, &rangeOrder, error))
        return false;
    if (rangeOrder > 0)
        return fail(error, QStringLiteral("plugin minVersion is greater than maxVersion"));

    *metadata = std::move(parsed);
    if (error)
        error->clear();
    return true;
}

} // namespace QtNote
