#include "notedocumenthistory.h"

#include <QCoreApplication>
#include <QUndoCommand>

namespace AnyKeep {
namespace {
    QString typingKind(const QString &before, const QString &after, const QVariantMap &beforeView)
    {
        const QVariantMap active = beforeView.value(QStringLiteral("active")).toMap();
        if (active.value(QStringLiteral("selectionStart")).toInt()
            != active.value(QStringLiteral("selectionEnd")).toInt()) {
            return {};
        }
        if (after.size() > before.size())
            return QStringLiteral("typing-insertion");
        if (after.size() < before.size())
            return QStringLiteral("typing-deletion");
        return {};
    }

    QString commandText(const QString &kind)
    {
        const char *source = nullptr;
        if (kind == QLatin1String("typing-insertion"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Typing");
        else if (kind == QLatin1String("typing-deletion") || kind == QLatin1String("delete-selection"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Delete");
        else if (kind == QLatin1String("paste") || kind == QLatin1String("paste-code"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Paste");
        else if (kind == QLatin1String("cut"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Cut");
        else if (kind == QLatin1String("insert-text"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Insert text");
        else if (kind == QLatin1String("insert-image"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Insert image");
        else if (kind == QLatin1String("remove-image") || kind == QLatin1String("remove-adjacent-image"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Remove image");
        else if (kind == QLatin1String("resize-image"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Resize image");
        else if (kind == QLatin1String("align-image"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Align image");
        else if (kind == QLatin1String("reset-image-presentation"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Reset image size and alignment");
        else if (kind == QLatin1String("insert-table"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Insert table");
        else if (kind == QLatin1String("insert-code-block"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Insert code block");
        else if (kind == QLatin1String("remove-table"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Remove table");
        else if (kind == QLatin1String("insert-table-row"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Insert table row");
        else if (kind == QLatin1String("remove-table-row"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Remove table row");
        else if (kind == QLatin1String("insert-table-column"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Insert table column");
        else if (kind == QLatin1String("remove-table-column"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Remove table column");
        else if (kind == QLatin1String("insert-or-convert-list"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Insert or convert list");
        else if (kind == QLatin1String("split-list-item"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Split list item");
        else if (kind == QLatin1String("merge-list-items"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Merge list items");
        else if (kind == QLatin1String("remove-list-item"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Remove list item");
        else if (kind == QLatin1String("indent-list-items"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Change list indentation");
        else if (kind == QLatin1String("convert-list-level"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Change list type");
        else if (kind == QLatin1String("convert-heading"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Change heading");
        else if (kind == QLatin1String("inline-format"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Format text");
        else if (kind == QLatin1String("set-link") || kind == QLatin1String("leave-link"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Edit link");
        else if (kind == QLatin1String("spell-replace"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Correct spelling");
        else if (kind == QLatin1String("toggle-task"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Toggle task");
        else if (kind == QLatin1String("move-block") || kind == QLatin1String("move-list-block")
                 || kind == QLatin1String("move-list-item"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Move block");
        else if (kind == QLatin1String("append-following-text-block"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Add text block");
        else if (kind == QLatin1String("leave-table"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Leave table");
        else if (kind == QLatin1String("format-conversion"))
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Change note format");
        else
            source = QT_TRANSLATE_NOOP("NoteDocumentHistory", "Edit");
        return QCoreApplication::translate("NoteDocumentHistory", source);
    }
}

class NoteDocumentHistory::SnapshotCommand final : public QUndoCommand {
public:
    SnapshotCommand(NoteDocumentHistory *history, const QString &kind, DocumentState before, QVariantMap beforeView,
                    DocumentState after, QVariantMap afterView) :
        QUndoCommand(commandText(kind)), history_(history), before_(std::move(before)),
        beforeView_(std::move(beforeView)), after_(std::move(after)), afterView_(std::move(afterView))
    {
    }

    void undo() override { history_->apply(before_, beforeView_); }

    void redo() override
    {
        if (firstRedo_) {
            firstRedo_ = false;
            return;
        }
        history_->apply(after_, afterView_);
    }

private:
    NoteDocumentHistory *history_ = nullptr;
    DocumentState        before_;
    QVariantMap          beforeView_;
    DocumentState        after_;
    QVariantMap          afterView_;
    bool                 firstRedo_ = true;
};

class NoteDocumentHistory::ScalarCommand final : public QUndoCommand {
public:
    ScalarCommand(NoteDocumentHistory *history, QString kind, ScalarAddress address, QString before,
                  QVariantMap beforeView, QString after, QVariantMap afterView, bool mergeable, int mergeGeneration,
                  qint64 timestamp) :
        QUndoCommand(commandText(kind)), history_(history), kind_(std::move(kind)), address_(address),
        before_(std::move(before)), beforeView_(std::move(beforeView)), after_(std::move(after)),
        afterView_(std::move(afterView)), mergeable_(mergeable), mergeGeneration_(mergeGeneration),
        timestamp_(timestamp)
    {
    }

    void undo() override { history_->applyScalar(address_, before_, beforeView_); }

    void redo() override
    {
        if (firstRedo_) {
            firstRedo_ = false;
            return;
        }
        history_->applyScalar(address_, after_, afterView_);
    }

    int id() const override { return mergeable_ ? 0x514E4853 : -1; }

    bool mergeWith(const QUndoCommand *command) override
    {
        const auto *next = dynamic_cast<const ScalarCommand *>(command);
        if (!next || !mergeable_ || !next->mergeable_ || kind_ != next->kind_ || !(address_ == next->address_)
            || mergeGeneration_ != next->mergeGeneration_ || after_ != next->before_
            || next->timestamp_ - timestamp_ > 1000) {
            return false;
        }
        after_     = next->after_;
        afterView_ = next->afterView_;
        timestamp_ = next->timestamp_;
        setObsolete(before_ == after_);
        return true;
    }

private:
    NoteDocumentHistory *history_ = nullptr;
    QString              kind_;
    ScalarAddress        address_;
    QString              before_;
    QVariantMap          beforeView_;
    QString              after_;
    QVariantMap          afterView_;
    bool                 mergeable_       = false;
    int                  mergeGeneration_ = 0;
    qint64               timestamp_       = 0;
    bool                 firstRedo_       = true;
};

NoteDocumentHistory::NoteDocumentHistory()
{
    elapsed_.start();
    stack_.setUndoLimit(256);
    QObject::connect(&stack_, &QUndoStack::canUndoChanged, [this] { changed(); });
    QObject::connect(&stack_, &QUndoStack::canRedoChanged, [this] { changed(); });
    QObject::connect(&stack_, &QUndoStack::undoTextChanged, [this] { changed(); });
    QObject::connect(&stack_, &QUndoStack::redoTextChanged, [this] { changed(); });
}

bool NoteDocumentHistory::statesEqual(const DocumentState &left, const DocumentState &right)
{
    return left.model == right.model && left.media == right.media;
}

void NoteDocumentHistory::reset(const DocumentState &state)
{
    stack_.clear();
    current_ = state;
    currentView_.clear();
    transactionDepth_ = 0;
    ++mergeGeneration_;
    changed();
}

void NoteDocumentHistory::beginTransaction(const QString &kind, const DocumentState &before,
                                           const QVariantMap &beforeView)
{
    if (restoring_)
        return;
    if (transactionDepth_++ > 0)
        return;
    transactionBefore_     = before;
    transactionBeforeView_ = beforeView;
    transactionKind_       = kind;
    currentView_           = beforeView;
}

void NoteDocumentHistory::endTransaction(const DocumentState &after, const QVariantMap &afterView)
{
    if (restoring_ || transactionDepth_ <= 0)
        return;
    if (--transactionDepth_ > 0)
        return;

    const bool hasChanges = !statesEqual(transactionBefore_, after);
    if (hasChanges)
        pushSnapshot(transactionKind_, transactionBefore_, transactionBeforeView_, after, afterView);
    if (hasChanges)
        current_ = after;
    currentView_ = afterView;
    if (hasChanges)
        ++mergeGeneration_;
}

void NoteDocumentHistory::observeChange(const DocumentState &after, const QVariantMap &afterView)
{
    if (restoring_ || inTransaction())
        return;
    if (!statesEqual(current_, after)) {
        pushSnapshot(QStringLiteral("edit"), current_, currentView_, after, afterView);
        ++mergeGeneration_;
    }
    current_     = after;
    currentView_ = afterView;
}

void NoteDocumentHistory::observeScalar(const ScalarAddress &address, const QString &before, const QString &after,
                                        const QVariantMap &afterView)
{
    if (restoring_ || inTransaction() || before == after)
        return;
    const QString kind = typingKind(before, after, currentView_);
    pushScalar(kind.isEmpty() ? QStringLiteral("edit") : kind, address, before, currentView_, after, afterView,
               !kind.isEmpty());
    setCurrentScalar(address, after);
    currentView_ = afterView;
}

void NoteDocumentHistory::updateViewState(const QVariantMap &viewState, bool breakMerge)
{
    if (restoring_ || inTransaction())
        return;
    currentView_ = viewState;
    if (breakMerge)
        ++mergeGeneration_;
}

void NoteDocumentHistory::breakMerge()
{
    if (!restoring_)
        ++mergeGeneration_;
}

void NoteDocumentHistory::pushSnapshot(const QString &kind, const DocumentState &before, const QVariantMap &beforeView,
                                       const DocumentState &after, const QVariantMap &afterView)
{
    if (!statesEqual(before, after))
        stack_.push(new SnapshotCommand(this, kind, before, beforeView, after, afterView));
}

void NoteDocumentHistory::pushScalar(const QString &kind, const ScalarAddress &address, const QString &before,
                                     const QVariantMap &beforeView, const QString &after, const QVariantMap &afterView,
                                     bool mergeable)
{
    if (before != after) {
        stack_.push(new ScalarCommand(this, kind, address, before, beforeView, after, afterView, mergeable,
                                      mergeGeneration_, elapsed_.elapsed()));
    }
}

void NoteDocumentHistory::apply(const DocumentState &state, const QVariantMap &viewState)
{
    if (!restoreHandler_)
        return;
    restoring_ = true;
    restoreHandler_(state, viewState);
    current_     = state;
    currentView_ = viewState;
    restoring_   = false;
    ++mergeGeneration_;
}

void NoteDocumentHistory::applyScalar(const ScalarAddress &address, const QString &value, const QVariantMap &viewState)
{
    if (!scalarRestoreHandler_)
        return;
    restoring_ = true;
    scalarRestoreHandler_(address, value, viewState);
    setCurrentScalar(address, value);
    currentView_ = viewState;
    restoring_   = false;
    ++mergeGeneration_;
}

void NoteDocumentHistory::setCurrentScalar(const ScalarAddress &address, const QString &value)
{
    if (address.blockIndex < 0 || address.blockIndex >= current_.model.blocks.size())
        return;
    auto &block = current_.model.blocks[address.blockIndex];
    switch (address.role) {
    case NoteBlockModel::TextRole:
        block.text = value;
        break;
    case NoteBlockModel::ItemsRole:
        if (address.fieldIndex >= 0 && address.fieldIndex < block.items.size())
            block.items[address.fieldIndex] = value;
        break;
    case NoteBlockModel::CellsRole:
        if (address.fieldIndex >= 0 && address.fieldIndex < block.cells.size())
            block.cells[address.fieldIndex] = value;
        break;
    case NoteBlockModel::UrlRole:
        block.url = value;
        break;
    case NoteBlockModel::AltRole:
        block.alt = value;
        break;
    case NoteBlockModel::ImageWidthRole:
        block.imageWidth = value.toInt();
        break;
    case NoteBlockModel::ImageAlignmentRole:
        block.imageAlignment = value;
        break;
    case NoteBlockModel::LanguageRole:
        block.language = value;
        break;
    case NoteBlockModel::AudioTranscriptRole:
        block.audioTranscript = value;
        break;
    }
}

void NoteDocumentHistory::undo()
{
    if (stack_.canUndo())
        stack_.undo();
}

void NoteDocumentHistory::redo()
{
    if (stack_.canRedo())
        stack_.redo();
}

void NoteDocumentHistory::changed()
{
    if (changedHandler_)
        changedHandler_();
}

} // namespace AnyKeep
