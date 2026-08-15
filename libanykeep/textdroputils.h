#ifndef TEXTDROPUTILS_H
#define TEXTDROPUTILS_H

#include <QMimeData>
#include <QRegularExpression>
#include <QString>

namespace AnyKeep::TextDropUtils {

struct CodeDetection {
    bool    isCode { false };
    QString language;
};

inline QString codeLanguageForMimeType(QString mimeType)
{
    mimeType = mimeType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    if (mimeType == QLatin1String("text/x-c++src") || mimeType == QLatin1String("text/x-c++hdr")
        || mimeType == QLatin1String("text/x-objc++src"))
        return QStringLiteral("cpp");
    if (mimeType == QLatin1String("text/x-csrc") || mimeType == QLatin1String("text/x-chdr")
        || mimeType == QLatin1String("text/x-c"))
        return QStringLiteral("c");
    if (mimeType == QLatin1String("text/x-python") || mimeType == QLatin1String("text/x-python3")
        || mimeType == QLatin1String("text/x-python-gui"))
        return QStringLiteral("python");
    if (mimeType == QLatin1String("text/x-qml"))
        return QStringLiteral("qml");
    if (mimeType == QLatin1String("text/x-cmake") || mimeType == QLatin1String("text/x-cmake-project"))
        return QStringLiteral("cmake");
    if (mimeType == QLatin1String("text/x-java"))
        return QStringLiteral("java");
    if (mimeType == QLatin1String("text/x-csharp"))
        return QStringLiteral("csharp");
    if (mimeType == QLatin1String("text/x-go"))
        return QStringLiteral("go");
    if (mimeType == QLatin1String("text/rust") || mimeType == QLatin1String("text/x-rustsrc"))
        return QStringLiteral("rust");
    if (mimeType == QLatin1String("text/x-lua"))
        return QStringLiteral("lua");
    if (mimeType == QLatin1String("text/x-makefile"))
        return QStringLiteral("makefile");
    if (mimeType == QLatin1String("application/x-shellscript") || mimeType == QLatin1String("text/x-shellscript"))
        return QStringLiteral("bash");
    if (mimeType == QLatin1String("application/x-yaml") || mimeType == QLatin1String("text/x-yaml"))
        return QStringLiteral("yaml");
    if (mimeType == QLatin1String("application/x-javascript") || mimeType == QLatin1String("text/x-javascript"))
        return QStringLiteral("javascript");
    if (mimeType == QLatin1String("application/xml") || mimeType == QLatin1String("text/xml")
        || mimeType == QLatin1String("application/xhtml+xml") || mimeType == QLatin1String("image/svg+xml"))
        return QStringLiteral("xml");
    if (mimeType == QLatin1String("text/x-glsl") || mimeType.startsWith(QLatin1String("text/x-glsl-"))
        || mimeType == QLatin1String("application/x-glsl"))
        return QStringLiteral("glsl");
    if (mimeType == QLatin1String("text/x-patch"))
        return QStringLiteral("diff");
    return {};
}

inline QString plainText(const QMimeData *mimeData)
{
    if (!mimeData)
        return {};
    const QString text = mimeData->text();
    if (!text.isEmpty())
        return text;
    for (const QString &format : mimeData->formats()) {
        if (format.startsWith(QLatin1String("text/plain"), Qt::CaseInsensitive))
            return QString::fromUtf8(mimeData->data(format));
    }
    for (const QString &format : mimeData->formats()) {
        if (!codeLanguageForMimeType(format).isEmpty())
            return QString::fromUtf8(mimeData->data(format));
    }
    return {};
}

inline QString inferredCodeLanguage(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList               lines         = text.split(QLatin1Char('\n'));
    int                             nonEmptyLines = 0;
    int                             cppScore      = 0;
    int                             cppStatements = 0;
    int                             pythonScore   = 0;
    int                             indentedLines = 0;
    static const QRegularExpression cppWord(QStringLiteral(
        R"(\b(?:auto|class|const|namespace|struct|void|bool|char|double|float|int|long|QString|QByteArray|QUuid)\b)"));
    const QString                   trimmedText = text.trimmed();
    static const QRegularExpression xmlStart(
        QStringLiteral(R"(^(?:<\?xml\b|<!DOCTYPE\b|<[A-Za-z_][A-Za-z0-9_.:-]*(?:\s[^<>]*?)?/?>))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression nestedOrClosingTag(
        QStringLiteral(R"(<(?:/|[A-Za-z_][A-Za-z0-9_.:-]*(?:\s[^<>]*?)?/?>))"));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        ++nonEmptyLines;
        if (line.size() > trimmed.size() && line.at(0).isSpace())
            ++indentedLines;
        if (trimmed.startsWith(QLatin1String("def ")) || trimmed.startsWith(QLatin1String("async def ")))
            pythonScore += 4;
        if (trimmed.startsWith(QLatin1String("class ")))
            pythonScore += 3;
        if (trimmed.startsWith(QLatin1String("import ")) || trimmed.startsWith(QLatin1String("from ")))
            pythonScore += 2;
        if (trimmed.startsWith(QLatin1String("with ")) || trimmed.startsWith(QLatin1String("async with ")))
            pythonScore += 2;
        if (trimmed.endsWith(QLatin1Char(':')))
            ++pythonScore;
        if (trimmed.startsWith(QLatin1String("//")) || trimmed.startsWith(QLatin1String("/*"))
            || trimmed.startsWith(QLatin1String("* ")))
            cppScore += 2;
        if (trimmed.startsWith(QLatin1String("#include")))
            cppScore += 4;
        if (trimmed.contains(QLatin1String("QStringLiteral(")) || trimmed.contains(QLatin1String("QByteArrayLiteral(")))
            cppScore += 3;
        if (trimmed.contains(QLatin1String("::")) || cppWord.match(trimmed).hasMatch())
            ++cppScore;
        if (trimmed.endsWith(QLatin1Char(';')))
            ++cppStatements;
    }
    // Source editors commonly expose a selection as text/plain only. Require
    // several independent language signals so prose, logs, and lightly
    // indented text remain ordinary Markdown drops.
    if (nonEmptyLines >= 2 && cppStatements >= 2 && cppScore + qMin(cppStatements, 3) >= 6)
        return QStringLiteral("cpp");
    if (nonEmptyLines >= 2 && indentedLines >= 2 && pythonScore >= 6)
        return QStringLiteral("python");
    if (xmlStart.match(trimmedText).hasMatch()) {
        const int  firstTagEnd = trimmedText.indexOf(QLatin1Char('>'));
        const bool explicitXml = trimmedText.startsWith(QLatin1String("<?xml"), Qt::CaseInsensitive)
            || trimmedText.startsWith(QLatin1String("<!DOCTYPE"), Qt::CaseInsensitive)
            || trimmedText.left(qMax(0, firstTagEnd)).contains(QLatin1String("xmlns"), Qt::CaseInsensitive);
        const bool hasFollowingTag
            = firstTagEnd >= 0 && nestedOrClosingTag.match(trimmedText, firstTagEnd + 1).hasMatch();
        if (explicitXml || hasFollowingTag)
            return QStringLiteral("xml");
    }
    return {};
}

inline bool looksLikeCode(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList               lines             = text.split(QLatin1Char('\n'));
    int                             nonEmptyLines     = 0;
    int                             signalLines       = 0;
    int                             indentedLines     = 0;
    int                             markdownListLines = 0;
    int                             score             = 0;
    static const QRegularExpression controlFlow(QStringLiteral(R"(^\s*(?:if|for|while|switch|catch)\s*\()"));
    static const QRegularExpression substitution(QStringLiteral(R"(\$[({])"));
    static const QRegularExpression operators(QStringLiteral(R"((?:==|!=|<=|>=|&&|\|\||=>|::|->))"));
    static const QRegularExpression call(QStringLiteral(R"(^\s*[A-Za-z_][A-Za-z0-9_.:-]*\s*\()"));
    static const QRegularExpression markdownList(QStringLiteral(R"(^\s*(?:[-+*]|\d+[.)])\s+)"));

    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        ++nonEmptyLines;
        if (!line.isEmpty() && line.at(0).isSpace())
            ++indentedLines;
        if (markdownList.match(line).hasMatch())
            ++markdownListLines;

        int lineScore = 0;
        if (controlFlow.match(line).hasMatch())
            lineScore += 3;
        if (substitution.match(line).hasMatch())
            lineScore += 3;
        if (operators.match(line).hasMatch())
            lineScore += 2;
        if (call.match(line).hasMatch())
            ++lineScore;
        if (trimmed == QLatin1String("{") || trimmed == QLatin1String("}") || trimmed.endsWith(QLatin1Char(';'))) {
            ++lineScore;
        }
        if (lineScore > 0) {
            ++signalLines;
            score += lineScore;
        }
    }

    // Inline quotations and short prose snippets are never promoted. Lists
    // need especially strong evidence because indentation is part of their
    // ordinary Markdown structure.
    if (nonEmptyLines < 3 || signalLines < 2)
        return false;
    if (indentedLines * 2 >= nonEmptyLines)
        score += 2;
    if (markdownListLines * 2 >= nonEmptyLines)
        score -= 5;
    return score >= 6;
}

inline CodeDetection detectCode(const QMimeData *mimeData)
{
    if (!mimeData)
        return {};
    for (const QString &format : mimeData->formats()) {
        const QString language = codeLanguageForMimeType(format);
        if (!language.isEmpty())
            return { true, language };
    }
    const QString text     = plainText(mimeData);
    const QString language = inferredCodeLanguage(text);
    if (!language.isEmpty())
        return { true, language };
    return { looksLikeCode(text), {} };
}

inline QString codeLanguage(const QMimeData *mimeData) { return detectCode(mimeData).language; }

} // namespace AnyKeep::TextDropUtils

#endif // TEXTDROPUTILS_H
