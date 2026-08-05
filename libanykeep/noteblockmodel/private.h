#ifndef ANYKEEP_NOTEBLOCKMODEL_PRIVATE_H
#define ANYKEEP_NOTEBLOCKMODEL_PRIVATE_H

#include "../noteblockmodel.h"

#include <QRegularExpression>

namespace AnyKeep::NoteBlockModelPrivate {

inline const QString    TableLineBreakMarker    = QStringLiteral("ANYKEEP_TABLE_LINE_BREAK_7F3A");
inline constexpr int    MaxSerializedImageWidth = 16384;
inline constexpr qint64 MaxAudioDurationMs      = 7LL * 24 * 60 * 60 * 1000;

inline QString normalizedImageAlignment(QString alignment)
{
    alignment = alignment.trimmed().toLower();
    return alignment == QLatin1String("left") || alignment == QLatin1String("right")
            || alignment == QLatin1String("center")
        ? alignment
        : QStringLiteral("center");
}

inline bool isListType(NoteBlockModel::BlockType type)
{
    return type == NoteBlockModel::BulletList || type == NoteBlockModel::CheckList
        || type == NoteBlockModel::NumberedList;
}

inline QString decodeTableCellLineBreaks(QString text)
{
    static const QRegularExpression lineBreak(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    text.replace(lineBreak, QStringLiteral("\n"));
    text.replace(TableLineBreakMarker, QStringLiteral("\n"));
    return text;
}

inline QString decodeListItem(QString text)
{
    text.replace(TableLineBreakMarker, QStringLiteral("\n"));
    while (text.endsWith(QLatin1Char('\n')))
        text.chop(1);
    return text;
}

inline QString coalesceAdjacentMarkdownLinks(QString text)
{
    static const QRegularExpression adjacentLinks(QStringLiteral(R"(\[([^\]]*)\]\(([^)\s]+)\)\[([^\]]*)\]\(\2\))"));
    QString                         previous;
    do {
        previous = text;
        text.replace(adjacentLinks, QStringLiteral("[\\1\\3](\\2)"));
    } while (text != previous);
    return text;
}

} // namespace AnyKeep::NoteBlockModelPrivate

#endif // ANYKEEP_NOTEBLOCKMODEL_PRIVATE_H
