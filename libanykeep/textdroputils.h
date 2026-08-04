#ifndef TEXTDROPUTILS_H
#define TEXTDROPUTILS_H

#include <QMimeData>
#include <QRegularExpression>
#include <QString>

namespace AnyKeep::TextDropUtils {

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
    return {};
}

inline QString codeLanguage(const QMimeData *mimeData)
{
    if (!mimeData)
        return {};
    for (const QString &format : mimeData->formats()) {
        const QString language = codeLanguageForMimeType(format);
        if (!language.isEmpty())
            return language;
    }
    return inferredCodeLanguage(plainText(mimeData));
}

} // namespace AnyKeep::TextDropUtils

#endif // TEXTDROPUTILS_H
