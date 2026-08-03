/*
AnyKeep - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.
*/

#include "tomboynoteformat.h"

#include <QList>
#include <QRegularExpression>
#include <QStringList>

namespace AnyKeep::TomboyNoteFormat {

namespace {

    QString normalizedNewlines(QString text)
    {
        text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
        text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
        text.replace(QChar::LineSeparator, QLatin1Char('\n'));
        text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        return text;
    }

    QString localElementName(const QDomElement &element)
    {
        QString name = element.localName();
        if (!name.isEmpty())
            return name;

        name            = element.tagName();
        const int colon = name.indexOf(QLatin1Char(':'));
        return colon < 0 ? name : name.mid(colon + 1);
    }

    struct RenderedNode {
        QString text;
        bool    block = false;
    };

    enum class ContentBlockKind { Paragraph, List };

    struct ContentBlock {
        QString          text;
        ContentBlockKind kind = ContentBlockKind::Paragraph;
    };

    RenderedNode renderNode(const QDomNode &node, int listDepth);

    QString renderChildren(const QDomNode &parent, int listDepth)
    {
        QString result;
        bool    previousWasBlock = false;

        for (auto child = parent.firstChild(); !child.isNull(); child = child.nextSibling()) {
            const auto rendered = renderNode(child, listDepth);
            if (rendered.text.isEmpty())
                continue;

            if (rendered.block) {
                if (!result.isEmpty() && !result.endsWith(QLatin1Char('\n')))
                    result += QLatin1Char('\n');
                result += rendered.text;
                previousWasBlock = true;
                continue;
            }

            if (previousWasBlock && !result.endsWith(QLatin1Char('\n'))
                && !rendered.text.startsWith(QLatin1Char('\n'))) {
                result += QLatin1Char('\n');
            }
            result += rendered.text;
            previousWasBlock = false;
        }

        return result;
    }

    QString renderListItem(const QDomElement &item, int listDepth)
    {
        QStringList        inlineParts;
        QList<QDomElement> nestedLists;

        for (auto child = item.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (child.isElement() && localElementName(child.toElement()) == QLatin1String("list")) {
                nestedLists.append(child.toElement());
                continue;
            }
            inlineParts.append(renderNode(child, listDepth).text);
        }

        QString inlineText = normalizedNewlines(inlineParts.join(QString()));
        while (inlineText.startsWith(QLatin1Char('\n')))
            inlineText.remove(0, 1);
        while (inlineText.endsWith(QLatin1Char('\n')))
            inlineText.chop(1);

        // Tomboy represents deeper bullet levels by wrapping a list in an
        // otherwise empty list item. Keep the extra list level as Markdown
        // indentation without exposing the wrapper as an empty bullet.
        if (inlineText.trimmed().isEmpty() && !nestedLists.isEmpty()) {
            QStringList nested;
            for (const auto &list : nestedLists)
                nested.append(renderNode(list, listDepth + 1).text);
            return nested.join(QLatin1Char('\n'));
        }

        const QString indent(listDepth * 4, QLatin1Char(' '));
        const auto    lines              = inlineText.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        QString       result             = indent + QStringLiteral("- ") + lines.value(0);
        const QString continuationIndent = indent + QStringLiteral("  ");
        for (int line = 1; line < lines.size(); ++line) {
            result += QLatin1Char('\n');
            result += continuationIndent;
            result += lines.at(line);
        }

        for (const auto &list : nestedLists) {
            const auto nested = renderNode(list, listDepth + 1).text;
            if (!nested.isEmpty())
                result += QLatin1Char('\n') + nested;
        }
        return result;
    }

    RenderedNode renderList(const QDomElement &list, int listDepth)
    {
        QStringList items;
        for (auto child = list.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (!child.isElement() || localElementName(child.toElement()) != QLatin1String("list-item"))
                continue;
            const auto item = renderListItem(child.toElement(), listDepth);
            if (!item.isEmpty())
                items.append(item);
        }
        return { items.join(QLatin1Char('\n')), true };
    }

    RenderedNode renderNode(const QDomNode &node, int listDepth)
    {
        if (node.isText() || node.isCDATASection())
            return { normalizedNewlines(node.nodeValue()), false };
        if (!node.isElement())
            return {};

        const auto element = node.toElement();
        if (localElementName(element) == QLatin1String("list"))
            return renderList(element, listDepth);

        // Tomboy rich-text elements that AnyKeep does not model are deliberately
        // flattened, while their text and any supported nested lists survive.
        return { renderChildren(element, listDepth), false };
    }

    bool startsAtSourceLineBoundary(const QDomNode &node)
    {
        for (auto sibling = node.previousSibling(); !sibling.isNull(); sibling = sibling.previousSibling()) {
            if (sibling.isText() || sibling.isCDATASection()) {
                const QString text      = normalizedNewlines(sibling.nodeValue());
                const int     lineBreak = text.lastIndexOf(QLatin1Char('\n'));
                const QString tail      = lineBreak < 0 ? text : text.mid(lineBreak + 1);
                if (!tail.trimmed().isEmpty())
                    return false;
                if (lineBreak >= 0)
                    return true;
                continue;
            }
            if (sibling.isElement())
                return false;
        }
        return true;
    }

    bool endsAtSourceLineBoundary(const QDomNode &node)
    {
        for (auto sibling = node.nextSibling(); !sibling.isNull(); sibling = sibling.nextSibling()) {
            if (sibling.isText() || sibling.isCDATASection()) {
                const QString text      = normalizedNewlines(sibling.nodeValue());
                const int     lineBreak = text.indexOf(QLatin1Char('\n'));
                const QString head      = lineBreak < 0 ? text : text.left(lineBreak);
                if (!head.trimmed().isEmpty())
                    return false;
                if (lineBreak >= 0)
                    return true;
                continue;
            }
            if (sibling.isElement())
                return false;
        }
        return true;
    }

    int markdownHeadingLevel(const QDomElement &element)
    {
        const QString name = localElementName(element);
        if (name == QLatin1String("huge"))
            return 1;
        if (name == QLatin1String("large"))
            return 2;
        return 0;
    }

    bool isolatedTomboyHeading(const QDomNode &node, int &level, QString &text)
    {
        if (!node.isElement())
            return false;

        const auto element = node.toElement();
        level              = markdownHeadingLevel(element);
        if (level == 0 || !startsAtSourceLineBoundary(node) || !endsAtSourceLineBoundary(node))
            return false;

        text = normalizedNewlines(renderChildren(element, 0)).trimmed();
        return !text.isEmpty() && !text.contains(QLatin1Char('\n'));
    }

    void appendContentBlock(QList<ContentBlock> &blocks, QString text, ContentBlockKind kind)
    {
        while (text.startsWith(QLatin1Char('\n')))
            text.remove(0, 1);
        while (text.endsWith(QLatin1Char('\n')))
            text.chop(1);
        if (text.trimmed().isEmpty())
            return;

        if (kind == ContentBlockKind::List && !blocks.isEmpty() && blocks.constLast().kind == ContentBlockKind::List) {
            blocks.last().text += QLatin1Char('\n') + text;
            return;
        }
        blocks.append({ text, kind });
    }

    void flushParagraphLine(QList<ContentBlock> &blocks, QString &line)
    {
        appendContentBlock(blocks, line, ContentBlockKind::Paragraph);
        line.clear();
    }

    void appendInlineContent(QList<ContentBlock> &blocks, QString &line, const QString &text)
    {
        const auto parts = normalizedNewlines(text).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        line += parts.value(0);
        for (int part = 1; part < parts.size(); ++part) {
            flushParagraphLine(blocks, line);
            line = parts.at(part);
        }
    }

    QString renderContent(const QDomElement &content)
    {
        QList<ContentBlock> blocks;
        QString             line;

        for (auto child = content.firstChild(); !child.isNull(); child = child.nextSibling()) {
            if (child.isElement() && localElementName(child.toElement()) == QLatin1String("list")) {
                flushParagraphLine(blocks, line);
                const auto list = renderList(child.toElement(), 0).text;
                appendContentBlock(blocks, list, ContentBlockKind::List);
                continue;
            }

            int     headingLevel = 0;
            QString headingText;
            if (isolatedTomboyHeading(child, headingLevel, headingText)) {
                flushParagraphLine(blocks, line);
                appendContentBlock(blocks, QString(headingLevel, QLatin1Char('#')) + QLatin1Char(' ') + headingText,
                                   ContentBlockKind::Paragraph);
                continue;
            }

            appendInlineContent(blocks, line, renderNode(child, 0).text);
        }
        flushParagraphLine(blocks, line);

        QString result;
        for (const auto &block : blocks) {
            if (!result.isEmpty())
                result += QStringLiteral("\n\n");
            result += block.text;
        }
        return result;
    }

    bool parseHeadingLine(const QString &line, int &level, QString &text)
    {
        static const QRegularExpression expression(QStringLiteral(R"(^(#{1,2})[ \t]+(.+?)[ \t]*$)"));
        const auto                      match = expression.match(line);
        if (!match.hasMatch())
            return false;

        level = match.capturedLength(1);
        text  = match.captured(2);
        return !text.isEmpty();
    }

    void appendHeading(QDomDocument &dom, QDomElement &content, int level, const QString &text)
    {
        auto heading = dom.createElement(level == 1 ? QStringLiteral("size:huge") : QStringLiteral("size:large"));
        heading.appendChild(dom.createTextNode(text));
        content.appendChild(heading);
    }

    QString removeEmbeddedTitle(QString markdown, const QString &title)
    {
        markdown                      = normalizedNewlines(markdown);
        const QString normalizedTitle = normalizedNewlines(title).trimmed();
        if (normalizedTitle.isEmpty())
            return markdown;

        int titleStart = 0;
        while (titleStart < markdown.size() && markdown.at(titleStart) == QLatin1Char('\n'))
            ++titleStart;

        const int firstBreak = markdown.indexOf(QLatin1Char('\n'), titleStart);
        QString   firstLine
            = firstBreak < 0 ? markdown.mid(titleStart) : markdown.mid(titleStart, firstBreak - titleStart);
        firstLine = firstLine.trimmed();

        // Tomboy variants commonly wrap the embedded title in size:huge. The
        // rich-text renderer intentionally maps an isolated huge element to a
        // Markdown heading, so compare the semantic first line rather than the
        // generated heading marker.
        static const QRegularExpression headingPrefix(QStringLiteral(R"(^#{1,6}[ \t]+)"));
        firstLine.remove(headingPrefix);
        if (firstLine.trimmed() != normalizedTitle)
            return markdown;

        if (firstBreak < 0)
            return QString();

        // Tomboy/Tomboy-ng stores the title inside note-content and normally
        // separates it from the body with two line breaks. Remove at most those
        // two format-owned separators; any additional blank line belongs to the
        // note body and must survive.
        int bodyStart = firstBreak + 1;
        if (bodyStart < markdown.size() && markdown.at(bodyStart) == QLatin1Char('\n'))
            ++bodyStart;
        return markdown.mid(bodyStart);
    }

    int indentationColumns(const QString &indent)
    {
        int columns = 0;
        for (const QChar ch : indent) {
            if (ch == QLatin1Char('\t'))
                columns += 4 - (columns % 4);
            else
                ++columns;
        }
        return columns;
    }

    struct BulletLine {
        int     level = 0;
        QString text;
    };

    bool parseBulletLine(const QString &line, BulletLine &bullet)
    {
        static const QRegularExpression expression(QStringLiteral(R"(^([ \t]*)[-+*][ \t]+(.*)$)"));
        const auto                      match = expression.match(line);
        if (!match.hasMatch())
            return false;

        const int columns = indentationColumns(match.captured(1));
        bullet.level      = columns == 0 ? 0 : (columns + 3) / 4;
        bullet.text       = match.captured(2);
        return true;
    }

    void appendBullet(QDomDocument &dom, QDomElement &content, const BulletLine &bullet)
    {
        QDomElement parent   = content;
        const int   wrappers = qMax(1, bullet.level + 1);
        for (int level = 0; level < wrappers; ++level) {
            auto list = dom.createElement(QStringLiteral("list"));
            auto item = dom.createElement(QStringLiteral("list-item"));
            item.setAttribute(QStringLiteral("dir"), QStringLiteral("ltr"));
            list.appendChild(item);
            parent.appendChild(list);
            parent = item;
        }
        parent.appendChild(dom.createTextNode(bullet.text));
    }

} // namespace

QString markdownFromContent(const QDomElement &content, const QString &title)
{
    return removeEmbeddedTitle(renderContent(content), title);
}

void appendMarkdownContent(QDomDocument &dom, QDomElement &content, const QString &title, const QString &markdown)
{
    auto underlinedTitle = dom.createElement(QStringLiteral("underline"));
    underlinedTitle.appendChild(dom.createTextNode(title));
    content.appendChild(underlinedTitle);

    // Tomboy-ng expects the embedded, underlined title to be followed by an
    // empty line before the body. Keep this separator even for an empty note.
    content.appendChild(dom.createTextNode(QStringLiteral("\n\n")));

    // AnyKeep stores title and body separately. Canonicalize a legacy/draft
    // body that still contains Tomboy's embedded title before writing the
    // inverse representation, otherwise every save would add another copy.
    const QString normalized = removeEmbeddedTitle(markdown, title);
    if (normalized.isEmpty())
        return;
    const auto lines = normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    int previousContentLine = -1;
    for (int line = 0; line < lines.size(); ++line) {
        if (lines.at(line).trimmed().isEmpty())
            continue;

        if (previousContentLine >= 0) {
            const int markdownLineBreaks = line - previousContentLine;
            const int tomboyLineBreaks   = qMax(1, markdownLineBreaks - 1);
            content.appendChild(dom.createTextNode(QString(tomboyLineBreaks, QLatin1Char('\n'))));
        }

        BulletLine bullet;
        int        headingLevel = 0;
        QString    headingText;
        if (parseBulletLine(lines.at(line), bullet))
            appendBullet(dom, content, bullet);
        else if (parseHeadingLine(lines.at(line), headingLevel, headingText))
            appendHeading(dom, content, headingLevel, headingText);
        else
            content.appendChild(dom.createTextNode(lines.at(line)));

        previousContentLine = line;
    }
}

} // namespace AnyKeep::TomboyNoteFormat
