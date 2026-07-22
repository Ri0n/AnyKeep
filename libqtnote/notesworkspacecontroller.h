#ifndef NOTESWORKSPACECONTROLLER_H
#define NOTESWORKSPACECONTROLLER_H

#include "note.h"
#include "qtnote_export.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QUuid>
#include <QVariantList>
#include <QVariantMap>

namespace QtNote {

class NoteEditor;
class NoteLoadJob;
class NotesModel;
class NotesSearchModel;
class RecentNotesModel;

class QTNOTE_EXPORT NotesWorkspaceController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *notesModel READ notesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *groupedNotesModel READ groupedNotesModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *recentNotesModel READ recentNotesModel CONSTANT)
    Q_PROPERTY(QObject *currentEditor READ currentEditor NOTIFY currentEditorChanged)
    Q_PROPERTY(QString currentStorageId READ currentStorageId NOTIFY currentEditorChanged)
    Q_PROPERTY(QString currentNoteId READ currentNoteId NOTIFY currentEditorChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY currentTitleChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorStringChanged)
    Q_PROPERTY(int noteCount READ noteCount NOTIFY noteCountChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(bool searchInBody READ searchInBody WRITE setSearchInBody NOTIFY searchInBodyChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    Q_PROPERTY(QVariantList storages READ storages NOTIFY storagesChanged)

public:
    explicit NotesWorkspaceController(QObject *parent = nullptr);
    ~NotesWorkspaceController() override;

    QAbstractItemModel *notesModel() const;
    QAbstractItemModel *groupedNotesModel() const;
    QAbstractItemModel *recentNotesModel() const;
    NotesModel         *sourceModel() const { return notesModel_; }
    NotesSearchModel   *searchModel() const { return searchModel_; }
    QObject            *currentEditor() const;
    NoteEditor         *editor() const;
    QString             currentStorageId() const;
    QString             currentNoteId() const;
    QString             currentTitle() const;
    bool                loading() const { return loading_; }
    bool                busy() const { return loading_ || pendingOperations_ > 0; }
    QString             errorString() const { return errorString_; }
    int                 noteCount() const;
    QString             searchText() const;
    bool                searchInBody() const;
    bool                searching() const;
    QVariantList        storages() const;

    Q_INVOKABLE bool openNote(const QString &storageId, const QString &noteId);
    bool             openNote(const Note &note, const QUuid &draftId = {});
    Q_INVOKABLE bool createNote(const QString &storageId = {});
    Q_INVOKABLE bool saveCurrentNote();
    Q_INVOKABLE bool closeCurrentNote();
    Q_INVOKABLE bool reloadCurrentNote();
    Q_INVOKABLE bool deleteNote(const QString &storageId, const QString &noteId);
    Q_INVOKABLE bool moveNote(const QString &sourceStorageId, const QString &noteId,
                              const QString &destinationStorageId);
    Q_INVOKABLE bool moveCurrentNote(const QString &destinationStorageId);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE bool openStandalone(const QString &storageId, const QString &noteId);
    Q_INVOKABLE bool openCurrentStandalone();

public slots:
    void setSearchText(const QString &text);
    void setSearchInBody(bool enabled);

signals:
    void currentEditorChanged();
    void currentTitleChanged();
    void loadingChanged();
    void busyChanged();
    void errorStringChanged();
    void noteCountChanged();
    void searchTextChanged();
    void searchInBodyChanged();
    void searchingChanged();
    void storagesChanged();
    void openStandaloneRequested(const QString &storageId, const QString &noteId);

private:
    struct PendingMove {
        QString sourceStorageId;
        QString sourceNoteId;
    };

    void setCurrentEditor(NoteEditor *editor);
    void clearCurrentEditor();
    void setLoading(bool loading);
    void setError(const QString &error);
    void beginOperation();
    void endOperation();
    bool stageMove(const Note &source, const QString &destinationStorageId, QUuid *draftId);
    void startStagedMove(const QUuid &draftId, const Note &source);
    bool beginMove(const Note &source, const QString &destinationStorageId);
    void connectEditorSignals(NoteEditor *editor);

    NotesModel               *notesModel_ { nullptr };
    NotesSearchModel         *searchModel_ { nullptr };
    RecentNotesModel         *recentNotesModel_ { nullptr };
    QPointer<NoteEditor>      currentEditor_;
    QPointer<NoteLoadJob>     loadJob_;
    QHash<QUuid, PendingMove> pendingMoves_;
    bool                      loading_ { false };
    int                       pendingOperations_ { 0 };
    QString                   errorString_;
};

} // namespace QtNote

#endif // NOTESWORKSPACECONTROLLER_H
