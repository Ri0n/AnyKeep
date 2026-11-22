#include <QtTest>

#include "notedata.h"
#include "notemanager.h"
#include "notesindex.h"
#include "notesmodel.h"
#include "notessearchmodel.h"
#include "recentnotesmodel.h"

#include <QAbstractItemModelTester>
#include <QPointer>
#include <QScopeGuard>
#include <QSettings>

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

    void storagePriorityChangeKeepsTheGroupedRows()
    {
        auto      *manager            = NoteManager::instance();
        const auto originalPriorities = QSettings().value(QStringLiteral("storage.priority")).toStringList();
        auto       first              = std::make_unique<FakeStorage>(QStringLiteral("fake-priority-first"));
        auto       second             = std::make_unique<FakeStorage>(QStringLiteral("fake-priority-second"));
        first->sourceNotes            = { first->makeNote(QStringLiteral("first-note"), QStringLiteral("First")) };
        second->sourceNotes           = { second->makeNote(QStringLiteral("second-note"), QStringLiteral("Second")) };
        auto         *firstStorage    = first.get();
        auto         *secondStorage   = second.get();
        const QString firstId         = firstStorage->systemName();
        const QString secondId        = secondStorage->systemName();
        manager->registerStorage(std::move(first));
        manager->registerStorage(std::move(second));
        const auto cleanup = qScopeGuard([&] {
            manager->unregisterStorage(firstStorage);
            manager->unregisterStorage(secondStorage);
            manager->setPriorities(originalPriorities);
        });
        manager->setPriorities({ firstId, secondId });
        QTRY_COMPARE(manager->notesIndex()->noteCount(firstId), 1);
        QTRY_COMPARE(manager->notesIndex()->noteCount(secondId), 1);

        NotesModel       model;
        NotesSearchModel grouped;
        grouped.setSourceModel(&model);
        const QPersistentModelIndex firstGroup(grouped.index(0, 0));
        const QPersistentModelIndex secondGroup(grouped.index(1, 0));
        QCOMPARE(firstGroup.data(NotesModel::StorageIdRole).toString(), firstId);
        QCOMPARE(secondGroup.data(NotesModel::StorageIdRole).toString(), secondId);
        const QPersistentModelIndex firstNote(grouped.index(0, 0, firstGroup));
        QSignalSpy                  reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy                  moved(&model, &QAbstractItemModel::rowsMoved);

        manager->setPriorities({ secondId, firstId });

        QCOMPARE(reset.size(), 0);
        QCOMPARE(moved.size(), 1);
        QCOMPARE(firstGroup.row(), 1);
        QCOMPARE(secondGroup.row(), 0);
        QCOMPARE(firstNote.parent(), QModelIndex(firstGroup));
        QCOMPARE(firstNote.data(NotesModel::NoteIdRole).toString(), QStringLiteral("first-note"));
    }

    void noteChangesKeepGroupedAndRecentIdentities()
    {
        auto  storage = std::make_unique<FakeStorage>(QStringLiteral("incremental-notes"));
        auto *raw     = storage.get();
        auto  first   = raw->makeNote(QStringLiteral("first"), QStringLiteral("First"));
        auto  second  = raw->makeNote(QStringLiteral("second"), QStringLiteral("Second"));
        auto  third   = raw->makeNote(QStringLiteral("third"), QStringLiteral("Third"));
        second.setLastChangeUTC(first.lastChangeUTC().addSecs(-1));
        third.setLastChangeUTC(first.lastChangeUTC().addSecs(-2));
        raw->sourceNotes = { first, second, third };

        auto *manager = NoteManager::instance();
        manager->registerStorage(std::move(storage));
        const auto unregister = qScopeGuard([manager, raw] { manager->unregisterStorage(raw); });
        QTRY_COMPARE(manager->notesIndex()->noteCount(raw->systemName()), 3);
        NotesModel       grouped;
        NotesSearchModel filter;
        filter.setSourceModel(&grouped);
        RecentNotesModel         recent(&filter);
        QAbstractItemModelTester groupedTester(&grouped, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QAbstractItemModelTester filterTester(&filter, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QAbstractItemModelTester recentTester(&recent, QAbstractItemModelTester::FailureReportingMode::QtTest);
        const auto               parent = grouped.index(0, 0);
        QCOMPARE(parent.data(NotesModel::StorageIdRole).toString(), raw->systemName());
        QCOMPARE(grouped.rowCount(parent), 3);
        QCOMPARE(recent.rowCount(), 3);

        const QPersistentModelIndex groupedFirst(grouped.index(0, 0, parent));
        const QPersistentModelIndex groupedThird(grouped.index(2, 0, parent));
        const QPersistentModelIndex recentFirst(recent.index(0, 0));
        const QPersistentModelIndex recentThird(recent.index(2, 0));
        const auto                 *thirdItem = groupedThird.internalPointer();
        QSignalSpy                  groupedChanges(&grouped, &QAbstractItemModel::dataChanged);
        QSignalSpy                  groupedMoves(&grouped, &QAbstractItemModel::rowsMoved);
        QSignalSpy                  groupedInserted(&grouped, &QAbstractItemModel::rowsInserted);
        QSignalSpy                  groupedRemoved(&grouped, &QAbstractItemModel::rowsRemoved);
        QSignalSpy                  groupedReset(&grouped, &QAbstractItemModel::modelReset);
        QSignalSpy                  recentChanges(&recent, &QAbstractItemModel::dataChanged);
        QSignalSpy                  recentMoves(&recent, &QAbstractItemModel::rowsMoved);
        QSignalSpy                  recentInserted(&recent, &QAbstractItemModel::rowsInserted);
        QSignalSpy                  recentRemoved(&recent, &QAbstractItemModel::rowsRemoved);
        QSignalSpy                  recentReset(&recent, &QAbstractItemModel::modelReset);
        QSignalSpy                  recentCount(&recent, &RecentNotesModel::noteCountChanged);

        first.setText(QStringLiteral("Changed preview"), Note::Markdown);
        raw->announceModified(first);
        QCOMPARE(groupedChanges.size(), 1);
        QCOMPARE(recentChanges.size(), 1);
        QCOMPARE(groupedChanges.first().at(0).value<QModelIndex>(), QModelIndex(groupedFirst));
        QCOMPARE(groupedChanges.first().at(2).value<QList<int>>(), QList<int> { NotesModel::PreviewRole });
        QCOMPARE(recentChanges.first().at(0).value<QModelIndex>(), QModelIndex(recentFirst));
        QCOMPARE(recentChanges.first().at(2).value<QList<int>>(), QList<int> { NotesModel::PreviewRole });
        QCOMPARE(recentFirst.data(NotesModel::PreviewRole).toString(), QStringLiteral("Changed preview"));

        raw->announceModified(first);
        recent.rebuild();
        QCOMPARE(groupedChanges.size(), 1);
        QCOMPARE(recentChanges.size(), 1);
        QCOMPARE(groupedMoves.size(), 0);
        QCOMPARE(recentMoves.size(), 0);

        first.setLastChangeUTC(third.lastChangeUTC().addSecs(-1));
        raw->announceModified(first);
        QCOMPARE(groupedMoves.size(), 1);
        QCOMPARE(recentMoves.size(), 1);
        QCOMPARE(groupedMoves.first().at(0).value<QModelIndex>(), parent);
        QCOMPARE(groupedMoves.first().at(1).toInt(), 0);
        QCOMPARE(groupedMoves.first().at(4).toInt(), 3);
        QCOMPARE(groupedFirst.row(), 2);
        QCOMPARE(recentFirst.row(), 2);
        QCOMPARE(groupedThird.row(), 1);
        QCOMPARE(recentThird.row(), 1);
        QCOMPARE(groupedThird.internalPointer(), thirdItem);
        QCOMPARE(groupedInserted.size(), 0);
        QCOMPARE(groupedRemoved.size(), 0);
        QCOMPARE(recentInserted.size(), 0);
        QCOMPARE(recentRemoved.size(), 0);
        QCOMPARE(recentCount.size(), 0);

        raw->announceRemoved(second);
        QCOMPARE(groupedRemoved.size(), 1);
        QCOMPARE(recentRemoved.size(), 1);
        QCOMPARE(groupedThird.row(), 0);
        QCOMPARE(recentThird.row(), 0);
        QCOMPARE(recentCount.size(), 1);
        raw->announceAdded(second);
        QCOMPARE(groupedInserted.size(), 1);
        QCOMPARE(recentInserted.size(), 1);
        QCOMPARE(groupedThird.row(), 1);
        QCOMPARE(recentThird.row(), 1);
        QCOMPARE(recentCount.size(), 2);

        recent.setMaximumCount(2);
        QCOMPARE(recent.rowCount(), 2);
        QVERIFY(!recentFirst.isValid());
        QVERIFY(recentThird.isValid());
        recentCount.clear();
        recentInserted.clear();
        recentRemoved.clear();
        first.setLastChangeUTC(second.lastChangeUTC().addSecs(1));
        raw->announceModified(first);
        QCOMPARE(recent.rowCount(), 2);
        QCOMPARE(recent.index(0, 0).data(NotesModel::NoteIdRole).toString(), first.id());
        QCOMPARE(recentInserted.size(), 1);
        QCOMPARE(recentRemoved.size(), 1);
        QCOMPARE(recentCount.size(), 0);
        QVERIFY(!recentThird.isValid());
        QCOMPARE(groupedReset.size(), 0);
        QCOMPARE(recentReset.size(), 0);
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
        const QPersistentModelIndex loadedTail(model.index(29, 0, storageIndex));
        NotesSearchModel            filter;
        filter.setSourceModel(&model);
        QSignalSpy insertedRows(&model, &QAbstractItemModel::rowsInserted);
        QSignalSpy removedRows(&model, &QAbstractItemModel::rowsRemoved);
        filter.setSearchText(QStringLiteral("Note 34"));
        QCOMPARE(model.rowCount(storageIndex), 35);
        QCOMPARE(insertedRows.size(), 1);
        QCOMPARE(insertedRows.first().at(1).toInt(), model.pageSize());
        QCOMPARE(removedRows.size(), 0);
        QCOMPARE(loadedTail.row(), 29);
        const auto matching = model.storageNoteDescriptors(raw->systemName(), &filter);
        QCOMPARE(matching.size(), 1);
        QCOMPARE(matching.first().toMap().value(QStringLiteral("noteId")).toString(), QStringLiteral("note-34"));
        filter.setSearchText({});
        QCOMPARE(model.rowCount(storageIndex), model.pageSize());
        QCOMPARE(removedRows.size(), 1);
        QCOMPARE(removedRows.first().at(1).toInt(), model.pageSize());
        QCOMPARE(loadedTail.row(), 29);
        insertedRows.clear();
        removedRows.clear();

        {
            QSignalSpy                  inserted(&model, &QAbstractItemModel::rowsInserted);
            QSignalSpy                  removed(&model, &QAbstractItemModel::rowsRemoved);
            const QPersistentModelIndex loadedNote(model.index(29, 0, storageIndex));
            const auto                  selection = model.storageNoteDescriptors(raw->systemName(), nullptr);
            QCOMPARE(selection.size(), 35);
            QCOMPARE(selection.last().toMap().value(QStringLiteral("noteId")).toString(), QStringLiteral("note-34"));
            QCOMPARE(model.rowCount(storageIndex), model.pageSize());
            QCOMPARE(inserted.size(), 0);
            QCOMPARE(removed.size(), 0);
            QCOMPARE(loadedNote.data(NotesModel::NoteIdRole).toString(), QStringLiteral("note-29"));
        }

        auto inserted = raw->makeNote(QStringLiteral("inserted"), QStringLiteral("Inserted"));
        inserted.setLastChangeUTC(baseTime.addSecs(-28).addMSecs(-500));
        raw->sourceNotes.append(inserted);
        std::stable_sort(raw->sourceNotes.begin(), raw->sourceNotes.end(), noteListItemModifyComparer);
        raw->announceAdded(inserted);

        QTRY_COMPARE(manager->notesIndex()->noteCount(raw->systemName()), 36);
        QTRY_COMPARE(model.rowCount(storageIndex), model.pageSize() + 1);
        QCOMPARE(model.index(29, 0, storageIndex).data(NotesModel::NoteIdRole).toString(), QStringLiteral("inserted"));
        QCOMPARE(loadedTail.row(), 30);
        QCOMPARE(loadedTail.data(NotesModel::NoteIdRole).toString(), QStringLiteral("note-29"));
        QCOMPARE(insertedRows.size(), 1);
        QCOMPARE(insertedRows.first().at(1).toInt(), 29);
        QCOMPARE(insertedRows.first().at(2).toInt(), 29);
        QCOMPARE(removedRows.size(), 0);
        QVERIFY(model.fetchMoreNear(raw->systemName(), QStringLiteral("note-29")));
        QCOMPARE(model.rowCount(storageIndex), 36);
        QCOMPARE(insertedRows.size(), 2);
        QCOMPARE(insertedRows.last().at(1).toInt(), 31);
        QCOMPARE(removedRows.size(), 0);
        QVERIFY(!model.fetchMoreNear(raw->systemName(), QStringLiteral("note-34")));
    }
};

QTEST_GUILESS_MAIN(NotesIndexTest)

#include "notesindex_test.moc"
