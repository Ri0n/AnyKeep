#include "notetitleresolver.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QUrl>

namespace AnyKeep::NoteTitleResolver {

namespace {
    QString htmlAttribute(const QString &attributes, const QString &name)
    {
        const QRegularExpression expression(
            QStringLiteral(R"((?:^|\s)%1\s*=\s*(["'])(.*?)\1)").arg(QRegularExpression::escape(name)),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
        const auto match = expression.match(attributes);
        return match.hasMatch() ? QTextDocumentFragment::fromHtml(match.captured(2)).toPlainText() : QString();
    }

    QString audioDisplayTitle(const QString &source)
    {
        static const QRegularExpression audio(QStringLiteral(R"(^\s*<audio\b([^>]*)>.*?</audio>\s*$)"),
                                              QRegularExpression::CaseInsensitiveOption
                                                  | QRegularExpression::DotMatchesEverythingOption);
        const auto                      match = audio.match(source);
        if (!match.hasMatch())
            return {};
        QString result = htmlAttribute(match.captured(1), QStringLiteral("title")).trimmed();
        if (result.isEmpty())
            result = QFileInfo(QUrl(htmlAttribute(match.captured(1), QStringLiteral("src"))).path()).fileName();
        return result.trimmed();
    }

    QString htmlImageDisplayTitle(const QString &source)
    {
        if (!source.contains(QStringLiteral("<img"), Qt::CaseInsensitive))
            return {};
        QTextDocument document;
        document.setHtml(source);
        QString result;
        bool    foundImage = false;
        for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
            for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
                const auto textFragment = fragment.fragment();
                if (!textFragment.isValid() || !textFragment.charFormat().isImageFormat())
                    continue;
                foundImage       = true;
                const auto image = textFragment.charFormat().toImageFormat();
                QString    label = image.property(QTextFormat::ImageAltText).toString();
                if (label.isEmpty())
                    label = image.property(QTextFormat::ImageTitle).toString();
                if (label.isEmpty())
                    label = QFileInfo(QUrl(image.name()).path()).fileName();
                result += label;
            }
        }
        return foundImage ? result.trimmed() : QString();
    }

    QString markdownDisplayTitle(const QString &source)
    {
        const QString audioTitle = audioDisplayTitle(source);
        if (!audioTitle.isNull())
            return audioTitle;
        const QString htmlImageTitle = htmlImageDisplayTitle(source);
        if (!htmlImageTitle.isNull())
            return htmlImageTitle;
        if (!source.contains(QLatin1Char('[')))
            return source;

        QTextDocument document;
        document.setMarkdown(source);
        QString result;
        bool    foundImage = false;
        for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
            for (auto fragment = block.begin(); !fragment.atEnd(); ++fragment) {
                const auto textFragment = fragment.fragment();
                if (!textFragment.isValid())
                    continue;
                const auto format = textFragment.charFormat();
                if (!format.isImageFormat()) {
                    result += textFragment.text();
                    continue;
                }
                foundImage       = true;
                const auto image = format.toImageFormat();
                QString    label = image.property(QTextFormat::ImageAltText).toString();
                if (label.isEmpty())
                    label = image.property(QTextFormat::ImageTitle).toString();
                if (label.isEmpty())
                    label = QFileInfo(QUrl(image.name()).path()).fileName();
                result += label;
            }
        }
        return foundImage ? result.trimmed() : source;
    }

    QString codeLanguageName(QString language)
    {
        language = language.trimmed().toLower();
        static const QHash<QString, QString> names {
            { QStringLiteral("c"), QStringLiteral("C") },
            { QStringLiteral("cpp"), QStringLiteral("C++") },
            { QStringLiteral("c++"), QStringLiteral("C++") },
            { QStringLiteral("csharp"), QStringLiteral("C#") },
            { QStringLiteral("cs"), QStringLiteral("C#") },
            { QStringLiteral("css"), QStringLiteral("CSS") },
            { QStringLiteral("html"), QStringLiteral("HTML") },
            { QStringLiteral("javascript"), QStringLiteral("JavaScript") },
            { QStringLiteral("js"), QStringLiteral("JavaScript") },
            { QStringLiteral("json"), QStringLiteral("JSON") },
            { QStringLiteral("markdown"), QStringLiteral("Markdown") },
            { QStringLiteral("md"), QStringLiteral("Markdown") },
            { QStringLiteral("php"), QStringLiteral("PHP") },
            { QStringLiteral("python"), QStringLiteral("Python") },
            { QStringLiteral("py"), QStringLiteral("Python") },
            { QStringLiteral("qml"), QStringLiteral("QML") },
            { QStringLiteral("sql"), QStringLiteral("SQL") },
            { QStringLiteral("typescript"), QStringLiteral("TypeScript") },
            { QStringLiteral("ts"), QStringLiteral("TypeScript") },
            { QStringLiteral("xml"), QStringLiteral("XML") },
            { QStringLiteral("yaml"), QStringLiteral("YAML") },
            { QStringLiteral("yml"), QStringLiteral("YAML") },
        };
        return names.value(language, language.isEmpty() ? QString() : language.toUpper());
    }
}

QString displayTitle(const QString &title, const QString &body, Note::Format format)
{
    const QString explicitTitle = (format == Note::Markdown ? markdownDisplayTitle(title) : title).trimmed();
    if (!explicitTitle.isEmpty())
        return explicitTitle;

    if (format == Note::Markdown) {
        static const QRegularExpression fencedCode(QStringLiteral(R"(^\s*`{3,}[ \t]*([^\s`]*)[^\n]*\n)"));
        const auto                      match = fencedCode.match(body);
        if (match.hasMatch()) {
            const QString language = codeLanguageName(match.captured(1));
            if (!language.isEmpty()) {
                return QCoreApplication::translate("NoteTitleResolver", "%1 code").arg(language);
            }
            return QCoreApplication::translate("NoteTitleResolver", "Code snippet");
        }
    }
    return {};
}

} // namespace AnyKeep::NoteTitleResolver
