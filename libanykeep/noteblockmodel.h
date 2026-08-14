#ifndef NOTEBLOCKMODEL_H
#define NOTEBLOCKMODEL_H

#include <QAbstractListModel>
#include <QVariantMap>

#include "note.h"
#include "notefragment.h"

namespace AnyKeep {

class NoteDocumentHistory;
class NoteEditor;

struct ANYKEEP_EXPORT NoteBlockSelectionRange {
    int     blockIndex { -1 };
    int     listItemIndex { -1 };
    int     tableCellIndex { -1 };
    QString markdown;
    bool    wholeEditor { false };
    QString before;
    QString after;
};

class ANYKEEP_EXPORT NoteBlockModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool markdown READ markdown NOTIFY markdownChanged)
    Q_PROPERTY(QString contents READ contents WRITE setContents NOTIFY contentsChanged)

public:
    enum BlockType {
        Text,
        BulletList,
        CheckList,
        Table,
        Image,
        NumberedList,
        Heading,
        BlockQuote,
        CodeBlock,
        TagLine,
        Audio,
        Attachment
    };
    Q_ENUM(BlockType)
    enum Role {
        TypeRole = Qt::UserRole + 1,
        TextRole,
        ItemsRole,
        CheckedRole,
        CellsRole,
        UrlRole,
        AltRole,
        PreviewUrlRole,
        IndentsRole,
        ItemTypesRole,
        HeadingLevelRole,
        LanguageRole,
        ImageWidthRole,
        ImageAlignmentRole,
        TagsRole,
        AudioDurationRole,
        AudioTranscriptRole,
        AttachmentMediaTypeRole,
        AttachmentSizeRole
    };

    explicit NoteBlockModel(QObject *parent = nullptr);

    int                    rowCount(const QModelIndex &parent = {}) const override;
    QVariant               data(const QModelIndex &index, int role) const override;
    bool                   setData(const QModelIndex &index, const QVariant &value, int role) override;
    Qt::ItemFlags          flags(const QModelIndex &index) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool    markdown() const { return markdown_; }
    QString contents() const;
    void    setContents(const QString &contents);

    Q_INVOKABLE void load(const QString &contents, bool markdown);
    Q_INVOKABLE void setBlockText(int row, const QString &text);
    Q_INVOKABLE bool mergeTextBlockWithNext(int row);
    Q_INVOKABLE void setListItem(int row, int item, const QString &text);
    Q_INVOKABLE void insertListItem(int row, int item, const QString &text);
    Q_INVOKABLE void mergeListItemWithNext(int row, int item);
    Q_INVOKABLE bool mergeListItemWithFollowingBlock(int row, int item);
    Q_INVOKABLE void removeListItem(int row, int item);
    Q_INVOKABLE void removeListItems(int row, int firstItem, int lastItem);
    Q_INVOKABLE bool moveListRange(int sourceRow, int sourceFirstItem, int sourceLastItem, int targetRow,
                                   int targetItem, int targetIndent);
    Q_INVOKABLE int  moveListRangeResolved(int sourceRow, int sourceFirstItem, int sourceLastItem, int targetRow,
                                           int targetItem, int targetIndent);
    Q_INVOKABLE int  moveListRangeToBlock(int sourceRow, int sourceFirstItem, int sourceLastItem, int targetRow);
    Q_INVOKABLE bool moveListSubtree(int sourceRow, int sourceItem, int targetRow, int targetItem, int targetIndent);
    Q_INVOKABLE void convertListToText(int row);
    Q_INVOKABLE int  unlistListItem(int row, int item);
    Q_INVOKABLE void indentListItems(int row, int firstItem, int lastItem, int delta);
    Q_INVOKABLE void setChecked(int row, int item, bool checked);
    Q_INVOKABLE void setTableCell(int row, int cell, const QString &text);
    Q_INVOKABLE void insertTableRow(int row, int tableRow);
    Q_INVOKABLE void removeTableRow(int row, int tableRow);
    Q_INVOKABLE void removeTableRows(int row, int firstRow, int lastRow);
    Q_INVOKABLE void insertTableColumn(int row, int column);
    Q_INVOKABLE void removeTableColumn(int row, int column);
    Q_INVOKABLE bool moveTableColumn(int row, int from, int to);
    Q_INVOKABLE void setImageUrl(int row, const QString &url);
    Q_INVOKABLE void setImageAlt(int row, const QString &alt);
    Q_INVOKABLE void setImageWidth(int row, int width);
    Q_INVOKABLE void setImageAlignment(int row, const QString &alignment);
    Q_INVOKABLE void insertTextBlock(int row);
    Q_INVOKABLE void appendTextBlock();
    Q_INVOKABLE void appendText(const QString &text);
    Q_INVOKABLE void appendImage(const QString &url, const QString &alt);
    Q_INVOKABLE void insertImage(int row, const QString &url, const QString &alt);
    Q_INVOKABLE void appendAudio(const QString &url, const QString &title, qint64 durationMs);
    Q_INVOKABLE void insertAudio(int row, const QString &url, const QString &title, qint64 durationMs);
    Q_INVOKABLE bool setAudioTitle(int row, const QString &title);
    Q_INVOKABLE bool setAudioTranscript(int row, const QString &transcript);
    Q_INVOKABLE void appendAttachment(const QString &url, const QString &fileName, const QString &mediaType,
                                      qint64 size);
    Q_INVOKABLE void insertAttachment(int row, const QString &url, const QString &fileName, const QString &mediaType,
                                      qint64 size);
    Q_INVOKABLE void insertTable(int row);
    Q_INVOKABLE void insertList(int row, BlockType type);
    Q_INVOKABLE void insertBlockQuote(int row);
    Q_INVOKABLE void insertCodeBlock(int row, const QString &language = QString());
    Q_INVOKABLE void setCodeLanguage(int row, const QString &language);
    Q_INVOKABLE QVariantMap promoteTagLineFromText(int row, const QString &plainText, const QString &markdownText,
                                                   int cursorPosition, bool force);
    Q_INVOKABLE QVariantMap setTagLineTag(int row, int tagIndex, const QString &value, int cursorPosition = -1);
    Q_INVOKABLE QVariantMap appendTagLineTag(int row, const QString &value, int cursorPosition = -1);
    Q_INVOKABLE QVariantMap removeTagLineTag(int row, int tagIndex);
    Q_INVOKABLE bool        moveTagLineTag(int row, int from, int to);
    Q_INVOKABLE QVariantMap convertTagLineToText(int row, const QString &text = QString(), int cursorPosition = -1);
    Q_INVOKABLE int         blockTypeAt(int row) const;
    Q_INVOKABLE int         listItemCountAt(int row) const;
    Q_INVOKABLE bool        isExplicitEmptyTextBlock(int row) const;
    Q_INVOKABLE QVariantMap findText(const QString &text, const QVariantMap &after = {}, bool backwards = false,
                                     bool caseSensitive = false) const;
    Q_INVOKABLE bool        convertListLevel(int row, int item, BlockType type);
    Q_INVOKABLE int         convertTextBlockToHeading(int row, int position, int level);
    Q_INVOKABLE int         convertTextBlockToQuote(int row, int position, bool quote);
    Q_INVOKABLE QVariantMap convertTextRangeToQuote(int row, int start, int end);
    Q_INVOKABLE int         sourcePositionAfterTextCoalesce(int row, int position) const;
    Q_INVOKABLE int         sourcePositionInParagraph(int row, int position) const;
    Q_INVOKABLE bool        splitTitleBlock(const QString &before, const QString &after);
    Q_INVOKABLE bool        splitStructuredBlockToText(int row, const QString &before, const QString &after);
    Q_INVOKABLE bool        moveBlock(int row, int targetRow);
    Q_INVOKABLE int         moveBlockResolved(int row, int targetRow);
    Q_INVOKABLE void        removeBlock(int row);
    void                    setPreviewUrls(const QHash<QString, QString> &urls);

    // C++ transfer boundary. QML reports visual editor ranges, while the model
    // restores their list/table/block semantics.
    NoteFragment extractBlockFragment(int firstRow, int lastRow) const;
    NoteFragment extractSelectionFragment(const QList<NoteBlockSelectionRange> &ranges) const;
    int          removeSelectionRanges(const QList<NoteBlockSelectionRange> &ranges);
    int  replaceTextSelectionWithCodeBlock(const QList<NoteBlockSelectionRange> &ranges, const QString &plainText,
                                           const QString &language = QString());
    bool insertBlockFragment(int row, const NoteFragment &fragment, QString *error = nullptr);
    int  replaceTextBlockRangeWithFragment(int row, const QString &before, const QString &after,
                                           const NoteFragment &fragment, QString *error = nullptr);
    bool replaceTableCellsWithFragment(int row, int firstCell, const NoteFragment &fragment, QString *error = nullptr);
    int  replaceListItemRangeWithFragment(int row, int item, const QString &before, const QString &after,
                                          const NoteFragment &fragment, QString *error = nullptr);

signals:
    void markdownChanged();
    void scalarEdited(int row, int role, int fieldIndex, const QString &before, const QString &after);
    void contentsChanged();

private:
    friend class NoteDocumentHistory;
    friend class NoteEditor;
    // Internal document representation used by editor history. It preserves
    // topology without turning the storage format into public libanykeep API.
    struct Block {
        BlockType    type = Text;
        QString      text;
        QStringList  items;
        QVariantList indents;
        QVariantList itemTypes;
        QVariantList checked;
        QStringList  cells;
        int          columns = 0;
        QString      url;
        QString      alt;
        int          imageWidth     = 0;
        QString      imageAlignment = QStringLiteral("center");
        int          headingLevel   = 0;
        QString      language;
        QStringList  tags;
        qint64       audioDurationMs = 0;
        QString      audioTranscript;
        QString      attachmentMediaType;
        qint64       attachmentSize = 0;
        bool         explicitEmpty  = false;

        bool operator==(const Block &other) const
        {
            return type == other.type && text == other.text && items == other.items && indents == other.indents
                && itemTypes == other.itemTypes && checked == other.checked && cells == other.cells
                && columns == other.columns && url == other.url && alt == other.alt && imageWidth == other.imageWidth
                && imageAlignment == other.imageAlignment && headingLevel == other.headingLevel
                && language == other.language && tags == other.tags && audioTranscript == other.audioTranscript
                && attachmentMediaType == other.attachmentMediaType && attachmentSize == other.attachmentSize
                && audioDurationMs == other.audioDurationMs && explicitEmpty == other.explicitEmpty;
        }
    };

    struct State {
        QList<Block> blocks;
        bool         markdown = false;

        bool operator==(const State &other) const { return markdown == other.markdown && blocks == other.blocks; }
    };

    State               state() const;
    bool                restoreState(const State &state);
    static QList<Block> parseMarkdown(const QString &source);
    static QList<Block> parseMarkdownCore(const QString &source);
    static QList<Block> parseMarkdownWithoutCode(const QString &source);
    static QString      writeMarkdown(const QList<Block> &blocks);
    static bool         normalizeTitleBlock(QList<Block> *blocks, bool markdown);
    static void         normalizeListStorage(Block *block);
    static void         normalizeMovedListTypes(Block *block, int firstItem, int itemCount);
    static void         mergeListPair(QList<Block> *blocks, int leftRow, bool residentIsLeft, int *trackedRow);
    static void         coalesceListAtBoundary(QList<Block> *blocks, int boundary, int *trackedRow);
    static void         coalesceMovedList(QList<Block> *blocks, int *movedRow);
    static void         coalesceTextNear(QList<Block> *blocks, int row, bool markdown, int *trackedRow);
    int                 convertStructuredBlockToText(int row);
    static bool         blocksFromFragment(const NoteFragment &fragment, QList<Block> *blocks, QString *error);
    static QList<Block> cloneBlocks(const QList<Block> &blocks);
    static bool normalizeTagLinePositions(QList<Block> *blocks, bool markdown, bool promoteTextCandidate = false);
    void        notifyNormalizedTagLines(bool promoteTextCandidate = false);
    void        changed(int row, const QList<int> &roles);

    QList<Block>            blocks_;
    QHash<QString, QString> previewUrls_;
    bool                    markdown_ = false;
};

} // namespace AnyKeep

#endif
