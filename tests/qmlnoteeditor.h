#ifndef QTNOTE_TESTS_QMLNOTEEDITOR_FIXTURE_H
#define QTNOTE_TESTS_QMLNOTEEDITOR_FIXTURE_H

// Test-only compatibility fixture. Production code uses NoteEditor plus
// DesktopNoteEditorHost/DesktopEditorPlatformBackend.

#include <QPointer>
#include <QStringList>
#include <QVariantMap>
#include <QWidget>
#include <memory>

#include "mediareference.h"
#include "note.h"
#include "noteeditor.h"
#include "notefragment.h"

class QQuickWidget;
class QQuickTextDocument;
class QImage;
class QMimeData;
class QPointF;
class QShowEvent;
class QTemporaryDir;

namespace QtNote {
class NoteBlockModel;
class NoteEditor;
class NoteHighlighter;
class HighlighterExtension;

class QmlNoteEditor : public QWidget {
    Q_OBJECT
    Q_PROPERTY(bool spellCheckEnabled READ spellCheckEnabled WRITE setSpellCheckEnabled NOTIFY spellCheckEnabledChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoStateChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoStateChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY undoStateChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY undoStateChanged)
    Q_PROPERTY(bool markdown READ isMarkdown NOTIFY formatChanged)

public:
    explicit QmlNoteEditor(QWidget *parent = nullptr);
    QmlNoteEditor(NoteEditor *editor, QWidget *parent);
    ~QmlNoteEditor() override;

    NoteBlockModel          *model() const { return model_; }
    QList<MediaReference>    media() const;
    void                     load(const QString &contents, Note::Format format,
                                  NoteEditor::LoadPolicy policy = NoteEditor::LoadPolicy::ResetHistory);
    void                     setMedia(const QList<MediaReference> &media);
    void                     setImageInsertionEnabled(bool enabled) { imageInsertionEnabled_ = enabled; }
    QString                  contents() const;
    bool                     isMarkdown() const;
    bool                     canUndo() const;
    bool                     canRedo() const;
    QString                  undoText() const;
    QString                  redoText() const;
    void                     insertText(const QString &text);
    void                     focusEditor();
    void                     insertTable();
    void                     insertList(int type);
    void                     beginExternalHistoryTransaction(const QString &kind);
    void                     endExternalHistoryTransaction();
    void                     breakHistoryMerge();
    Q_INVOKABLE void         registerEditorView(QObject *view);
    Q_INVOKABLE void         registerTextDocument(QQuickTextDocument *document, bool titleDocument);
    Q_INVOKABLE QVariantList spellCheckRanges(QQuickTextDocument *document);
    Q_INVOKABLE QStringList  spellingSuggestions(const QString &word) const;
    Q_INVOKABLE void         addToSpellingDictionary(const QString &word);
    Q_INVOKABLE void         saveImageAs(const QString &url);
    Q_INVOKABLE bool         startImageDrag(int row);
    Q_INVOKABLE void         beginHistoryTransaction(const QString &kind, const QVariantMap &beforeView);
    Q_INVOKABLE void         endHistoryTransaction(const QVariantMap &afterView);
    Q_INVOKABLE void         updateHistoryViewState(const QVariantMap &viewState, bool breakMerge = false);
    Q_INVOKABLE bool         undo();
    Q_INVOKABLE bool         redo();
    bool                     spellCheckEnabled() const { return spellCheckEnabled_; }
    void                     setSpellCheckEnabled(bool enabled);
    void                     addHighlightExtension(const std::shared_ptr<HighlighterExtension> &extension, int type);
    void                     rehighlight();

signals:
    void contentsChanged();
    void focusReceived();
    void focusLost();
    void imagePasteRequested(const QImage &image);
    void imageDropRequested(const QImage &image, int row);
    void imageFilesDropRequested(const QStringList &fileNames, int row);
    void mediaInserted(const QList<MediaReference> &references);
    void mediaChanged(const QList<MediaReference> &references);
    void spellCheckEnabledChanged();
    void undoStateChanged();
    void formatChanged(bool markdown);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void            updateFocusWindow();
    void            flushPendingEditorChanges();
    NoteFragment    documentFragment() const;
    NoteFragment    withMedia(NoteFragment fragment) const;
    bool            canAcceptImageDrop(const QMimeData *mimeData) const;
    bool            handleImageDrop(const QMimeData *mimeData, int row);
    int             insertionRowAt(const QPointF &position) const;
    QString         materializeDragImage(const MediaReference &reference, const QByteArray &data);
    NoteEditor     *editor_ = nullptr;
    NoteBlockModel *model_  = nullptr;
    QQuickWidget   *quick_  = nullptr;
    struct HighlightExtension {
        std::shared_ptr<HighlighterExtension> extension;
        int                                   type;
    };
    struct RegisteredHighlighter {
        QPointer<NoteHighlighter> highlighter;
        bool                      titleDocument;
    };
    QList<HighlightExtension>      extensions_;
    QList<RegisteredHighlighter>   highlighters_;
    QPointer<QWidget>              focusWindow_;
    quint64                        loadGeneration_           = 0;
    bool                           suppressNextFocusRefresh_ = false;
    bool                           spellCheckEnabled_        = true;
    bool                           imageInsertionEnabled_    = false;
    bool                           imageDragAccepted_        = false;
    std::unique_ptr<QTemporaryDir> dragExportDirectory_;
};
} // namespace QtNote

#endif // QTNOTE_TESTS_QMLNOTEEDITOR_FIXTURE_H
