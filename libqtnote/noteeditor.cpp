#include "noteeditor.h"

#include "draftmanager.h"
#include "noteblockmodel.h"
#include "notedocumenthistory.h"
#include "notestorage.h"
#include "utils.h"

#include <QDebug>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QTimer>

namespace QtNote {

Q_LOGGING_CATEGORY(logEditorPersistence, "qtnote.persistence.editor")

NoteEditor::NoteEditor(const Note &note, const QUuid &draftId, QObject *parent) :
    NoteEditor(note, *DraftManager::instance(), draftId, parent)
{
}

NoteEditor::NoteEditor(const Note &note, DraftManager &drafts, const QUuid &draftId, QObject *parent) :
    QObject(parent), note_(note), drafts_(&drafts), model_(new NoteBlockModel(this)),
    history_(std::make_unique<NoteDocumentHistory>())
{
    draftId_ = drafts_->acquireEditingSession(note_, draftId);
    qCInfo(logEditorPersistence) << "Editor session created: draft=" << draftId_.toString(QUuid::WithoutBraces)
                                 << "storage=" << note_.storageId() << "noteIdPresent=" << !note_.id().isEmpty()
                                 << "knownDraft=" << draftId.toString(QUuid::WithoutBraces);
    const auto draft = drafts_->editingDraft(draftId_);
    if (draft)
        adoptEditingDraft(draft.value);
    loadFromNote();
    media_ = note_.media();
    if (!note_.isNull())
        model_->load(text_, isMarkdown());
    updateMediaPreviewUrls();

    history_->setRestoreHandler([this](const NoteDocumentHistory::DocumentState &state, const QVariantMap &viewState) {
        prepareEditorViewForHistoryRestore();
        const bool modeChanged = model_->markdown() != state.model.markdown;
        if (media_ != state.media)
            setMedia(state.media);
        model_->restoreState(state.model);
        scheduleEditorViewRestore(viewState);
        emit historyDocumentRestored(modeChanged);
    });
    history_->setScalarRestoreHandler(
        [this](const NoteDocumentHistory::ScalarAddress &address, const QString &value, const QVariantMap &viewState) {
            prepareEditorViewForHistoryRestore();
            restoreScalarField(address.blockIndex, address.role, address.fieldIndex, value);
            scheduleEditorViewRestore(viewState);
        });
    history_->setChangedHandler([this] { emit undoStateChanged(); });
    history_->reset({ model_->state(), media_ });

    connect(model_, &NoteBlockModel::scalarEdited, this,
            [this](int row, int role, int fieldIndex, const QString &before, const QString &after) {
                scalarHistoryChangePending_ = true;
                if (!history_->isRestoring() && !history_->inTransaction()) {
                    history_->observeScalar({ row, role, fieldIndex }, before, after, captureEditorViewState());
                }
            });
    connect(model_, &NoteBlockModel::contentsChanged, this, [this] {
        const auto contents      = model_->contents();
        const auto format        = model_->markdown() ? Note::Markdown : Note::PlainText;
        const bool changedText   = text_ != contents;
        const bool changedFormat = format_ != format;
        text_                    = contents;
        format_                  = format;
        if (changedText)
            emit textChanged();
        if (changedFormat)
            emit formatChanged();
        setDirty(true);

        if (scalarHistoryChangePending_) {
            scalarHistoryChangePending_ = false;
            return;
        }
        if (!history_->isRestoring() && !history_->inTransaction())
            history_->observeChange({ model_->state(), media_ }, captureEditorViewState());
    });
}

NoteEditor::~NoteEditor()
{
    qCInfo(logEditorPersistence) << "Editor session destroyed: draft=" << draftId_.toString(QUuid::WithoutBraces)
                                 << "storage=" << note_.storageId() << "noteIdPresent=" << !note_.id().isEmpty()
                                 << "dirty=" << dirty_ << "released=" << sessionReleased_;
    // QUndoStack emits state changes while it is being destroyed. Detach the
    // outward callback before QObject children or a registered view can enter
    // their base-class destructors.
    history_->setChangedHandler({});
    if (!sessionReleased_)
        drafts_->releaseEditingSession(draftId_);
}

void NoteEditor::loadFromNote()
{
    if (note_.isNull()) {
        text_.clear();
        baselineText_ = text_;
        format_ = baselineFormat_ = Note::PlainText;
        baselineFolderId_         = {};
        return;
    }
    if (!note_.isLoaded())
        note_.load();
    format_           = note_.format() == Note::PlainText ? Note::PlainText : Note::Markdown;
    text_             = format_ == Note::PlainText ? note_.title() + QLatin1Char('\n') + note_.text()
                                                   : note_.title() + QLatin1String("\n\n") + note_.text();
    baselineText_     = text_;
    baselineFormat_   = format_;
    baselineFolderId_ = note_.folderId();
}

void NoteEditor::adoptEditingDraft(const DraftRecord &draft)
{
    note_.setTitle(draft.title);
    note_.setText(draft.body, draft.format);
    note_.setFolderId(draft.folderId);
    note_.setMedia(draft.media);
    note_.setBackendData(draft.backendData);
    folderUserOverride_ = draft.folderUserOverride;
    draftPersisted_ = true;
    draftRevision_  = draft.revision;
}

void NoteEditor::setText(const QString &text)
{
    if (text_ == text)
        return;
    text_ = text;
    emit textChanged();
    setDirty(true);
}

void NoteEditor::setMarkdown(bool markdown)
{
    const auto target = markdown ? Note::Markdown : Note::PlainText;
    if (format_ == target && model_->markdown() == markdown)
        return;
    loadDocument(text_, target, LoadPolicy::RecordFormatConversion);
}

bool NoteEditor::save()
{
    qCInfo(logEditorPersistence) << "Editor checkpoint requested: draft=" << draftId_.toString(QUuid::WithoutBraces)
                                 << "storage=" << note_.storageId() << "noteIdPresent=" << !note_.id().isEmpty()
                                 << "dirty=" << dirty_ << "textLength=" << text_.size() << "format=" << int(format_);
    if (!dirty_) {
        qCInfo(logEditorPersistence) << "Editor checkpoint skipped because document is clean"
                                     << draftId_.toString(QUuid::WithoutBraces);
        return true;
    }

    const bool hasEditingDraft = draftPersisted_ || bool(drafts_->editingDraft(draftId_));
    if (text_ == baselineText_ && format_ == baselineFormat_ && !metadataDirty_ && !hasEditingDraft) {
        setMetadataDirty(false);
        setDirty(false);
        return true;
    }

    const auto split = Utils::splitTitle(text_);
    note_.setTitle(split.first);
    note_.setText(split.second, format_);
    auto media = media_;
    media.removeIf([this](const MediaReference &reference) {
        return !text_.contains(reference.id.toString(QUuid::WithoutBraces), Qt::CaseInsensitive);
    });
    note_.setMedia(media);
    if (media_ != media) {
        media_ = media;
        updateMediaPreviewUrls();
        emit mediaChanged(media_);
    }

    const auto result
        = drafts_->saveEditing(draftId_, note_, split.first, split.second, format_, folderUserOverride_);
    if (result) {
        qCWarning(logEditorPersistence) << "Editor checkpoint failed:" << draftId_.toString(QUuid::WithoutBraces)
                                        << int(result.code) << result.message;
        return setError(result.message);
    }

    baselineFolderId_ = note_.folderId();
    setMetadataDirty(false);
    setDirty(false);
    draftPersisted_ = true;
    if (const auto draft = drafts_->editingDraft(draftId_); draft)
        draftRevision_ = draft.value.revision;
    qCInfo(logEditorPersistence) << "Editor checkpoint completed: draft=" << draftId_.toString(QUuid::WithoutBraces)
                                 << "revision=" << draftRevision_;
    return true;
}

bool NoteEditor::close()
{
    qCInfo(logEditorPersistence) << "Editor close requested: draft=" << draftId_.toString(QUuid::WithoutBraces)
                                 << "storage=" << note_.storageId() << "noteIdPresent=" << !note_.id().isEmpty()
                                 << "dirty=" << dirty_ << "released=" << sessionReleased_;
    if (sessionReleased_)
        return true;
    if (dirty_ && !save())
        return false;

    if (drafts_->isLastEditingSession(draftId_)) {
        const auto draft = drafts_->editingDraft(draftId_);
        if (draft) {
            const auto result = drafts_->markReady(draftId_);
            if (result) {
                qCWarning(logEditorPersistence)
                    << "Failed to make editor draft publishable" << draftId_.toString(QUuid::WithoutBraces)
                    << int(result.code) << result.message;
                return setError(result.message);
            }
            qCInfo(logEditorPersistence) << "Editor draft marked ready for publication"
                                         << draftId_.toString(QUuid::WithoutBraces);
        } else if (draft.error.code != DraftStoreError::NotFound) {
            return setError(draft.error.message);
        }
    }
    drafts_->releaseEditingSession(draftId_);
    sessionReleased_ = true;
    qCInfo(logEditorPersistence) << "Editor close completed" << draftId_.toString(QUuid::WithoutBraces);
    return true;
}

bool NoteEditor::discardDraft()
{
    if (draftPersisted_) {
        const auto result = drafts_->discard(draftId_);
        if (result)
            return setError(result.message);
    }
    draftPersisted_ = false;
    return true;
}

bool NoteEditor::discardAndClose()
{
    if (sessionReleased_)
        return true;
    const auto result = drafts_->discard(draftId_);
    if (result && result.code != DraftStoreError::NotFound)
        return setError(result.message);
    draftPersisted_ = false;
    drafts_->releaseEditingSession(draftId_);
    sessionReleased_ = true;
    setMetadataDirty(false);
    setDirty(false);
    return true;
}

void NoteEditor::setDirty(bool dirty)
{
    if (contentDirty_ == dirty)
        return;
    contentDirty_ = dirty;
    updateDirty();
}

void NoteEditor::setMetadataDirty(bool dirty)
{
    if (metadataDirty_ == dirty)
        return;
    metadataDirty_ = dirty;
    updateDirty();
}

void NoteEditor::updateDirty()
{
    const auto dirty = contentDirty_ || metadataDirty_;
    if (dirty_ == dirty)
        return;
    dirty_ = dirty;
    emit dirtyChanged();
}

bool NoteEditor::setError(const QString &error)
{
    qWarning() << "Note editing session failed:" << error;
    if (errorString_ != error) {
        errorString_ = error;
        emit errorStringChanged();
    }
    return false;
}

void NoteEditor::setMedia(const QList<MediaReference> &media)
{
    if (media_ == media)
        return;
    media_ = media;
    note_.setMedia(media);
    updateMediaPreviewUrls();
    emit mediaChanged(media);
}

void NoteEditor::setFolderId(const QUuid &folderId)
{
    if (note_.isNull() || note_.folderId() == folderId)
        return;
    note_.setFolderId(folderId);
    setMetadataDirty(note_.folderId() != baselineFolderId_);
    emit folderIdChanged();
}

void NoteEditor::markFolderPersisted(const QUuid &folderId)
{
    if (note_.isNull() || note_.folderId() != folderId)
        return;
    baselineFolderId_ = folderId;
    setMetadataDirty(false);
}

QObject *NoteEditor::blockModel() const { return model_; }

void NoteEditor::resetContent(const QString &text, Note::Format format)
{
    const bool textChanged   = text_ != text;
    const bool formatChanged = format_ != format;
    text_                    = text;
    format_                  = format == Note::PlainText ? Note::PlainText : Note::Markdown;
    baselineText_            = text_;
    baselineFormat_          = format_;
    setDirty(false);
    if (textChanged)
        emit this->textChanged();
    if (formatChanged)
        emit this->formatChanged();
}

void NoteEditor::resetHistory() { history_->reset({ model_->state(), media_ }); }

bool NoteEditor::canUndo() const { return history_->canUndo(); }

bool NoteEditor::canRedo() const { return history_->canRedo(); }

QString NoteEditor::undoText() const { return history_->undoText(); }

QString NoteEditor::redoText() const { return history_->redoText(); }

bool NoteEditor::supportsMedia() const { return note_.storage() && note_.storage()->supportsMedia(); }

bool NoteEditor::canInsertImages() const { return supportsMedia() && isMarkdown(); }

bool NoteEditor::historyInTransaction() const { return history_->inTransaction(); }

void NoteEditor::registerEditorView(QObject *view) { editorView_ = view; }

void NoteEditor::beginHistoryTransaction(const QString &kind, const QVariantMap &beforeView)
{
    history_->beginTransaction(kind, { model_->state(), media_ },
                               beforeView.isEmpty() ? captureEditorViewState() : beforeView);
}

void NoteEditor::endHistoryTransaction(const QVariantMap &afterView)
{
    history_->endTransaction({ model_->state(), media_ }, afterView.isEmpty() ? captureEditorViewState() : afterView);
}

void NoteEditor::updateHistoryViewState(const QVariantMap &viewState, bool breakMerge)
{
    history_->updateViewState(viewState, breakMerge);
}

bool NoteEditor::undo()
{
    if (editorView_)
        QMetaObject::invokeMethod(editorView_, "flushPendingEditorChanges");
    if (!canUndo())
        return false;
    history_->undo();
    return true;
}

bool NoteEditor::redo()
{
    if (editorView_)
        QMetaObject::invokeMethod(editorView_, "flushPendingEditorChanges");
    if (!canRedo())
        return false;
    history_->redo();
    return true;
}

void NoteEditor::breakHistoryMerge() { history_->breakMerge(); }

QVariantMap NoteEditor::captureEditorViewState() const
{
    if (!editorView_)
        return {};
    QVariant result;
    if (!QMetaObject::invokeMethod(editorView_, "captureEditorState", Q_RETURN_ARG(QVariant, result)))
        return {};
    return result.toMap();
}

void NoteEditor::prepareEditorViewForHistoryRestore()
{
    if (editorView_)
        QMetaObject::invokeMethod(editorView_, "prepareForHistoryRestore");
}

void NoteEditor::scheduleEditorViewRestore(const QVariantMap &viewState)
{
    QTimer::singleShot(0, this, [this, viewState] {
        if (editorView_)
            QMetaObject::invokeMethod(editorView_, "restoreEditorState", Q_ARG(QVariant, viewState));
    });
}

void NoteEditor::restoreScalarField(int blockIndex, int role, int fieldIndex, const QString &value)
{
    switch (role) {
    case NoteBlockModel::TextRole:
        model_->setBlockText(blockIndex, value);
        break;
    case NoteBlockModel::ItemsRole:
        model_->setListItem(blockIndex, fieldIndex, value);
        break;
    case NoteBlockModel::CellsRole:
        model_->setTableCell(blockIndex, fieldIndex, value);
        break;
    case NoteBlockModel::UrlRole:
        model_->setImageUrl(blockIndex, value);
        break;
    case NoteBlockModel::AltRole:
        model_->setImageAlt(blockIndex, value);
        break;
    }
}

void NoteEditor::updateMediaPreviewUrls()
{
    QHash<QString, QString> urls;
    for (const auto &reference : media_) {
        if (reference.isValid() && reference.mediaType.startsWith(QLatin1String("image/"))) {
            urls.insert(reference.uri(),
                        QStringLiteral("image://qtnote-media/%1").arg(QString::fromLatin1(reference.blobId.toHex())));
        }
    }
    model_->setPreviewUrls(urls);
}

bool NoteEditor::reloadNewerDraft()
{
    if (dirty_)
        return false;
    const auto draft = drafts_->editingDraft(draftId_);
    if (!draft || draft.value.state != DraftRecord::Editing || draft.value.revision <= draftRevision_)
        return false;
    const auto previousFolderId = note_.folderId();
    adoptEditingDraft(draft.value);
    loadFromNote();
    media_ = note_.media();
    model_->load(text_, isMarkdown());
    updateMediaPreviewUrls();
    setMetadataDirty(false);
    setDirty(false);
    emit textChanged();
    emit formatChanged();
    if (previousFolderId != note_.folderId())
        emit folderIdChanged();
    return true;
}

} // namespace QtNote
