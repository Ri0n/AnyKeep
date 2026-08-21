#include <QtTest>

#include "notedata.h"
#include "notemanager.h"
#include "notesindex.h"
#include "notesmodel.h"
#include "recentnotesmodel.h"

#include <QPointer>
#include <QScopeGuard>

#include <algorithm>
#include <utility>

using namespace AnyKeep;

class FakeStorage final : public NoteStorage {
    Q_OBJECT
public:
    explicit FakeStorage(QString storageId = QStringLiteral("fake"), QObject *parent = nullptr) :
        NoteStorage(parent), storageId_(std::move(storageId))
    {
    }

    bool                init() override { return true; }
    const QString       systemName() const override { return storageId_; }
    const QString       name() const override { return QStringLiteral("Fake"); }
    QIcon               storageIcon() const override { return {}; }
    QIcon               noteIcon() const override { return {}; }
    bool                isAccessible() const override { return accessible; }
    QList<Note::Format> availableFormats() const override { return { Note::Markdown }; }
    qint64              requestedModificationTimeResolutionMs() const override { return reorderResolutionMs; }

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
        Note saved = note;
        saved.removeBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey));
        for (auto &existing : sourceNotes) {
            if (existing.id() == saved.id()) {
                existing = saved;
                break;
            }
        }
        std::stable_sort(sourceNotes.begin(), sourceNotes.end(), noteListItemModifyComparer);
        ++saveCalls;
        emit noteModified(saved);
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
    int                   saveCalls { 0 };
    qint64                reorderResolutionMs { 0 };
    bool                  accessible { true };
    bool                  failRefreshSynchronously { false };
    bool                  holdRefresh { false };

private:
    QString storageId_;
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
        QCOMPARE(added->backendValue(QStringLiteral("anykeep.index.preview")).toString(),
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

    void unloadedSummaryKeepsResolvedCodeTitle()
    {
        FakeStorage storage;
        Note        code = storage.makeNote(QStringLiteral("code"), QString());
        code.setText(QStringLiteral("```qml\nItem {\n}\n```"), Note::Markdown);
        storage.sourceNotes = { code };

        NotesIndex index;
        index.addStorage(&storage);
        index.markStorageReady(&storage);
        QTRY_COMPARE(index.noteCount(storage.systemName()), 1);
        const Note summary = index.notes(storage.systemName()).constFirst();
        QVERIFY(!summary.isLoaded());
        QCOMPARE(summary.title(), QString());
        QCOMPARE(summary.displayTitle(), QStringLiteral("QML code"));
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

    void defaultStorageReorderFallbackUsesTheCapability()
    {
        FakeStorage storage;
        auto       *unsupported = storage.reorderNoteAsync(QStringLiteral("one"), {});
        QVERIFY(unsupported->isFinished());
        QCOMPARE(unsupported->state(), StorageJob::Failed);
        delete unsupported;

        storage.reorderResolutionMs = 1;
        const auto baseTime         = QDateTime::currentDateTimeUtc().addSecs(-10);
        auto       first            = storage.makeNote(QStringLiteral("first"), QStringLiteral("First"));
        auto       second           = storage.makeNote(QStringLiteral("second"), QStringLiteral("Second"));
        auto       third            = storage.makeNote(QStringLiteral("third"), QStringLiteral("Third"));
        first.setLastChangeUTC(baseTime);
        second.setLastChangeUTC(baseTime.addSecs(-1));
        third.setLastChangeUTC(baseTime.addSecs(-2));
        storage.sourceNotes = { first, second, third };

        auto *job = storage.reorderNoteAsync(third.id(), {});
        QTRY_VERIFY(job->isFinished());
        QCOMPARE(job->state(), StorageJob::Succeeded);
        QCOMPARE(storage.saveCalls, 1);
        QCOMPARE(storage.sourceNotes.at(0).id(), third.id());
        QCOMPARE(storage.sourceNotes.at(1).id(), first.id());
        QCOMPARE(storage.sourceNotes.at(2).id(), second.id());
        delete job;
    }

    void favoriteNotesLeadGroupedAndRecentProjections()
    {
        auto       storage  = std::make_unique<FakeStorage>(QStringLiteral("fake-favorite-order"));
        auto      *raw      = storage.get();
        const auto baseTime = QDateTime::currentDateTimeUtc();
        auto       recent   = raw->makeNote(QStringLiteral("recent"), QStringLiteral("Recent"));
        recent.setLastChangeUTC(baseTime);
        auto favorite = raw->makeNote(QStringLiteral("favorite"), QStringLiteral("Favorite"));
        favorite.setFavorite(true);
        favorite.setLastChangeUTC(baseTime.addSecs(-3600));
        raw->sourceNotes = { recent, favorite };

        auto *manager = NoteManager::instance();
        manager->registerStorage(std::move(storage));
        const auto unregister = qScopeGuard([manager, raw]() {
            if (manager->storage(raw->systemName()) == raw)
                manager->unregisterStorage(raw);
        });
        QTRY_COMPARE(manager->notesIndex()->noteCount(raw->systemName()), 2);

        NotesModel  grouped;
        QModelIndex storageIndex;
        for (int row = 0; row < grouped.rowCount(); ++row) {
            const auto candidate = grouped.index(row, 0);
            if (candidate.data(NotesModel::StorageIdRole).toString() == raw->systemName()) {
                storageIndex = candidate;
                break;
            }
        }
        QVERIFY(storageIndex.isValid());
        QTRY_COMPARE(grouped.rowCount(storageIndex), 2);
        QCOMPARE(grouped.index(0, 0, storageIndex).data(NotesModel::NoteIdRole).toString(), QStringLiteral("favorite"));
        QVERIFY(grouped.index(0, 0, storageIndex).data(NotesModel::FavoriteRole).toBool());

        RecentNotesModel recentModel(&grouped);
        recentModel.setMaximumCount(10);
        QTRY_COMPARE(recentModel.rowCount(), 2);
        QCOMPARE(recentModel.index(0, 0).data(NotesModel::NoteIdRole).toString(), QStringLiteral("favorite"));
        QVERIFY(recentModel.index(0, 0).data(NotesModel::FavoriteRole).toBool());
    }

    void visiblePageKeepsItsTailWhenAnInsertedNoteIncreasesTheCount()
    {
        auto       storage  = std::make_unique<FakeStorage>(QStringLiteral("fake-visible-page"));
        auto      *raw      = storage.get();
        const auto baseTime = QDateTime::currentDateTimeUtc().addSecs(-60);
        for (int index = 0; index < 35; ++index) {
            auto note = raw->makeNote(QStringLiteral("note-%1").arg(index), QStringLiteral("Note %1").arg(index));
            note.setLastChangeUTC(baseTime.addSecs(-index));
            raw->sourceNotes.append(note);
        }

        auto *manager = NoteManager::instance();
        manager->registerStorage(std::move(storage));
        const auto unregister = qScopeGuard([manager, raw]() {
            if (manager->storage(raw->systemName()) == raw)
                manager->unregisterStorage(raw);
        });
        QTRY_COMPARE(manager->notesIndex()->noteCount(raw->systemName()), 35);

        NotesModel  model;
        QModelIndex storageIndex;
        for (int row = 0; row < model.rowCount(); ++row) {
            const auto candidate = model.index(row, 0);
            if (candidate.data(NotesModel::StorageIdRole).toString() == raw->systemName()) {
                storageIndex = candidate;
                break;
            }
        }
        QVERIFY(storageIndex.isValid());
        QCOMPARE(model.rowCount(storageIndex), model.pageSize());
        QCOMPARE(model.index(29, 0, storageIndex).data(NotesModel::NoteIdRole).toString(), QStringLiteral("note-29"));
        QVERIFY(!model.fetchMoreNear(raw->systemName(), QStringLiteral("note-0")));
        QCOMPARE(model.rowCount(storageIndex), model.pageSize());

        auto inserted = raw->makeNote(QStringLiteral("inserted"), QStringLiteral("Inserted"));
        inserted.setLastChangeUTC(baseTime.addSecs(-28).addMSecs(-500));
        raw->sourceNotes.append(inserted);
        std::stable_sort(raw->sourceNotes.begin(), raw->sourceNotes.end(), noteListItemModifyComparer);
        raw->announceAdded(inserted);

        QTRY_COMPARE(manager->notesIndex()->noteCount(raw->systemName()), 36);
        QTRY_COMPARE(model.rowCount(storageIndex), model.pageSize() + 1);
        QCOMPARE(model.index(29, 0, storageIndex).data(NotesModel::NoteIdRole).toString(), QStringLiteral("inserted"));
        QCOMPARE(model.index(30, 0, storageIndex).data(NotesModel::NoteIdRole).toString(), QStringLiteral("note-29"));
        QVERIFY(model.fetchMoreNear(raw->systemName(), QStringLiteral("note-29")));
        QCOMPARE(model.rowCount(storageIndex), 36);
        QVERIFY(!model.fetchMoreNear(raw->systemName(), QStringLiteral("note-34")));
    }
};

QTEST_GUILESS_MAIN(NotesIndexTest)

#include "notesindex_test.moc"
