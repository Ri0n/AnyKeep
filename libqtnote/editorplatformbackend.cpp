#include "editorplatformbackend.h"

#include "defaults.h"
#include "localmediastore.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "notefragmentmediatransfer.h"
#include "notehighlighter.h"
#include "notetransfercontroller.h"
#include "utils.h"

#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QMimeDatabase>
#include <QPalette>
#include <QQuickTextDocument>
#include <QSettings>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace QtNote {
namespace {

    int documentEnd(const QTextDocument *document) { return document ? qMax(0, document->characterCount() - 1) : 0; }

    QTextCharFormat formatAt(QTextDocument *document, int position)
    {
        const int limit = documentEnd(document);
        if (!document || position < 0 || position >= limit)
            return {};
        const QTextBlock block = document->findBlock(position);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.contains(position))
                return fragment.charFormat();
        }
        return {};
    }

    bool isUsableImageFragment(const NoteFragment &fragment)
    {
        if (fragment.blocks.isEmpty())
            return false;
        for (const auto &block : fragment.blocks) {
            if (block.type != NoteFragmentBlockType::Image || block.image.sourceUri.isEmpty())
                return false;
            const QUrl source(block.image.sourceUri);
            if (source.scheme().compare(QStringLiteral("qtnote-media"), Qt::CaseInsensitive) != 0)
                continue;
            bool hasMedia = false;
            for (const auto &media : fragment.media) {
                if (media.sourceUri == block.image.sourceUri && media.reference.isValid()) {
                    hasMedia = true;
                    break;
                }
            }
            if (!hasMedia)
                return false;
        }
        return true;
    }

    QStringList localImageFiles(const QMimeData *mimeData)
    {
        QStringList result;
        if (!mimeData || !mimeData->hasUrls())
            return result;
        QMimeDatabase database;
        for (const auto &url : mimeData->urls()) {
            const QString fileName = url.toLocalFile();
            if (fileName.isEmpty() || !QFileInfo(fileName).isFile())
                continue;
            if (database.mimeTypeForFile(fileName, QMimeDatabase::MatchContent)
                    .name()
                    .startsWith(QLatin1String("image/"))) {
                result.append(fileName);
            }
        }
        return result;
    }

    class FirstLineHighlighter final : public HighlighterExtension {
    public:
        void setColor(const QColor &color) { color_ = color; }
        void reset() override { }

        void highlight(NoteHighlighter *highlighter, const QString &) override
        {
            const QTextBlock block = highlighter->currentBlock();
            if (block.position() != 0)
                return;

            QTextCharFormat format;
            format.setForeground(color_);
            format.setFontWeight(QFont::Normal);
            format.setFontItalic(false);
            format.setFontUnderline(false);
            format.setFontStrikeOut(false);
            format.setFontFixedPitch(false);
            format.setAnchor(false);
            format.setAnchorHref(QString());
            qreal pointSize = block.charFormat().font().pointSizeF();
            if (pointSize <= 0)
                pointSize = block.document()->defaultFont().pointSizeF();
            if (pointSize > 0)
                format.setFontPointSize(pointSize * 1.5);
            highlighter->addFormat(0, block.length(), format);
        }

    private:
        QColor color_;
    };

} // namespace

EditorPlatformBackend::EditorPlatformBackend(QObject *parent) : QObject(parent)
{
    setCustomSpellingDictionary(QSettings().value(QStringLiteral("editor/customSpellingDictionary")).toStringList());
    installBuiltInExtensions();
    if (qGuiApp)
        qGuiApp->installEventFilter(this);
}

EditorPlatformBackend::EditorPlatformBackend(NoteEditor *editor, QObject *parent) : EditorPlatformBackend(parent)
{
    setEditor(editor);
}

EditorPlatformBackend::~EditorPlatformBackend() = default;

bool EditorPlatformBackend::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qGuiApp && event->type() == QEvent::ApplicationPaletteChange)
        reloadVisualSettings();
    return QObject::eventFilter(watched, event);
}

NoteEditor *EditorPlatformBackend::editor() const { return editor_.data(); }

void EditorPlatformBackend::setEditor(NoteEditor *editor)
{
    if (editor_ == editor)
        return;
    if (editor_)
        disconnect(editor_, nullptr, this, nullptr);
    editor_ = editor;
    // The manager and mobile views reuse their QML TextArea documents while
    // switching NoteEditor instances. Clearing here detaches the highlighters
    // after the documents have already registered and they are not guaranteed
    // to register again until the format is toggled. Stale documents remove
    // their QSyntaxHighlighter automatically when they are destroyed.
    if (editor_) {
        connect(editor_, &NoteEditor::formatChanged, this, &EditorPlatformBackend::canInsertImagesChanged);
        connect(editor_, &QObject::destroyed, this, [this] {
            editor_.clear();
            emit canInsertImagesChanged();
        });
    }
    emit canInsertImagesChanged();
    // NotesManager switches its reusable QML documents after this C++ signal
    // handler returns. Rehighlight on the next event-loop turn, once the new
    // text and title-document bindings have settled.
    QTimer::singleShot(0, this, &EditorPlatformBackend::rehighlight);
}

bool EditorPlatformBackend::canInsertImages() const { return editor_ && editor_->canInsertImages(); }

void EditorPlatformBackend::registerTextDocument(QQuickTextDocument *document, bool titleDocument)
{
    if (!document || !document->textDocument())
        return;
    auto *textDocument = document->textDocument();
    for (auto &registered : highlighters_) {
        if (!registered.highlighter || registered.highlighter->document() != textDocument)
            continue;
        if (registered.titleDocument != titleDocument) {
            registered.titleDocument = titleDocument;
            if (titleDocument)
                registered.highlighter->enableExtension(NoteHighlighter::Title);
            else
                registered.highlighter->disableExtension(NoteHighlighter::Title);
        }
        registered.highlighter->rehighlight();
        QTimer::singleShot(0, this, &EditorPlatformBackend::rehighlight);
        emit highlightingChanged();
        return;
    }
    textDocument->setUndoRedoEnabled(false);
    auto *highlighter = new NoteHighlighter(textDocument);
    for (const auto &item : std::as_const(extensions_)) {
        const auto type = NoteHighlighter::ExtType(item.type);
        if (type == NoteHighlighter::Other)
            continue;
        highlighter->addExtension(item.extension, type);
        if ((type == NoteHighlighter::SpellCheck && !spellCheckEnabled_)
            || (type == NoteHighlighter::Title && !titleDocument)) {
            highlighter->disableExtension(type);
        }
    }
    highlighters_.append({ highlighter, titleDocument });
    highlighter->rehighlight();
    QTimer::singleShot(0, this, &EditorPlatformBackend::rehighlight);
}

QVariantList EditorPlatformBackend::spellCheckRanges(QQuickTextDocument *document)
{
    QVariantList result;
    if (!spellCheckEnabled_ || !document || !document->textDocument())
        return result;
    auto      *textDocument      = document->textDocument();
    const auto visibleTextIsHref = [textDocument](int position) {
        const int limit = documentEnd(textDocument);
        if (position < 0 || position >= limit)
            return false;
        const QTextCharFormat format = formatAt(textDocument, position);
        const QString         href   = format.anchorHref().trimmed();
        if (!format.isAnchor() || href.isEmpty())
            return false;
        int start = position;
        while (start > 0) {
            const auto previous = formatAt(textDocument, start - 1);
            if (!previous.isAnchor() || previous.anchorHref() != href)
                break;
            --start;
        }
        int end = position + 1;
        while (end < limit) {
            const auto next = formatAt(textDocument, end);
            if (!next.isAnchor() || next.anchorHref() != href)
                break;
            ++end;
        }
        QTextCursor cursor(textDocument);
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        QString visible = cursor.selectedText().trimmed();
        visible.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        return visible == href || (href.startsWith(QStringLiteral("mailto:")) && visible == href.mid(7))
            || (visible.startsWith(QStringLiteral("www.")) && href == QStringLiteral("https://") + visible);
    };

    for (auto block = textDocument->begin(); block.isValid(); block = block.next()) {
        if (!block.layout())
            continue;
        for (const auto &range : block.layout()->formats()) {
            if (!range.format.property(SpellCheckFormatProperty).toBool())
                continue;
            const int start = block.position() + range.start;
            if (visibleTextIsHref(start))
                continue;
            QTextCursor wordCursor(textDocument);
            wordCursor.setPosition(start);
            wordCursor.setPosition(start + range.length, QTextCursor::KeepAnchor);
            if (isCustomSpellingWord(wordCursor.selectedText()))
                continue;
            result.append(
                QVariantMap { { QStringLiteral("start"), start }, { QStringLiteral("length"), range.length } });
        }
    }
    return result;
}

QStringList EditorPlatformBackend::spellingSuggestions(const QString &word) const
{
    if (!spellCheckEnabled_)
        return {};
    for (const auto &item : extensions_) {
        if (item.type != int(NoteHighlighter::SpellCheck))
            continue;
        if (auto spell = std::dynamic_pointer_cast<SpellCheckExtension>(item.extension))
            return spell->suggestions(word);
    }
    return {};
}

void EditorPlatformBackend::addToSpellingDictionary(const QString &word)
{
    QStringList words = customSpellingDictionary_;
    words.append(word);
    setCustomSpellingDictionary(words);
}

QStringList EditorPlatformBackend::customSpellingDictionary() const { return customSpellingDictionary_; }

void EditorPlatformBackend::setCustomSpellingDictionary(const QStringList &words)
{
    QStringList normalized;
    for (QString word : words) {
        word = word.trimmed();
        if (!word.isEmpty() && !normalized.contains(word, Qt::CaseInsensitive))
            normalized.append(word);
    }
    std::sort(normalized.begin(), normalized.end(),
              [](const QString &left, const QString &right) { return QString::localeAwareCompare(left, right) < 0; });
    if (normalized == customSpellingDictionary_)
        return;
    customSpellingDictionary_ = normalized;
    saveCustomSpellingDictionary();
    rehighlight();
    emit customSpellingDictionaryChanged();
}

bool EditorPlatformBackend::isCustomSpellingWord(const QString &word) const
{
    return customSpellingDictionary_.contains(word.trimmed(), Qt::CaseInsensitive);
}

void EditorPlatformBackend::saveCustomSpellingDictionary()
{
    QSettings().setValue(QStringLiteral("editor/customSpellingDictionary"), customSpellingDictionary_);
}

bool EditorPlatformBackend::insertClipboardImage(int row)
{
    if (!canInsertImages())
        return false;
    const auto *mimeData = QGuiApplication::clipboard()->mimeData();
    if (!mimeData)
        return false;

    NoteTransferController controller;
    const auto             imported = controller.importMimeData(mimeData);
    const bool structured = imported && !imported.hasImage() && imported.sourceMimeType != QStringLiteral("text/plain")
        && !imported.fragment.blocks.isEmpty();
    if (structured)
        return false;

    if (mimeData->hasImage()) {
        const auto image = qvariant_cast<QImage>(mimeData->imageData());
        if (!image.isNull()) {
            const QString name
                = QStringLiteral("Clipboard_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
            return insertRasterImage(image, name, row);
        }
    }
    return insertImageMimeData(mimeData, row);
}

bool EditorPlatformBackend::insertImageData(const QByteArray &data, const QString &name, const QString &mediaType,
                                            int row)
{
    if (!canInsertImages() || data.isEmpty())
        return false;
    const auto imported = LocalMediaStore::instance()->importData(data, name, mediaType);
    if (!imported) {
        emit operationFailed(imported.error);
        return false;
    }
    return insertImportedImages({ imported.value }, row, QStringLiteral("insert-image"));
}

bool EditorPlatformBackend::insertImage(int row)
{
    if (!canInsertImages())
        return false;
    emit imageInsertionRequested(row);
    return true;
}

void EditorPlatformBackend::saveImageAs(const QString &) { }
bool EditorPlatformBackend::startImageDrag(int) { return false; }

bool EditorPlatformBackend::insertRasterImage(const QImage &image, const QString &name, int row)
{
    if (!canInsertImages() || image.isNull())
        return false;
    QByteArray encoded;
    QBuffer    buffer(&encoded);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) {
        emit operationFailed(tr("Could not encode the image."));
        return false;
    }
    return insertImageData(encoded, name, QStringLiteral("image/png"), row);
}

bool EditorPlatformBackend::insertImageFiles(const QStringList &fileNames, int row, QString *error)
{
    if (!canInsertImages() || fileNames.isEmpty())
        return false;
    QList<MediaReference> references;
    for (const auto &fileName : fileNames) {
        const auto imported = LocalMediaStore::instance()->importFile(fileName);
        if (!imported) {
            if (error)
                *error = imported.error;
            emit operationFailed(imported.error);
            return false;
        }
        references.append(imported.value);
    }
    return insertImportedImages(references, row, QStringLiteral("insert-images"));
}

bool EditorPlatformBackend::canAcceptImageMimeData(const QMimeData *mimeData) const
{
    if (!canInsertImages() || !mimeData)
        return false;
    NoteTransferController controller;
    const auto             imported = controller.importMimeData(mimeData);
    return (imported && (imported.hasImage() || canInsertImageFragment(imported.fragment)))
        || !localImageFiles(mimeData).isEmpty();
}

bool EditorPlatformBackend::insertImageMimeData(const QMimeData *mimeData, int row)
{
    if (!canAcceptImageMimeData(mimeData))
        return false;
    NoteTransferController controller;
    const auto             imported = controller.importMimeData(mimeData);
    if (imported && canInsertImageFragment(imported.fragment))
        return insertImageFragment(imported.fragment, row);
    if (imported && imported.hasImage()) {
        const QString name
            = QStringLiteral("Dropped_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
        return insertRasterImage(imported.image, name, row);
    }
    return insertImageFiles(localImageFiles(mimeData), row);
}

bool EditorPlatformBackend::canInsertImageFragment(const NoteFragment &fragment) const
{
    return canInsertImages() && isUsableImageFragment(fragment);
}

bool EditorPlatformBackend::insertImageFragment(const NoteFragment &sourceFragment, int row)
{
    if (!editor_ || !canInsertImageFragment(sourceFragment))
        return false;

    NoteFragment          fragment = sourceFragment;
    QList<MediaReference> insertedMedia;
    if (!fragment.media.isEmpty()) {
        const auto cloned = NoteFragmentMediaTransfer::cloneForDestination(fragment, *LocalMediaStore::instance(),
                                                                           LocalMediaStore::instance());
        if (!cloned) {
            qWarning() << "Image media import failed:" << cloned.error;
            emit operationFailed(cloned.error);
            return false;
        }
        fragment      = cloned.fragment;
        insertedMedia = cloned.importedMedia;
    }

    const int insertionRow = row < 0 ? editor_->model()->rowCount() : qBound(0, row, editor_->model()->rowCount());
    editor_->beginHistoryTransaction(QStringLiteral("drop-image"));
    QString    error;
    const bool inserted = editor_->model()->insertBlockFragment(insertionRow, fragment, &error);
    if (inserted && !insertedMedia.isEmpty()) {
        auto manifest = editor_->media();
        manifest.append(insertedMedia);
        editor_->setMedia(manifest);
        emit mediaInserted(insertedMedia);
    }
    editor_->endHistoryTransaction();

    if (!inserted) {
        qWarning() << "Image insertion failed:" << error;
        emit operationFailed(error);
    }
    return inserted;
}

void EditorPlatformBackend::setSpellCheckEnabled(bool enabled)
{
    if (spellCheckEnabled_ == enabled)
        return;
    spellCheckEnabled_ = enabled;
    for (const auto &registered : std::as_const(highlighters_)) {
        if (!registered.highlighter)
            continue;
        if (enabled)
            registered.highlighter->enableExtension(NoteHighlighter::SpellCheck);
        else
            registered.highlighter->disableExtension(NoteHighlighter::SpellCheck);
    }
    rehighlight();
    emit spellCheckEnabledChanged();
}

void EditorPlatformBackend::addHighlightExtension(const std::shared_ptr<HighlighterExtension> &extension, int type)
{
    if (!extension)
        return;
    extensions_.append({ extension, type });
    for (const auto &registered : std::as_const(highlighters_)) {
        if (!registered.highlighter || type == int(NoteHighlighter::Other))
            continue;
        const auto extensionType = NoteHighlighter::ExtType(type);
        registered.highlighter->addExtension(extension, extensionType);
        if (extensionType == NoteHighlighter::Title && !registered.titleDocument)
            registered.highlighter->disableExtension(extensionType);
    }
    rehighlight();
}

void EditorPlatformBackend::rehighlight()
{
    highlighters_.removeIf([](const auto &registered) { return registered.highlighter.isNull(); });
    for (const auto &registered : std::as_const(highlighters_))
        registered.highlighter->rehighlight();
    emit highlightingChanged();
}

void EditorPlatformBackend::reloadVisualSettings()
{
    auto title = std::dynamic_pointer_cast<FirstLineHighlighter>(titleExtension_);
    if (!title)
        return;
    const QColor configured
        = QSettings().value(QStringLiteral("ui.title-color"), Defaults::firstLineHighlightColor()).value<QColor>();
    title->setColor(Utils::mergeColors(configured, qGuiApp->palette().color(QPalette::Text)));
    rehighlight();
}

bool EditorPlatformBackend::insertImportedImages(const QList<MediaReference> &references, int row,
                                                 const QString &historyKind)
{
    if (!editor_ || references.isEmpty() || !canInsertImages())
        return false;
    editor_->beginHistoryTransaction(historyKind);
    auto media = editor_->media();
    media.append(references);
    editor_->setMedia(media);
    int insertionRow = row < 0 ? editor_->model()->rowCount() : qBound(0, row, editor_->model()->rowCount());
    for (const auto &reference : references)
        editor_->model()->insertImage(insertionRow++, reference.uri(), reference.originalName);
    editor_->endHistoryTransaction();
    emit mediaInserted(references);
    return true;
}

void EditorPlatformBackend::clearRegisteredDocuments()
{
    for (const auto &registered : std::as_const(highlighters_)) {
        if (registered.highlighter)
            delete registered.highlighter.data();
    }
    highlighters_.clear();
}

void EditorPlatformBackend::installBuiltInExtensions()
{
    titleExtension_ = std::make_shared<FirstLineHighlighter>();
    extensions_.append({ titleExtension_, int(NoteHighlighter::Title) });
    reloadVisualSettings();
}

} // namespace QtNote
