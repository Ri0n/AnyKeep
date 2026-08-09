#include "notetagline.h"

#include <QRegularExpression>

namespace AnyKeep {
namespace {

    std::optional<NoteTagLineMatch> lineAt(const QString &text, int position)
    {
        if (position < 0 || position > text.size())
            return std::nullopt;
        const int lineEnd = text.indexOf(QLatin1Char('\n'), position);
        const int end     = lineEnd < 0 ? text.size() : lineEnd;

        NoteTagLineMatch match;
        match.lineStart          = position;
        match.lineEnd            = end;
        match.contentStart       = position;
        match.rawLine            = text.mid(position, end - position);
        match.trailingWhitespace = !match.rawLine.isEmpty() && match.rawLine.at(match.rawLine.size() - 1).isSpace();
        return match;
    }

    std::optional<NoteTagLineMatch> plainTextBodyLine(const QString &source)
    {
        const QString text     = NoteTagLine::normalizeLineBreaks(source);
        const int     titleEnd = text.indexOf(QLatin1Char('\n'));
        if (titleEnd < 0)
            return std::nullopt;
        return lineAt(text, titleEnd + 1);
    }

    std::optional<NoteTagLineMatch> editorBodyLine(const QString &source)
    {
        const QString text     = NoteTagLine::normalizeLineBreaks(source);
        const int     titleEnd = text.indexOf(QLatin1Char('\n'));
        if (titleEnd < 0)
            return std::nullopt;

        int position = titleEnd + 1;
        while (position <= text.size()) {
            const auto match = lineAt(text, position);
            if (!match)
                return std::nullopt;
            if (!match->rawLine.trimmed().isEmpty())
                return match;
            if (match->lineEnd >= text.size())
                break;
            position = match->lineEnd + 1;
        }
        return std::nullopt;
    }

    std::optional<NoteTagLineMatch> markdownBodyLine(const QString &source)
    {
        const QString text     = NoteTagLine::normalizeLineBreaks(source);
        const int     titleEnd = text.indexOf(QLatin1Char('\n'));
        if (titleEnd < 0)
            return std::nullopt;

        int  position          = titleEnd + 1;
        bool separatorObserved = false;
        while (position <= text.size()) {
            const auto match = lineAt(text, position);
            if (!match)
                return std::nullopt;
            if (match->rawLine.trimmed().isEmpty()) {
                separatorObserved = true;
            } else {
                // "Title\n#tag" is still the title paragraph in Markdown source.
                // Only "Title\n\n#tag" starts the first body paragraph.
                return separatorObserved ? match : std::nullopt;
            }

            if (match->lineEnd >= text.size())
                break;
            position = match->lineEnd + 1;
        }
        return std::nullopt;
    }

    std::optional<NoteTagLineMatch> parsed(std::optional<NoteTagLineMatch> match)
    {
        if (!match)
            return std::nullopt;
        match->tags = NoteTagLine::parseLine(match->rawLine);
        if (match->tags.isEmpty())
            return std::nullopt;
        return match;
    }

} // namespace

bool NoteTagLine::isValidTagName(const QString &name)
{
    if (name.isEmpty())
        return false;
    static const QRegularExpression valid(QStringLiteral(R"(^[\p{L}\p{N}_.+\-]+$)"),
                                          QRegularExpression::UseUnicodePropertiesOption);
    return valid.match(name).hasMatch();
}

QStringList NoteTagLine::parseLine(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return {};

    const QStringList tokens = trimmed.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    QStringList       tags;
    tags.reserve(tokens.size());
    for (const QString &token : tokens) {
        if (!token.startsWith(QLatin1Char('#')) || token.size() == 1)
            return {};
        const QString tag = token.mid(1);
        if (!isValidTagName(tag))
            return {};
        if (!tags.contains(tag))
            tags.append(tag);
    }
    return tags;
}

QString NoteTagLine::serialize(const QStringList &tags)
{
    QStringList tokens;
    tokens.reserve(tags.size());
    for (const QString &tag : tags) {
        if (isValidTagName(tag))
            tokens.append(QLatin1Char('#') + tag);
    }
    return tokens.join(QLatin1Char(' '));
}

std::optional<NoteTagLineMatch> NoteTagLine::findPlainTextDocumentTagLine(const QString &text)
{
    return parsed(plainTextBodyLine(text));
}

std::optional<NoteTagLineMatch> NoteTagLine::findEditorDocumentTagLine(const QString &text)
{
    return parsed(editorBodyLine(text));
}

std::optional<NoteTagLineMatch> NoteTagLine::findMarkdownDocumentTagLine(const QString &text)
{
    return parsed(markdownBodyLine(text));
}

std::optional<NoteTagLineMatch> NoteTagLine::firstMarkdownDocumentBodyLine(const QString &text)
{
    return markdownBodyLine(text);
}

QString NoteTagLine::normalizeLineBreaks(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text.replace(QChar(QChar::LineSeparator), QLatin1Char('\n'));
    text.replace(QChar(QChar::ParagraphSeparator), QLatin1Char('\n'));
    return text;
}

} // namespace AnyKeep
