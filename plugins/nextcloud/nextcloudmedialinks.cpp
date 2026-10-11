#include "nextcloudmedialinks.h"

#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace AnyKeep {
namespace {

struct LinkTarget {
    qsizetype offset { 0 };
    qsizetype length { 0 };
    QString raw;
};

QList<LinkTarget> linkTargets(const QString &markdown)
{
    QList<LinkTarget> result;
    // Preserve source syntax and replace only the URL capture. Native Notes
    // clients commonly use Markdown image/link syntax and HTML media tags.
    // URLs with literal spaces should be percent-escaped in Markdown.
    static const QRegularExpression markdownLink(
        QStringLiteral(R"((?:!?\[[^\]\r\n]*\]\(\s*<?([^\s<>\)\r\n]+)>?(?:\s+"[^"]*")?\s*\)))"));
    static const QRegularExpression htmlLink(
        QStringLiteral(R"(<(?:img|audio|video|source|a)\b[^>]*?\b(?:src|href)\s*=\s*(["'])(.*?)\1)"),
        QRegularExpression::CaseInsensitiveOption);

    for (int index = 0; index < 2; ++index) {
        const auto &expression = index == 0 ? markdownLink : htmlLink;
        auto match = expression.globalMatch(markdown);
        while (match.hasNext()) {
            const auto item = match.next();
            const int capture = index == 0 ? 1 : 2;
            const auto target = item.captured(capture);
            if (!target.isEmpty())
                result.append({ item.capturedStart(capture), item.capturedLength(capture), target });
        }
    }

    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.offset < right.offset;
    });
    return result;
}

bool inFencedCode(const QString &markdown, qsizetype offset)
{
    bool fenced = false;
    QChar delimiter;
    qsizetype start = 0;
    while (start < offset) {
        const qsizetype finish = markdown.indexOf(QLatin1Char('\n'), start);
        const qsizetype lineEnd = finish < 0 ? markdown.size() : finish;
        const QString line = markdown.mid(start, lineEnd - start).trimmed();
        if (line.startsWith(QStringLiteral("```")) || line.startsWith(QStringLiteral("~~~"))) {
            const QChar current = line.at(0);
            if (!fenced) {
                fenced = true;
                delimiter = current;
            } else if (current == delimiter) {
                fenced = false;
            }
        }
        if (finish < 0 || finish >= offset)
            break;
        start = finish + 1;
    }
    return fenced;
}

QString decodedSafePath(const QString &path)
{
    const QString decoded = QUrl::fromPercentEncoding(path.toUtf8());
    if (decoded.isEmpty() || decoded.startsWith(QLatin1Char('/'))
        || decoded.startsWith(QLatin1Char('\\')) || decoded.contains(QLatin1Char('\\'))
        || decoded.contains(QLatin1Char('?')) || decoded.contains(QLatin1Char('#'))
        || decoded.contains(QLatin1Char('\r')) || decoded.contains(QLatin1Char('\n'))
        || decoded.contains(QChar::Null) || decoded.contains(QLatin1Char(':'))) {
        return {};
    }
    const auto parts = decoded.split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (part.isEmpty() || part == QStringLiteral("..") || part == QStringLiteral("."))
            return {};
    }
    return decoded;
}

} // namespace

bool NextcloudMediaLinks::isSafeNativePath(const QString &path)
{
    return !decodedSafePath(path).isEmpty();
}

QString NextcloudMediaLinks::toNative(const QString &markdown, const QHash<QString, QString> &paths, QString *error)
{
    if (error)
        error->clear();
    QString result = markdown;
    const auto links = linkTargets(markdown);
    for (auto it = links.crbegin(); it != links.crend(); ++it) {
        if (inFencedCode(markdown, it->offset) || !it->raw.startsWith(QStringLiteral("anykeep-media:"),
                                                                       Qt::CaseInsensitive))
            continue;
        const auto target = paths.constFind(it->raw);
        if (target == paths.cend() || !isSafeNativePath(target.value())) {
            if (error)
                *error = QStringLiteral("Missing or unsafe native Nextcloud path for a media reference");
            return {};
        }
        const QString decoded = decodedSafePath(target.value());
        const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(decoded, QByteArrayLiteral("/.-_~")));
        result.replace(it->offset, it->length, encoded);
    }
    return result;
}

QStringList NextcloudMediaLinks::referencedNativePaths(const QString &markdown)
{
    QSet<QString> seen;
    QStringList result;
    for (const auto &link : linkTargets(markdown)) {
        if (inFencedCode(markdown, link.offset))
            continue;
        const QString decoded = decodedSafePath(link.raw);
        if (decoded.isEmpty() || seen.contains(decoded))
            continue;
        seen.insert(decoded);
        result.append(decoded);
    }
    return result;
}

} // namespace AnyKeep
