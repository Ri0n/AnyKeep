#ifndef QTNOTE_EDITORPLATFORMBACKEND_H
#define QTNOTE_EDITORPLATFORMBACKEND_H

#include "highlighterext.h"
#include "mediareference.h"
#include "qtnote_export.h"

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantList>
#include <memory>

class QImage;
class QMimeData;
class QQuickTextDocument;

namespace QtNote {

class NoteEditor;
class NoteHighlighter;
struct NoteFragment;

// UI-neutral editor integration shared by desktop and mobile shells.  It owns
// document decoration, spell checking, and media insertion; platform adapters
// only provide pickers, drag-and-drop, printing, and other OS-specific actions.
class QTNOTE_EXPORT EditorPlatformBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool spellCheckEnabled READ spellCheckEnabled WRITE setSpellCheckEnabled NOTIFY spellCheckEnabledChanged)
    Q_PROPERTY(bool canInsertImages READ canInsertImages NOTIFY canInsertImagesChanged)

public:
    explicit EditorPlatformBackend(QObject *parent = nullptr);
    EditorPlatformBackend(NoteEditor *editor, QObject *parent = nullptr);
    ~EditorPlatformBackend() override;

    NoteEditor *editor() const;
    void        setEditor(NoteEditor *editor);

    bool spellCheckEnabled() const { return spellCheckEnabled_; }
    bool canInsertImages() const;

    Q_INVOKABLE void         registerTextDocument(QQuickTextDocument *document, bool titleDocument);
    Q_INVOKABLE QVariantList spellCheckRanges(QQuickTextDocument *document);
    Q_INVOKABLE QStringList  spellingSuggestions(const QString &word) const;
    Q_INVOKABLE void         addToSpellingDictionary(const QString &word);
    Q_INVOKABLE bool         insertClipboardImage(int row = -1);
    Q_INVOKABLE bool         insertImageData(const QByteArray &data, const QString &name, const QString &mediaType,
                                             int row = -1);
    Q_INVOKABLE virtual bool insertImage(int row = -1);
    Q_INVOKABLE virtual void saveImageAs(const QString &url);
    Q_INVOKABLE virtual bool startImageDrag(int row);

    bool insertRasterImage(const QImage &image, const QString &name, int row = -1);
    bool insertImageFiles(const QStringList &fileNames, int row = -1, QString *error = nullptr);
    bool canAcceptImageMimeData(const QMimeData *mimeData) const;
    bool insertImageMimeData(const QMimeData *mimeData, int row = -1);
    bool canInsertImageFragment(const NoteFragment &fragment) const;
    bool insertImageFragment(const NoteFragment &fragment, int row = -1);

    void setSpellCheckEnabled(bool enabled);
    void addHighlightExtension(const std::shared_ptr<HighlighterExtension> &extension, int type);
    void rehighlight();
    void reloadVisualSettings();

signals:
    void spellCheckEnabledChanged();
    void canInsertImagesChanged();
    void highlightingChanged();
    void mediaInserted(const QList<MediaReference> &references);
    void imageInsertionRequested(int row);
    void operationFailed(const QString &message);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

    QPointer<NoteEditor> editor_;

private:
    struct HighlightExtension {
        std::shared_ptr<HighlighterExtension> extension;
        int                                   type;
    };
    struct RegisteredHighlighter {
        QPointer<NoteHighlighter> highlighter;
        bool                      titleDocument;
    };

    bool insertImportedImages(const QList<MediaReference> &references, int row, const QString &historyKind);
    void clearRegisteredDocuments();
    void installBuiltInExtensions();

    QList<HighlightExtension>             extensions_;
    QList<RegisteredHighlighter>          highlighters_;
    std::shared_ptr<HighlighterExtension> titleExtension_;
    bool                                  spellCheckEnabled_ { true };
};

} // namespace QtNote

#endif // QTNOTE_EDITORPLATFORMBACKEND_H
