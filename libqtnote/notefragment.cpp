#include "notefragment.h"

#include "notetagline.h"

#include <QCborArray>
#include <QCborMap>
#include <QCborParserError>
#include <QCborValue>

namespace QtNote {
namespace {

    constexpr auto   Schema             = "org.qtnote.fragment";
    constexpr int    MaxBlocks          = 10000;
    constexpr int    MaxListItems       = 100000;
    constexpr int    MaxTableCells      = 100000;
    constexpr int    MaxMedia           = 1000;
    constexpr int    MaxTags            = 1000;
    constexpr int    MaxIndent          = 128;
    constexpr qint64 MaxAudioDurationMs = 7LL * 24 * 60 * 60 * 1000;

    QCborMap encodeMediaReference(const MediaReference &reference)
    {
        QCborMap map;
        map.insert(QStringLiteral("id"), reference.id.toString(QUuid::WithoutBraces));
        map.insert(QStringLiteral("blobId"), reference.blobId);
        map.insert(QStringLiteral("originalName"), reference.originalName);
        map.insert(QStringLiteral("portableName"), reference.portableName);
        map.insert(QStringLiteral("mediaType"), reference.mediaType);
        map.insert(QStringLiteral("size"), reference.size);
        map.insert(QStringLiteral("checksum"), reference.checksum);
        map.insert(QStringLiteral("remoteData"), QCborValue::fromVariant(reference.remoteData));
        return map;
    }

    bool decodeMediaReference(const QCborValue &value, MediaReference *reference, QString *error)
    {
        if (!value.isMap()) {
            *error = QStringLiteral("media reference is not a map");
            return false;
        }

        const QCborMap map = value.toMap();
        const QUuid    id(map.value(QStringLiteral("id")).toString());
        if (id.isNull()) {
            *error = QStringLiteral("media reference has no valid id");
            return false;
        }

        reference->id           = id;
        reference->blobId       = map.value(QStringLiteral("blobId")).toByteArray();
        reference->originalName = map.value(QStringLiteral("originalName")).toString();
        reference->portableName = map.value(QStringLiteral("portableName")).toString();
        reference->mediaType    = map.value(QStringLiteral("mediaType")).toString();
        reference->size         = map.value(QStringLiteral("size")).toInteger(-1);
        reference->checksum     = map.value(QStringLiteral("checksum")).toByteArray();
        reference->remoteData   = map.value(QStringLiteral("remoteData")).toVariant().toMap();
        if (!reference->isValid() || reference->size < 0) {
            *error = QStringLiteral("media reference is incomplete");
            return false;
        }
        return true;
    }

    QCborMap encodeListItem(const NoteFragmentListItem &item)
    {
        QCborMap map;
        map.insert(QStringLiteral("markdown"), item.markdown);
        map.insert(QStringLiteral("indent"), item.indent);
        map.insert(QStringLiteral("kind"), static_cast<int>(item.kind));
        map.insert(QStringLiteral("checked"), item.checked);
        return map;
    }

    bool decodeListItem(const QCborValue &value, NoteFragmentListItem *item, QString *error)
    {
        if (!value.isMap()) {
            *error = QStringLiteral("list item is not a map");
            return false;
        }
        const QCborMap map    = value.toMap();
        const qint64   kind   = map.value(QStringLiteral("kind")).toInteger(-1);
        const qint64   indent = map.value(QStringLiteral("indent")).toInteger(-1);
        if (kind < static_cast<qint64>(NoteFragmentListKind::Bullet)
            || kind > static_cast<qint64>(NoteFragmentListKind::Numbered) || indent < 0 || indent > MaxIndent) {
            *error = QStringLiteral("list item has invalid kind or indentation");
            return false;
        }
        item->markdown = map.value(QStringLiteral("markdown")).toString();
        item->indent   = static_cast<int>(indent);
        item->kind     = static_cast<NoteFragmentListKind>(kind);
        item->checked  = map.value(QStringLiteral("checked")).toBool();
        return true;
    }

    QCborMap encodeBlock(const NoteFragmentBlock &block)
    {
        QCborMap map;
        map.insert(QStringLiteral("type"), static_cast<int>(block.type));
        map.insert(QStringLiteral("markdown"), block.markdown);
        map.insert(QStringLiteral("headingLevel"), block.headingLevel);
        map.insert(QStringLiteral("language"), block.language);
        QCborArray tags;
        for (const QString &tag : block.tags)
            tags.append(tag);
        map.insert(QStringLiteral("tags"), tags);

        QCborArray listItems;
        for (const NoteFragmentListItem &item : block.listItems)
            listItems.append(encodeListItem(item));
        map.insert(QStringLiteral("listItems"), listItems);

        QCborMap table;
        table.insert(QStringLiteral("rows"), block.table.rows);
        table.insert(QStringLiteral("columns"), block.table.columns);
        table.insert(QStringLiteral("headerRows"), block.table.headerRows);
        QCborArray cells;
        for (const QString &cell : block.table.markdownCells)
            cells.append(cell);
        table.insert(QStringLiteral("cells"), cells);
        map.insert(QStringLiteral("table"), table);

        QCborMap image;
        image.insert(QStringLiteral("sourceUri"), block.image.sourceUri);
        image.insert(QStringLiteral("alt"), block.image.alt);
        image.insert(QStringLiteral("width"), block.image.width);
        image.insert(QStringLiteral("alignment"), block.image.alignment);
        map.insert(QStringLiteral("image"), image);

        if (block.type == NoteFragmentBlockType::Audio) {
            QCborMap audio;
            audio.insert(QStringLiteral("sourceUri"), block.audio.sourceUri);
            audio.insert(QStringLiteral("title"), block.audio.title);
            audio.insert(QStringLiteral("durationMs"), block.audio.durationMs);
            audio.insert(QStringLiteral("transcript"), block.audio.transcript);
            map.insert(QStringLiteral("audio"), audio);
        }
        if (block.type == NoteFragmentBlockType::Attachment) {
            QCborMap attachment;
            attachment.insert(QStringLiteral("sourceUri"), block.attachment.sourceUri);
            attachment.insert(QStringLiteral("fileName"), block.attachment.fileName);
            attachment.insert(QStringLiteral("mediaType"), block.attachment.mediaType);
            attachment.insert(QStringLiteral("size"), block.attachment.size);
            map.insert(QStringLiteral("attachment"), attachment);
        }
        return map;
    }

    bool decodeBlock(const QCborValue &value, quint32 version, NoteFragmentBlock *block, QString *error)
    {
        if (!value.isMap()) {
            *error = QStringLiteral("block is not a map");
            return false;
        }
        const QCborMap map         = value.toMap();
        const qint64   type        = map.value(QStringLiteral("type")).toInteger(-1);
        const qint64   maximumType = version >= 6 ? static_cast<qint64>(NoteFragmentBlockType::Attachment)
              : version >= 5                      ? static_cast<qint64>(NoteFragmentBlockType::Audio)
              : version >= 4                      ? static_cast<qint64>(NoteFragmentBlockType::TagLine)
              : version >= 2                      ? static_cast<qint64>(NoteFragmentBlockType::CodeBlock)
                                                  : static_cast<qint64>(NoteFragmentBlockType::BlockQuote);
        if (type < static_cast<qint64>(NoteFragmentBlockType::Text) || type > maximumType) {
            *error = QStringLiteral("block has invalid type");
            return false;
        }
        block->type         = static_cast<NoteFragmentBlockType>(type);
        block->markdown     = map.value(QStringLiteral("markdown")).toString();
        block->headingLevel = static_cast<int>(map.value(QStringLiteral("headingLevel")).toInteger(0));
        block->language     = map.value(QStringLiteral("language")).toString().trimmed().toLower();
        if (version >= 4) {
            const QCborValue tagsValue = map.value(QStringLiteral("tags"));
            if (!tagsValue.isArray()) {
                *error = QStringLiteral("block tags are not an array");
                return false;
            }
            const QCborArray tags = tagsValue.toArray();
            if (tags.size() > MaxTags) {
                *error = QStringLiteral("fragment contains too many tags");
                return false;
            }
            for (const QCborValue &tagValue : tags) {
                if (!tagValue.isString()) {
                    *error = QStringLiteral("block tag is not a string");
                    return false;
                }
                const QString tag = tagValue.toString();
                if (!NoteTagLine::isValidTagName(tag) || block->tags.contains(tag)) {
                    *error = QStringLiteral("block has an invalid or duplicate tag");
                    return false;
                }
                block->tags.append(tag);
            }
        }
        if (block->type == NoteFragmentBlockType::TagLine && block->tags.isEmpty()) {
            *error = QStringLiteral("tag line block has no tags");
            return false;
        }
        if (block->type == NoteFragmentBlockType::Heading && (block->headingLevel < 1 || block->headingLevel > 6)) {
            *error = QStringLiteral("heading has invalid level");
            return false;
        }

        const QCborValue listValue = map.value(QStringLiteral("listItems"));
        if (!listValue.isArray()) {
            *error = QStringLiteral("block list items are not an array");
            return false;
        }
        const QCborArray listItems = listValue.toArray();
        if (listItems.size() > MaxListItems) {
            *error = QStringLiteral("fragment contains too many list items");
            return false;
        }
        for (const QCborValue &itemValue : listItems) {
            NoteFragmentListItem item;
            if (!decodeListItem(itemValue, &item, error))
                return false;
            block->listItems.append(item);
        }
        if (block->type == NoteFragmentBlockType::List && block->listItems.isEmpty()) {
            *error = QStringLiteral("list block has no items");
            return false;
        }

        const QCborValue tableValue = map.value(QStringLiteral("table"));
        if (!tableValue.isMap()) {
            *error = QStringLiteral("block table is not a map");
            return false;
        }
        const QCborMap   table      = tableValue.toMap();
        const qint64     rows       = table.value(QStringLiteral("rows")).toInteger(-1);
        const qint64     columns    = table.value(QStringLiteral("columns")).toInteger(-1);
        const qint64     headerRows = table.value(QStringLiteral("headerRows")).toInteger(-1);
        const QCborValue cellsValue = table.value(QStringLiteral("cells"));
        if (!cellsValue.isArray()) {
            *error = QStringLiteral("table cells are not an array");
            return false;
        }
        const QCborArray cells = cellsValue.toArray();
        if (cells.size() > MaxTableCells || rows < 0 || columns < 0 || headerRows < 0 || rows > MaxTableCells
            || columns > MaxTableCells || headerRows > rows || (rows == 0) != (columns == 0)
            || rows * columns != cells.size()) {
            *error = QStringLiteral("table has invalid geometry");
            return false;
        }
        block->table.rows       = static_cast<int>(rows);
        block->table.columns    = static_cast<int>(columns);
        block->table.headerRows = static_cast<int>(headerRows);
        for (const QCborValue &cell : cells)
            block->table.markdownCells.append(cell.toString());
        if (block->type == NoteFragmentBlockType::Table && block->table.rows == 0) {
            *error = QStringLiteral("table block is empty");
            return false;
        }

        const QCborValue imageValue = map.value(QStringLiteral("image"));
        if (!imageValue.isMap()) {
            *error = QStringLiteral("block image is not a map");
            return false;
        }
        const QCborMap image   = imageValue.toMap();
        block->image.sourceUri = image.value(QStringLiteral("sourceUri")).toString();
        block->image.alt       = image.value(QStringLiteral("alt")).toString();
        block->image.width     = 0;
        block->image.alignment = QStringLiteral("center");
        if (version >= 3 && block->type == NoteFragmentBlockType::Image) {
            const qint64  width     = image.value(QStringLiteral("width")).toInteger(-1);
            const QString alignment = image.value(QStringLiteral("alignment")).toString().trimmed().toLower();
            if (width < 0 || width > 16384
                || (alignment != QLatin1String("left") && alignment != QLatin1String("center")
                    && alignment != QLatin1String("right"))) {
                *error = QStringLiteral("image block has invalid presentation");
                return false;
            }
            block->image.width     = static_cast<int>(width);
            block->image.alignment = alignment;
        }
        if (block->type == NoteFragmentBlockType::Image && block->image.sourceUri.isEmpty()) {
            *error = QStringLiteral("image block has no source URI");
            return false;
        }

        if (version >= 5 && block->type == NoteFragmentBlockType::Audio) {
            const QCborValue audioValue = map.value(QStringLiteral("audio"));
            if (!audioValue.isMap()) {
                *error = QStringLiteral("block audio is not a map");
                return false;
            }
            const QCborMap audio    = audioValue.toMap();
            block->audio.sourceUri  = audio.value(QStringLiteral("sourceUri")).toString();
            block->audio.title      = audio.value(QStringLiteral("title")).toString();
            block->audio.durationMs = audio.value(QStringLiteral("durationMs")).toInteger(-1);
            if (version >= 6)
                block->audio.transcript = audio.value(QStringLiteral("transcript")).toString();
            if (block->audio.sourceUri.isEmpty() || block->audio.durationMs < 0
                || block->audio.durationMs > MaxAudioDurationMs) {
                *error = QStringLiteral("audio block is invalid");
                return false;
            }
        }
        if (version >= 6 && block->type == NoteFragmentBlockType::Attachment) {
            const QCborValue attachmentValue = map.value(QStringLiteral("attachment"));
            if (!attachmentValue.isMap()) {
                *error = QStringLiteral("block attachment is not a map");
                return false;
            }
            const QCborMap attachment   = attachmentValue.toMap();
            block->attachment.sourceUri = attachment.value(QStringLiteral("sourceUri")).toString();
            block->attachment.fileName  = attachment.value(QStringLiteral("fileName")).toString();
            block->attachment.mediaType = attachment.value(QStringLiteral("mediaType")).toString();
            block->attachment.size      = attachment.value(QStringLiteral("size")).toInteger(-1);
            if (block->attachment.sourceUri.isEmpty() || block->attachment.fileName.isEmpty()
                || block->attachment.size < 0) {
                *error = QStringLiteral("attachment block is invalid");
                return false;
            }
        }
        return true;
    }

    NoteFragmentDecodeResult failed(const QString &error)
    {
        NoteFragmentDecodeResult result;
        result.error = error;
        return result;
    }

} // namespace

QByteArray encodeNoteFragment(const NoteFragment &fragment)
{
    QCborMap root;
    root.insert(QStringLiteral("schema"), QString::fromLatin1(Schema));
    root.insert(QStringLiteral("version"), static_cast<qint64>(fragment.version));
    root.insert(QStringLiteral("kind"), static_cast<int>(fragment.kind));
    root.insert(QStringLiteral("sourceFormat"), static_cast<int>(fragment.sourceFormat));

    QCborArray blocks;
    for (const NoteFragmentBlock &block : fragment.blocks)
        blocks.append(encodeBlock(block));
    root.insert(QStringLiteral("blocks"), blocks);

    QCborArray media;
    for (const NoteFragmentMedia &item : fragment.media) {
        QCborMap encoded;
        encoded.insert(QStringLiteral("sourceUri"), item.sourceUri);
        encoded.insert(QStringLiteral("reference"), encodeMediaReference(item.reference));
        encoded.insert(QStringLiteral("data"), item.data);
        media.append(encoded);
    }
    root.insert(QStringLiteral("media"), media);
    return QCborValue(root).toCbor();
}

NoteFragmentDecodeResult decodeNoteFragment(const QByteArray &data)
{
    QCborParserError parserError;
    const QCborValue value = QCborValue::fromCbor(data, &parserError);
    if (parserError.error != QCborError::NoError || !value.isMap())
        return failed(QStringLiteral("invalid CBOR fragment"));

    const QCborMap root = value.toMap();
    if (root.value(QStringLiteral("schema")).toString() != QString::fromLatin1(Schema))
        return failed(QStringLiteral("unknown fragment schema"));

    const qint64 version = root.value(QStringLiteral("version")).toInteger(-1);
    if (version < 1 || version > NoteFragment::CurrentVersion)
        return failed(QStringLiteral("unsupported fragment version"));

    const qint64 kind         = root.value(QStringLiteral("kind")).toInteger(-1);
    const qint64 sourceFormat = root.value(QStringLiteral("sourceFormat")).toInteger(-1);
    if (kind < static_cast<qint64>(NoteFragmentKind::Inline) || kind > static_cast<qint64>(NoteFragmentKind::TableCells)
        || sourceFormat < static_cast<qint64>(NoteFragmentSourceFormat::PlainText)
        || sourceFormat > static_cast<qint64>(NoteFragmentSourceFormat::Markdown)) {
        return failed(QStringLiteral("fragment has invalid metadata"));
    }

    const QCborValue blocksValue = root.value(QStringLiteral("blocks"));
    const QCborValue mediaValue  = root.value(QStringLiteral("media"));
    if (!blocksValue.isArray() || !mediaValue.isArray())
        return failed(QStringLiteral("fragment arrays are missing"));
    const QCborArray blocks = blocksValue.toArray();
    const QCborArray media  = mediaValue.toArray();
    if (blocks.size() > MaxBlocks || media.size() > MaxMedia)
        return failed(QStringLiteral("fragment is too large"));

    NoteFragmentDecodeResult result;
    result.fragment.version      = static_cast<quint32>(version);
    result.fragment.kind         = static_cast<NoteFragmentKind>(kind);
    result.fragment.sourceFormat = static_cast<NoteFragmentSourceFormat>(sourceFormat);
    for (const QCborValue &blockValue : blocks) {
        NoteFragmentBlock block;
        if (!decodeBlock(blockValue, static_cast<quint32>(version), &block, &result.error))
            return result;
        result.fragment.blocks.append(block);
    }
    for (const QCborValue &mediaValue : media) {
        if (!mediaValue.isMap()) {
            result.error = QStringLiteral("media item is not a map");
            return result;
        }
        const QCborMap    mediaMap = mediaValue.toMap();
        NoteFragmentMedia media;
        media.sourceUri = mediaMap.value(QStringLiteral("sourceUri")).toString();
        media.data      = mediaMap.value(QStringLiteral("data")).toByteArray();
        if (media.sourceUri.isEmpty()
            || !decodeMediaReference(mediaMap.value(QStringLiteral("reference")), &media.reference, &result.error)) {
            if (result.error.isEmpty())
                result.error = QStringLiteral("media item has no source URI");
            return result;
        }
        result.fragment.media.append(media);
    }
    return result;
}

} // namespace QtNote
