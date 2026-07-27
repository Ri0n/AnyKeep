#include <QtTest>

#include "notedata.h"
#include "notesindex.h"

#include <QPointer>

#include <algorithm>
#include <utility>

using namespace QtNote;

class FakeStorage final : public NoteStorage {
    Q_OBJECT
public:
    explicit FakeStorage(QObject *parent = nullptr) : NoteStorage(parent) { }

    bool                init() override { return true; }
    const QString       systemName() const override { return QStringLiteral("fake"); }
    const QString       name() const override { return QStringLiteral("Fake"); }
    QIcon               storageIcon() const override { return {}; }
    QIcon               noteIcon() const override { return {}; }
    bool                isAccessible() const override { return accessible; }
    QList<Note::Format> availableFormats() const override { return { Note::Markdown }; }

    QList<Note> noteList(int limit = 0) override
    {
        ++noteListCalls;
        return limit > 0 ? sourceNotes.mid(0, limit) : sourceNotes;
    }

    NoteListJob *refreshNotesAsync(int limit = 0, QObject *owner = nullptr) override
    {
        ++refreshCalls;
        if (holdRefresh) {
            auto *job = new NoteListJob(owner ? owner : this);
            job->start();
            heldRefreshJob    = job;
            heldRefreshResult = limit > 0 ? sourceNotes.mid(0, limit) : sourceNotes;
            return job;
        }
        if (!failRefreshSynchronously)
            return NoteStorage::refreshNotesAsync(limit, owner);

        auto *job = new NoteListJob(owner ? owner : this);
        job->start();
        StorageError error;
        error.code    = StorageError::Unavailable;
        error.message = QStringLiteral("Synchronous failure");
        job->fail(error);
        return job;
    }

    Note note(const QString &id) override
    {
        for (const auto &item : std::as_const(sourceNotes)) {
            if (item.id() == id)
                return item;
        }
        return {};
    }

    Note createNote() override { return makeNote(QString(), QString()); }

    bool saveNote(const Note &note) override
    {
        emit noteModified(note);
        return true;
    }

    void removeNote(const QString &id) override
    {
        const auto removed = note(id);
        if (!removed.isNull())
            emit noteRemoved(removed);
    }

    Note makeNote(const QString &id, const QString &title)
    {
        Note result(new NoteData(this));
        result.setId(id);
        result.setTitle(title);
        result.setText(QString(), Note::Markdown);
        result.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        return result;
    }

    void announceAdded(const Note &note) { emit noteAdded(note); }
    void announceModified(const Note &note) { emit noteModified(note); }
    void announceRemoved(const Note &note) { emit noteRemoved(note); }
    void announceIdChanged(const Note &note, const QString &oldId) { emit noteIdChanged(note, oldId); }
    void announceInvalidated() { emit invalidated(); }
    void completeHeldRefresh()
    {
        QVERIFY(heldRefreshJob);
        auto *job = heldRefreshJob.data();
        heldRefreshJob.clear();
        job->complete(heldRefreshResult);
    }

    QList<Note>           sourceNotes;
    QList<Note>           heldRefreshResult;
    QPointer<NoteListJob> heldRefreshJob;
    int                   noteListCalls { 0 };
    int                   refreshCalls { 0 };
    bool                  accessible { true };
    bool                  failRefreshSynchronously { false };
    bool                  holdRefresh { false };
};

class NotesIndexTest : public QObject {
    Q_OBJECT

private slots:
    void waitsForStorageReadyBeforeInitialRefresh()
    {
        FakeStorage storage;
        storage.sourceNotes = { storage.makeNote(QStringLiteral("one"), QStringLiteral("One")) };

        NotesIndex index;
        index.addStorage(&storage);
        QCoreApplication::processEvents();
        QCOMPARE(storage.noteListCalls, 0);
        QVERIFY(!index.hasSnapshot(storage.systemName()));

        QSignalSpy changedSpy(&index, &NotesIndex::storageNotesChanged);
        index.markStorageReady(&storage);

        QTRY_COMPARE(storage.noteListCalls, 1);
        QTRY_COMPARE(index.noteCount(storage.systemName()), 1);
        QVERIFY(index.hasSnapshot(storage.systemName()));
        const auto snapshot = index.notes(storage.systemName());
        QCOMPARE(snapshot.size(), 1);
        QVERIFY(!snapshot.first().isLoaded());
        QCOMPARE(changedSpy.count(), 1);
    }

    void updatesSnapshotFromStorageSignals()
    {
        FakeStorage storage;
        auto        first   = storage.makeNote(QStringLiteral("one"), QStringLiteral("One"));
        storage.sourceNotes = { first };

        NotesIndex index;
        index.addStorage(&storage);
        index.markStorageReady(&storage);
        QTRY_COMPARE(index.noteCount(storage.systemName()), 1);

        auto second = storage.makeNote(QStringLiteral("two"), QStringLiteral("Two"));
        second.setText(QStringLiteral("Body preview"), Note::Markdown);
        storage.announceAdded(second);
        QCOMPARE(index.noteCount(storage.systemName()), 2);
        const auto afterAdd = index.notes(storage.systemName());
        const auto added    = std::find_if(afterAdd.cbegin(), afterAdd.cend(),
                                           [](const Note &note) { return note.id() == QLatin1String("two"); });
        QVERIFY(added != afterAdd.cend());
        QVERIFY(!added->isLoaded());
        QCOMPARE(added->backendValue(QStringLiteral("qtnote.index.preview")).toString(),
                 QStringLiteral("Body preview"));

        first.setTitle(QStringLiteral("One updated"));
        storage.announceModified(first);
        const auto afterModify = index.notes(storage.systemName());
        const auto modified    = std::find_if(afterModify.cbegin(), afterModify.cend(),
                                              [](const Note &note) { return note.id() == QLatin1String("one"); });
        QVERIFY(modified != afterModify.cend());
        QCOMPARE(modified->title(), QStringLiteral("One updated"));

        auto renamed = first;
        renamed.setId(QStringLiteral("renamed"));
        storage.announceIdChanged(renamed, QStringLiteral("one"));
        const auto afterRename = index.notes(storage.systemName());
        QCOMPARE(afterRename.size(), 2);
        QVERIFY(std::none_of(afterRename.cbegin(), afterRename.cend(),
                             [](const Note &note) { return note.id() == QLatin1String("one"); }));
        QVERIFY(std::any_of(afterRename.cbegin(), afterRename.cend(),
                            [](const Note &note) { return note.id() == QLatin1String("renamed"); }));

        storage.announceRemoved(second);
        QCOMPARE(index.noteCount(storage.systemName()), 1);
    }

    void explicitRefreshReplacesSnapshot()
    {
        FakeStorage storage;
        storage.sourceNotes = { storage.makeNote(QStringLiteral("one"), QStringLiteral("One")) };

        NotesIndex index;
        index.addStorage(&storage);
        index.markStorageReady(&storage);
        QTRY_COMPARE(index.noteCount(storage.systemName()), 1);

        storage.sourceNotes = { storage.makeNote(QStringLiteral("two"), QStringLiteral("Two")),
                                storage.makeNote(QStringLiteral("three"), QStringLiteral("Three")) };
        index.refreshStorage(storage.systemName());

        QTRY_COMPARE(index.noteCount(storage.systemName()), 2);
        QCOMPARE(storage.noteListCalls, 2);
    }

    void refreshesAfterAReconfiguredStorageBecomesAccessible()
    {
        FakeStorage storage;
        storage.sourceNotes = { storage.makeNote(QStringLiteral("one"), QStringLiteral("One")) };

        storage.accessible = false;
        NotesIndex index;
        index.addStorage(&storage);
        StorageError error;
        error.code    = StorageError::NotConfigured;
        error.message = QStringLiteral("Not configured");
        index.markStorageInitializationFailed(&storage, error);
        QCOMPARE(storage.noteListCalls, 0);

        storage.accessible = true;
        storage.announceInvalidated();

        QTRY_COMPARE(storage.noteListCalls, 1);
        QTRY_COMPARE(index.noteCount(storage.systemName()), 1);
        QVERIFY(index.hasSnapshot(storage.systemName()));
        QCOMPARE(index.errorString(storage.systemName()), QString());
    }

    void loadsReadableOfflineCacheAfterInitializationFailure()
    {
        FakeStorage storage;
        storage.sourceNotes = { storage.makeNote(QStringLiteral("cached"), QStringLiteral("Cached")) };
        storage.accessible  = true;

        NotesIndex index;
        index.addStorage(&storage);
        StorageError error;
        error.code      = StorageError::Network;
        error.message   = QStringLiteral("Offline");
        error.retryable = true;
        index.markStorageInitializationFailed(&storage, error);

        QTRY_COMPARE(storage.noteListCalls, 1);
        QTRY_COMPARE(index.noteCount(storage.systemName()), 1);
        QVERIFY(index.hasSnapshot(storage.systemName()));
    }

    void loadsReadableOfflineCacheWhileInitializationIsPending()
    {
        FakeStorage storage;
        storage.sourceNotes = { storage.makeNote(QStringLiteral("cached"), QStringLiteral("Cached")) };
        storage.accessible  = true;

        NotesIndex index;
        index.addStorage(&storage);

        // XmppStorage emits invalidated as soon as its persistent cache has
        // been opened, while the network probe can remain pending offline.
        storage.announceInvalidated();

        QTRY_COMPARE(storage.noteListCalls, 1);
        QTRY_COMPARE(index.noteCount(storage.systemName()), 1);
        QVERIFY(index.hasSnapshot(storage.systemName()));
    }

    void initializationFailureDoesNotCancelPendingOfflineCacheRead()
    {
        FakeStorage storage;
        storage.sourceNotes = { storage.makeNote(QStringLiteral("cached"), QStringLiteral("Cached")) };
        storage.accessible  = true;
        storage.holdRefresh = true;

        NotesIndex index;
        index.addStorage(&storage);
        storage.announceInvalidated();

        QCOMPARE(storage.refreshCalls, 1);
        QVERIFY(index.isLoading(storage.systemName()));

        StorageError error;
        error.code      = StorageError::Network;
        error.message   = QStringLiteral("Offline");
        error.retryable = true;
        index.markStorageInitializationFailed(&storage, error);

        QCOMPARE(storage.refreshCalls, 1);
        QVERIFY(index.isLoading(storage.systemName()));

        storage.holdRefresh = false;
        storage.completeHeldRefresh();

        QTRY_COMPARE(index.noteCount(storage.systemName()), 1);
        QVERIFY(index.hasSnapshot(storage.systemName()));
    }

    void invalidationDuringRefreshIsNotLost()
    {
        FakeStorage storage;
        storage.sourceNotes = { storage.makeNote(QStringLiteral("one"), QStringLiteral("One")) };
        storage.holdRefresh = true;

        NotesIndex index;
        index.addStorage(&storage);
        index.markStorageReady(&storage);
        QCOMPARE(storage.refreshCalls, 1);
        QVERIFY(index.isLoading(storage.systemName()));

        storage.sourceNotes = { storage.makeNote(QStringLiteral("one"), QStringLiteral("One")),
                                storage.makeNote(QStringLiteral("two"), QStringLiteral("Two")) };
        storage.announceInvalidated();
        storage.holdRefresh = false;
        storage.completeHeldRefresh();

        QTRY_COMPARE(storage.refreshCalls, 2);
        QTRY_COMPARE(index.noteCount(storage.systemName()), 2);
        QVERIFY(index.hasSnapshot(storage.systemName()));
    }

    void handlesSynchronouslyFailedRefreshJobs()
    {
        FakeStorage storage;
        storage.failRefreshSynchronously = true;

        NotesIndex index;
        index.addStorage(&storage);
        index.markStorageReady(&storage);

        QTRY_VERIFY(!index.isLoading(storage.systemName()));
        QCOMPARE(index.errorString(storage.systemName()), QStringLiteral("Synchronous failure"));
        QVERIFY(!index.hasSnapshot(storage.systemName()));
    }
};

QTEST_GUILESS_MAIN(NotesIndexTest)

#include "notesindex_test.moc"
