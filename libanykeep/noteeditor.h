#ifndef NOTEEDITOR_H
#define NOTEEDITOR_H

#include "anykeep_export.h"
#include "draftstore.h"
#include "note.h"
#include "notefragment.h"

#include <QObject>
#include <QPointer>
#include <QUuid>
#include <QVariantMap>

#include <memory>

class QQuickTextDocument;

namespace AnyKeep {

class AudioPlaybackController;
class DraftManager;
class NoteBlockModel;
class NoteDocumentHistory;

class ANYKEEP_EXPORT NoteEditor final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(bool markdown READ isMarkdown WRITE setMarkdown NOTIFY formatChanged)
    Q_PROPERTY(bool dirty READ isDirty NOTIFY dirtyChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(QString draftId READ draftIdString CONSTANT)
    Q_PROPERTY(QString storageId READ storageId CONSTANT)
    Q_PROPERTY(QString noteId READ noteId CONSTANT)
    Q_PROPERTY(QString folderId READ folderIdString NOTIFY folderIdChanged)
    Q_PROPERTY(bool supportsMedia READ supportsMedia CONSTANT)
    Q_PROPERTY(bool canInsertImages READ canInsertImages NOTIFY formatChanged)
    Q_PROPERTY(bool canInsertAudio READ canInsertAudio NOTIFY formatChanged)
    Q_PROPERTY(bool canInsertAttachments READ canInsertAttachments NOTIFY formatChanged)
    Q_PROPERTY(QObject *blockModel READ blockModel CONSTANT)
    Q_PROPERTY(QObject *audioPlayback READ audioPlayback CONSTANT)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStateChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoStateChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY undoStateChanged)

public:
    enum class LoadPolicy {
        ResetHistory,
        RecordFormatConversion,
        HistoryRestore,
        ReplaceContent,
    };
    Q_ENUM(LoadPolicy)

    explicit NoteEditor(const Note &note = {}, const QUuid &draftId = {}, QObject *parent = nullptr);
    NoteEditor(const Note &note, DraftManager &drafts, const QUuid &draftId = {}, QObject *parent = nullptr);
    ~NoteEditor() override;

    Note    note() const { return note_; }
    QUuid   draftId() const { return draftId_; }
    QString draftIdString() const { return draftId_.toString(QUuid::WithoutBraces); }
    QString storageId() const { return note_.storageId(); }
    QString noteId() const { return note_.id(); }
    QString folderIdString() const
    {
        const auto id = folderId();
        return id.isNull() ? QString() : id.toString(QUuid::WithoutBraces);
    }
    bool                  supportsMedia() const;
    bool                  canInsertImages() const;
    bool                  canInsertAudio() const;
    bool                  canInsertAttachments() const;
    QString               text() const { return text_; }
    bool                  isMarkdown() const { return format_ != Note::PlainText; }
    Note::Format          format() const { return format_; }
    bool                  isDirty() const { return dirty_; }
    QUuid                 folderId() const { return note_.folderId(); }
    bool                  hasPersistedDraft() const { return draftPersisted_; }
    bool                  folderUserOverride() const { return folderUserOverride_; }
    QString               errorString() const { return errorString_; }
    QObject              *blockModel() const;
    QObject              *audioPlayback() const;
    NoteBlockModel       *model() const { return model_; }
    QList<MediaReference> media() const { return media_; }
    bool                  canUndo() const;
    bool                  canRedo() const;
    QString               undoText() const;
    QString               redoText() const;
    bool                  historyInTransaction() const;

    void             setMedia(const QList<MediaReference> &media);
    bool             insertAudio(const MediaReference &reference, qint64 durationMs, int row = -1);
    bool             insertAudio(const MediaReference &reference, qint64 durationMs, int row, const QString &title);
    bool             insertAttachment(const MediaReference &reference, int row = -1);
    Q_INVOKABLE bool setAudioTranscript(int row, const QString &transcript);
    void             setFolderId(const QUuid &folderId);
    /** Preserves a direct user folder choice against automatic routing rules. */
    void setFolderUserOverride(bool enabled = true) { folderUserOverride_ = enabled; }
    /** Marks the current folder metadata as durably stored without touching document content. */
    void markFolderPersisted(const QUuid &folderId);
    void loadDocument(const QString &contents, Note::Format format, LoadPolicy policy = LoadPolicy::ResetHistory);
    void resetContent(const QString &text, Note::Format format);
    void resetHistory();
    bool reloadNewerDraft();

    Q_INVOKABLE void        registerEditorView(QObject *view);
    Q_INVOKABLE void        beginHistoryTransaction(const QString &kind, const QVariantMap &beforeView = {});
    Q_INVOKABLE void        endHistoryTransaction(const QVariantMap &afterView = {});
    Q_INVOKABLE void        updateHistoryViewState(const QVariantMap &viewState, bool breakMerge = false);
    Q_INVOKABLE bool        undo();
    Q_INVOKABLE bool        redo();
    Q_INVOKABLE void        copyToClipboard(const QString &text);
    Q_INVOKABLE void        copyMarkdownToClipboard(const QString &markdown);
    Q_INVOKABLE void        copyMarkdownAsPlainTextToClipboard(const QString &markdown);
    Q_INVOKABLE void        copyDocumentToClipboard();
    Q_INVOKABLE bool        copySelectionToClipboard(const QVariantList &ranges);
    Q_INVOKABLE bool        copyBlockToClipboard(int row);
    Q_INVOKABLE bool        copySelectionAsMarkdownToClipboard(const QVariantList &ranges);
    Q_INVOKABLE bool        copyTextToPrimarySelection(const QString &text);
    Q_INVOKABLE bool        copyMarkdownToPrimarySelection(const QString &markdown);
    Q_INVOKABLE bool        copySelectionToPrimarySelection(const QVariantList &ranges);
    Q_INVOKABLE QVariantMap deleteSelection(const QVariantList &ranges);
    Q_INVOKABLE QVariantMap convertSelectionToCodeBlock(const QVariantList &ranges, const QString &plainText,
                                                        const QString &language = QString());
    Q_INVOKABLE int insertDroppedCodeBlock(int row, const QString &before, const QString &after, const QString &text,
                                           const QString &language);
    Q_INVOKABLE int insertPlainText(QQuickTextDocument *document, int start, int end, const QString &text);
    Q_INVOKABLE int pastePlainText(QQuickTextDocument *document, int start, int end);
    Q_INVOKABLE int pastePrimarySelection(QQuickTextDocument *document, int start, int end);
    Q_INVOKABLE QVariantMap pasteStructuredFromClipboard(QQuickTextDocument *document, int row, int start, int end);
    Q_INVOKABLE QVariantMap pasteTableFromClipboard(int row, int cell);
    Q_INVOKABLE QVariantMap pasteListFromClipboard(QQuickTextDocument *document, int row, int item, int start, int end);
    Q_INVOKABLE QVariantMap linkInfo(QQuickTextDocument *document, int start, int end) const;
    Q_INVOKABLE int         setLink(QQuickTextDocument *document, int start, int end, const QString &href);
    Q_INVOKABLE bool        primaryModifierPressed() const;
    Q_INVOKABLE int         applyInlineFormat(QQuickTextDocument *document, int start, int end, const QString &style);
    Q_INVOKABLE bool        inlineFormatEnabled(QQuickTextDocument *document, int position, const QString &style) const;
    Q_INVOKABLE bool        setInlineFormat(QQuickTextDocument *document, int start, int end, const QString &style,
                                            bool enabled);
    Q_INVOKABLE void        applyInlineHtmlFormatting(QQuickTextDocument *document) const;
    Q_INVOKABLE QString     markdownText(QQuickTextDocument *document) const;
    Q_INVOKABLE QString     markdownTableCellText(QQuickTextDocument *document) const;
    Q_INVOKABLE QString     markdownSelection(QQuickTextDocument *document, int start, int end) const;
    void                    breakHistoryMerge();

public slots:
    void setText(const QString &text);
    void setMarkdown(bool markdown);
    bool save();
    bool close();
    bool discardDraft();
    bool discardAndClose();

signals:
    void textChanged();
    void formatChanged();
    void dirtyChanged();
    void errorStringChanged();
    void folderIdChanged();
    void mediaChanged(const QList<MediaReference> &media);
    void mediaInserted(const QList<MediaReference> &media);
    void documentLoaded(bool formatChanged, bool formatConversion);
    void undoStateChanged();
    void historyDocumentRestored(bool formatChanged);

private:
    void                     loadFromNote();
    void                     adoptEditingDraft(const DraftRecord &draft);
    QVariantMap              captureEditorViewState() const;
    void                     prepareEditorViewForHistoryRestore();
    void                     scheduleEditorViewRestore(const QVariantMap &viewState);
    void                     restoreScalarField(int blockIndex, int role, int fieldIndex, const QString &value);
    void                     updateMediaPreviewUrls();
    NoteFragment             documentFragment() const;
    NoteFragment             withMedia(NoteFragment fragment) const;
    void                     setDirty(bool dirty);
    void                     setMetadataDirty(bool dirty);
    void                     updateDirty();
    bool                     setError(const QString &error);
    Note                     note_;
    DraftManager            *drafts_ { nullptr };
    NoteBlockModel          *model_ { nullptr };
    AudioPlaybackController *audioPlayback_ { nullptr };
    QUuid                    draftId_;
    QString                  text_;
    QString                  baselineText_;
    Note::Format             format_ { Note::PlainText };
    Note::Format             baselineFormat_ { Note::PlainText };
    QUuid                    baselineFolderId_;
    bool                     contentDirty_ { false };
    bool                     metadataDirty_ { false };
    bool                     dirty_ { false };
    bool                     draftPersisted_ { false };
    bool                     folderUserOverride_ { false };
    bool                     sessionReleased_ { false };
    int                      draftRevision_ { 0 };
    QString                  errorString_;
    QList<MediaReference>    media_;
    QPointer<QObject>        editorView_;
    std::unique_ptr<NoteDocumentHistory> history_;
    bool                                 scalarHistoryChangePending_ { false };
};

} // namespace AnyKeep

#endif // NOTEEDITOR_H
