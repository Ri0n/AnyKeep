#ifndef NOTEDOCUMENTHISTORY_H
#define NOTEDOCUMENTHISTORY_H

#include <QElapsedTimer>
#include <QUndoStack>
#include <QVariantMap>

#include <functional>

#include "mediareference.h"
#include "noteblockmodel.h"

namespace AnyKeep {

// Per-editor transient history. Persistence deliberately remains the
// responsibility of NoteEditor and DraftManager.
class NoteDocumentHistory {
public:
    struct DocumentState {
        NoteBlockModel::State model;
        QList<MediaReference> media;
    };

    struct ScalarAddress {
        int blockIndex = -1;
        int role       = -1;
        int fieldIndex = -1;

        bool operator==(const ScalarAddress &other) const
        {
            return blockIndex == other.blockIndex && role == other.role && fieldIndex == other.fieldIndex;
        }
    };

    using RestoreHandler       = std::function<void(const DocumentState &, const QVariantMap &)>;
    using ScalarRestoreHandler = std::function<void(const ScalarAddress &, const QString &, const QVariantMap &)>;
    using ChangedHandler       = std::function<void()>;

    NoteDocumentHistory();

    void setRestoreHandler(RestoreHandler handler) { restoreHandler_ = std::move(handler); }
    void setScalarRestoreHandler(ScalarRestoreHandler handler) { scalarRestoreHandler_ = std::move(handler); }
    void setChangedHandler(ChangedHandler handler) { changedHandler_ = std::move(handler); }

    void reset(const DocumentState &state);
    void beginTransaction(const QString &kind, const DocumentState &before, const QVariantMap &beforeView);
    void endTransaction(const DocumentState &after, const QVariantMap &afterView);
    void observeChange(const DocumentState &after, const QVariantMap &afterView);
    void observeScalar(const ScalarAddress &address, const QString &before, const QString &after,
                       const QVariantMap &afterView);
    void updateViewState(const QVariantMap &viewState, bool breakMerge);
    void breakMerge();

    bool    canUndo() const { return stack_.canUndo(); }
    bool    canRedo() const { return stack_.canRedo(); }
    QString undoText() const { return stack_.undoText(); }
    QString redoText() const { return stack_.redoText(); }
    bool    inTransaction() const { return transactionDepth_ > 0; }
    bool    isRestoring() const { return restoring_; }
    void    undo();
    void    redo();

private:
    class SnapshotCommand;
    class ScalarCommand;

    static bool statesEqual(const DocumentState &left, const DocumentState &right);
    void        pushSnapshot(const QString &kind, const DocumentState &before, const QVariantMap &beforeView,
                             const DocumentState &after, const QVariantMap &afterView);
    void pushScalar(const QString &kind, const ScalarAddress &address, const QString &before,
                    const QVariantMap &beforeView, const QString &after, const QVariantMap &afterView, bool mergeable);
    void apply(const DocumentState &state, const QVariantMap &viewState);
    void applyScalar(const ScalarAddress &address, const QString &value, const QVariantMap &viewState);
    void setCurrentScalar(const ScalarAddress &address, const QString &value);
    void changed();

    QUndoStack           stack_;
    DocumentState        current_;
    QVariantMap          currentView_;
    DocumentState        transactionBefore_;
    QVariantMap          transactionBeforeView_;
    QString              transactionKind_;
    int                  transactionDepth_ = 0;
    int                  mergeGeneration_  = 0;
    bool                 restoring_        = false;
    QElapsedTimer        elapsed_;
    RestoreHandler       restoreHandler_;
    ScalarRestoreHandler scalarRestoreHandler_;
    ChangedHandler       changedHandler_;
};

} // namespace AnyKeep

#endif
