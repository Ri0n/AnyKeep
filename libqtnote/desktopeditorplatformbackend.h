#ifndef DESKTOPEDITORPLATFORMBACKEND_H
#define DESKTOPEDITORPLATFORMBACKEND_H

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
class QTemporaryDir;
class QWidget;

namespace QtNote {

class NoteEditor;
class NoteHighlighter;
struct NoteFragment;

class QTNOTE_EXPORT DesktopEditorPlatformBackend final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool spellCheckEnabled READ spellCheckEnabled WRITE setSpellCheckEnabled NOTIFY spellCheckEnabledChanged)
    Q_PROPERTY(bool canInsertImages READ canInsertImages NOTIFY canInsertImagesChanged)

public:
    explicit DesktopEditorPlatformBackend(QObject *parent = nullptr);
    DesktopEditorPlatformBackend(NoteEditor *editor, QObject *parent = nullptr);
    ~DesktopEditorPlatformBackend() override;

    NoteEditor *editor() const;
    void        setEditor(NoteEditor *editor);
    void        setDialogParent(QWidget *parent);
    void        setDragSource(QObject *source) { dragSource_ = source; }

    bool spellCheckEnabled() const { return spellCheckEnabled_; }
    bool canInsertImages() const;

    Q_INVOKABLE void         registerTextDocument(QQuickTextDocument *document, bool titleDocument);
    Q_INVOKABLE QVariantList spellCheckRanges(QQuickTextDocument *document);
    Q_INVOKABLE QStringList  spellingSuggestions(const QString &word) const;
    Q_INVOKABLE void         addToSpellingDictionary(const QString &word);
    Q_INVOKABLE void         saveImageAs(const QString &url);
    Q_INVOKABLE bool         startImageDrag(int row);
    Q_INVOKABLE bool         insertImage(int row = -1);
    Q_INVOKABLE bool         insertClipboardImage(int row = -1);
    bool                     insertRasterImage(const QImage &image, const QString &name, int row = -1);
    bool                     insertImageFiles(const QStringList &fileNames, int row = -1);
    bool                     canAcceptImageMimeData(const QMimeData *mimeData) const;
    bool                     insertImageMimeData(const QMimeData *mimeData, int row = -1);
    bool                     canInsertImageFragment(const NoteFragment &fragment) const;
    bool                     insertImageFragment(const NoteFragment &fragment, int row = -1);

    void setSpellCheckEnabled(bool enabled);
    void addHighlightExtension(const std::shared_ptr<HighlighterExtension> &extension, int type);
    void rehighlight();

signals:
    void spellCheckEnabledChanged();
    void canInsertImagesChanged();
    void mediaInserted(const QList<MediaReference> &references);

private:
    struct HighlightExtension {
        std::shared_ptr<HighlighterExtension> extension;
        int                                   type;
    };
    struct RegisteredHighlighter {
        QPointer<NoteHighlighter> highlighter;
        bool                      titleDocument;
    };

    bool    insertImportedImages(const QList<MediaReference> &references, int row, const QString &historyKind);
    QString materializeDragImage(const MediaReference &reference, const QByteArray &data);
    void    clearRegisteredDocuments();

    QPointer<NoteEditor>           editor_;
    QPointer<QWidget>              dialogParent_;
    QPointer<QObject>              dragSource_;
    QList<HighlightExtension>      extensions_;
    QList<RegisteredHighlighter>   highlighters_;
    bool                           spellCheckEnabled_ { true };
    std::unique_ptr<QTemporaryDir> dragExportDirectory_;
};

} // namespace QtNote

#endif // DESKTOPEDITORPLATFORMBACKEND_H
