#include "../noteblockmodel.h"

#include <climits>

namespace AnyKeep {

QVariantMap NoteBlockModel::findText(const QString &text, const QVariantMap &after, bool backwards,
                                     bool caseSensitive) const
{
    if (text.isEmpty() || blocks_.isEmpty())
        return {};

    struct SearchField {
        int     blockIndex     = -1;
        int     listItemIndex  = -1;
        int     tableCellIndex = -1;
        QString editorField;
        QString text;
    };
    struct Match {
        SearchField field;
        int         fieldOrder = -1;
        int         start      = -1;
    };

    QList<SearchField> fields;
    for (int blockIndex = 0; blockIndex < blocks_.size(); ++blockIndex) {
        const auto &block = blocks_.at(blockIndex);
        switch (block.type) {
        case Text:
            fields.append({ blockIndex, -1, -1, QStringLiteral("text"), block.text });
            break;
        case Heading:
            fields.append({ blockIndex, -1, -1, QStringLiteral("heading"), block.text });
            break;
        case BlockQuote:
            fields.append({ blockIndex, -1, -1, QStringLiteral("blockquote"), block.text });
            break;
        case CodeBlock:
            fields.append({ blockIndex, -1, -1, QStringLiteral("code"), block.text });
            break;
        case BulletList:
        case CheckList:
        case NumberedList:
            for (int item = 0; item < block.items.size(); ++item)
                fields.append({ blockIndex, item, -1, QStringLiteral("listItem"), block.items.at(item) });
            break;
        case Table:
            for (int cell = 0; cell < block.cells.size(); ++cell)
                fields.append({ blockIndex, -1, cell, QStringLiteral("tableCell"), block.cells.at(cell) });
            break;
        case Image:
        case Audio:
        case Attachment:
            // Structural media labels are not QTextDocument editors, so a
            // text-search result cannot expose a visible selection there yet.
            break;
        case TagLine:
            // Tag chips are edited by a dedicated structural delegate. Search
            // result selection currently addresses QTextDocument editors only.
            break;
        }
    }

    const auto   sensitivity = caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    QList<Match> matches;
    for (int fieldOrder = 0; fieldOrder < fields.size(); ++fieldOrder) {
        const auto &field = fields.at(fieldOrder);
        int         from  = 0;
        while (from <= field.text.size()) {
            const int position = field.text.indexOf(text, from, sensitivity);
            if (position < 0)
                break;
            matches.append({ field, fieldOrder, position });
            from = position + qMax(1, text.size());
        }
    }
    if (matches.isEmpty())
        return {};

    auto sameAddress = [](const SearchField &field, const QVariantMap &address) {
        return field.blockIndex == address.value(QStringLiteral("blockIndex"), -1).toInt()
            && field.listItemIndex == address.value(QStringLiteral("listItemIndex"), -1).toInt()
            && field.tableCellIndex == address.value(QStringLiteral("tableCellIndex"), -1).toInt()
            && field.editorField == address.value(QStringLiteral("field")).toString();
    };

    int  selected = backwards ? matches.size() - 1 : 0;
    bool wrapped  = false;
    if (!after.isEmpty()) {
        int afterFieldOrder = -1;
        for (int i = 0; i < fields.size(); ++i) {
            if (sameAddress(fields.at(i), after)) {
                afterFieldOrder = i;
                break;
            }
        }

        if (afterFieldOrder >= 0) {
            const int afterStart = after.value(QStringLiteral("start"), backwards ? INT_MAX : -1).toInt();
            selected             = -1;
            if (backwards) {
                for (int i = matches.size() - 1; i >= 0; --i) {
                    const auto &match = matches.at(i);
                    if (match.fieldOrder < afterFieldOrder
                        || (match.fieldOrder == afterFieldOrder && match.start < afterStart)) {
                        selected = i;
                        break;
                    }
                }
                if (selected < 0) {
                    selected = matches.size() - 1;
                    wrapped  = true;
                }
            } else {
                for (int i = 0; i < matches.size(); ++i) {
                    const auto &match = matches.at(i);
                    if (match.fieldOrder > afterFieldOrder
                        || (match.fieldOrder == afterFieldOrder && match.start > afterStart)) {
                        selected = i;
                        break;
                    }
                }
                if (selected < 0) {
                    selected = 0;
                    wrapped  = true;
                }
            }
        }
    }

    const auto &match = matches.at(selected);
    QVariantMap result;
    result.insert(QStringLiteral("blockIndex"), match.field.blockIndex);
    result.insert(QStringLiteral("listItemIndex"), match.field.listItemIndex);
    result.insert(QStringLiteral("tableCellIndex"), match.field.tableCellIndex);
    result.insert(QStringLiteral("field"), match.field.editorField);
    result.insert(QStringLiteral("start"), match.start);
    result.insert(QStringLiteral("length"), text.size());
    result.insert(QStringLiteral("wrapped"), wrapped);
    return result;
}
} // namespace AnyKeep
