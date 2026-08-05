#include "../noteblockmodel.h"
#include "private.h"

namespace AnyKeep {
using namespace NoteBlockModelPrivate;

void NoteBlockModel::setTableCell(int row, int cell, const QString &text)
{
    if (row < 0 || row >= blocks_.size() || cell < 0 || cell >= blocks_[row].cells.size())
        return;
    const QString value = coalesceAdjacentMarkdownLinks(decodeTableCellLineBreaks(text));
    if (blocks_[row].cells[cell] == value)
        return;
    const QString before     = blocks_[row].cells[cell];
    blocks_[row].cells[cell] = value;
    emit scalarEdited(row, CellsRole, cell, before, value);
    changed(row, { CellsRole });
}

void NoteBlockModel::insertTableRow(int row, int tableRow)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].type != Table)
        return;
    auto     &block = blocks_[row];
    const int rows  = block.columns > 0 ? block.cells.size() / block.columns : 0;
    tableRow        = qBound(0, tableRow, rows);
    for (int column = 0; column < block.columns; ++column)
        block.cells.insert(tableRow * block.columns, QString());
    changed(row, { CellsRole });
}

void NoteBlockModel::removeTableRow(int row, int tableRow)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].type != Table)
        return;
    const auto &block = blocks_[row];
    const int   rows  = block.columns > 0 ? block.cells.size() / block.columns : 0;
    if (rows <= 1 || tableRow < 0 || tableRow >= rows)
        return;
    removeTableRows(row, tableRow, tableRow);
}

void NoteBlockModel::removeTableRows(int row, int firstRow, int lastRow)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].type != Table)
        return;
    auto     &block = blocks_[row];
    const int rows  = block.columns > 0 ? block.cells.size() / block.columns : 0;
    if (rows <= 1)
        return;
    firstRow              = qBound(0, firstRow, rows - 1);
    lastRow               = qBound(firstRow, lastRow, rows - 1);
    const int removeCount = qMin(lastRow - firstRow + 1, rows - 1);
    for (int cell = 0; cell < removeCount * block.columns; ++cell)
        block.cells.removeAt(firstRow * block.columns);
    changed(row, { CellsRole });
}

void NoteBlockModel::insertTableColumn(int row, int column)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].type != Table)
        return;
    auto     &block = blocks_[row];
    const int rows  = block.columns > 0 ? block.cells.size() / block.columns : 0;
    column          = qBound(0, column, block.columns);
    for (int tableRow = rows - 1; tableRow >= 0; --tableRow)
        block.cells.insert(tableRow * block.columns + column, QString());
    ++block.columns;
    changed(row, { CellsRole });
}

void NoteBlockModel::removeTableColumn(int row, int column)
{
    if (row < 0 || row >= blocks_.size() || blocks_[row].type != Table)
        return;
    auto &block = blocks_[row];
    if (block.columns <= 1 || column < 0 || column >= block.columns)
        return;
    const int rows = block.cells.size() / block.columns;
    for (int tableRow = rows - 1; tableRow >= 0; --tableRow)
        block.cells.removeAt(tableRow * block.columns + column);
    --block.columns;
    changed(row, { CellsRole });
}

bool NoteBlockModel::moveTableColumn(int row, int from, int to)
{
    if (row < 0 || row >= blocks_.size() || blocks_.at(row).type != Table)
        return false;
    auto &block = blocks_[row];
    if (block.columns <= 1 || from < 0 || from >= block.columns)
        return false;
    to = qBound(0, to, block.columns - 1);
    if (from == to)
        return false;

    const int rows = block.cells.size() / block.columns;
    for (int tableRow = 0; tableRow < rows; ++tableRow) {
        const int     source = tableRow * block.columns + from;
        const QString cell   = block.cells.takeAt(source);
        block.cells.insert(tableRow * block.columns + to, cell);
    }
    changed(row, { CellsRole });
    return true;
}

void NoteBlockModel::setImageUrl(int row, const QString &url) { setData(index(row), url, UrlRole); }
void NoteBlockModel::setImageAlt(int row, const QString &alt) { setData(index(row), alt, AltRole); }
void NoteBlockModel::setImageWidth(int row, int width) { setData(index(row), width, ImageWidthRole); }
void NoteBlockModel::setImageAlignment(int row, const QString &alignment)
{
    setData(index(row), alignment, ImageAlignmentRole);
}

void NoteBlockModel::appendImage(const QString &url, const QString &alt) { insertImage(blocks_.size(), url, alt); }

void NoteBlockModel::insertImage(int row, const QString &url, const QString &alt)
{
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.type = Image;
    block.url  = url;
    block.alt  = alt;
    blocks_.insert(row, block);
    endInsertRows();
    notifyNormalizedTagLines();
    emit contentsChanged();
}

void NoteBlockModel::appendAudio(const QString &url, const QString &title, qint64 durationMs)
{
    insertAudio(blocks_.size(), url, title, durationMs);
}

void NoteBlockModel::insertAudio(int row, const QString &url, const QString &title, qint64 durationMs)
{
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.type            = Audio;
    block.url             = url;
    block.alt             = title;
    block.audioDurationMs = qBound<qint64>(0, durationMs, MaxAudioDurationMs);
    blocks_.insert(row, block);
    endInsertRows();
    notifyNormalizedTagLines();
    emit contentsChanged();
}

bool NoteBlockModel::setAudioTranscript(int row, const QString &transcript)
{
    return setData(index(row), transcript, AudioTranscriptRole);
}

bool NoteBlockModel::setAudioTitle(int row, const QString &title)
{
    if (blockTypeAt(row) != Audio)
        return false;
    return setData(index(row), title.trimmed(), AltRole);
}

void NoteBlockModel::appendAttachment(const QString &url, const QString &fileName, const QString &mediaType,
                                      qint64 size)
{
    insertAttachment(blocks_.size(), url, fileName, mediaType, size);
}

void NoteBlockModel::insertAttachment(int row, const QString &url, const QString &fileName, const QString &mediaType,
                                      qint64 size)
{
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.type                = Attachment;
    block.url                 = url;
    block.alt                 = fileName;
    block.attachmentMediaType = mediaType;
    block.attachmentSize      = qMax<qint64>(0, size);
    blocks_.insert(row, block);
    endInsertRows();
    notifyNormalizedTagLines();
    emit contentsChanged();
}

void NoteBlockModel::insertTable(int row)
{
    row = qBound(0, row, blocks_.size());
    beginInsertRows({}, row, row);
    Block block;
    block.type    = Table;
    block.columns = 2;
    block.cells   = { QString(), QString(), QString(), QString() };
    blocks_.insert(row, block);
    endInsertRows();
    notifyNormalizedTagLines();
    emit contentsChanged();
}

} // namespace AnyKeep
