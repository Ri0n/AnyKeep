#include "notesindex.h"
#include "notetitleresolver.h"

#include <QSet>
#include <QSharedPointer>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace AnyKeep {

namespace {
    constexpr auto IndexPreviewKey = "anykeep.index.preview";

    Note indexSummary(const Note &note)
    {
        if (note.isNull())
            return {};

        Note summary = note;
        if (summary.isLoaded()) {
            summary.setBackendValue(QString::fromLatin1(NoteTitleResolver::CachedDisplayTitleBackendKey),
                                    summary.displayTitle());
            QString preview = summary.text().simplified();
            if (preview.size() > 180)
                preview = preview.left(177) + QStringLiteral("...");
            summary.setBackendValue(QString::fromLatin1(IndexPreviewKey), preview);
            summary.unload();
        }
        summary.setMedia({});
        return summary;
    }

    QList<Note> indexSummaries(const QList<Note> &notes)
    {
        QList<Note> summaries;
        summaries.reserve(notes.size());
        for (const auto &note : notes) {
            auto summary = indexSummary(note);
            if (!summary.isNull() && !summary.id().isEmpty())
                summaries.append(std::move(summary));
        }
        std::sort(summaries.begin(), summaries.end(), noteListItemModifyComparer);
        return summaries;
    }
}

NotesIndex::NotesIndex(QObject *parent) : QObject(parent) {}

void NotesIndex::addStorage(const NoteStorage::Ptr &storage)
{
    if (!storage)
        return;

    const QString storageId = storage->systemName();
    auto         &state     = states_[storageId];
    state.storage           = storage;

    connect(storage, &NoteStorage::noteAdded, this, [this](const Note &note) { upsertNote(note); });
    connect(storage, &NoteStorage::noteModified, this, [this](const Note &note) { upsertNote(note); });
    connect(storage, &NoteStorage::noteRemoved, this, [this](const Note &note) { removeNote(note); });
    connect(storage, &NoteStorage::noteIdChanged, this,
            [this](const Note &note, const QString &oldNoteId) { upsertNote(note, oldNoteId); });
    connect(storage, &NoteStorage::invalidated, this, [this, storageId]() {
        auto *state = stateForStorage(storageId);
        if (!state)
            return;

        // A remote storage may expose a persistent offline snapshot before
        // its network probe finishes. Do not make the UI wait for the probe:
        // if the storage already reports readable data, load that snapshot
        // while initialization continues in the background.
        if (!state->ready && state->storage && state->storage->isAccessible()) {
            state->ready = true;
            state->errorString.clear();
            emit storageStateChanged(storageId);
        }

        // For ordinary storages invalidation before init completion still has
        // no meaning. The exception above is deliberately limited to a
        // storage that already exposes readable cached data.
        if (!state->initializationFinished && !state->ready)
            return;
        if (!state->ready)
            return;
        if (state->loading) {
            state->refreshPending = true;
            return;
        }
        refreshStorage(storageId);
    });
    connect(storage, &NoteStorage::storageErorr, this, [this, storageId](const QString &message) {
        auto *state = stateForStorage(storageId);
        if (!state)
            return;
        state->errorString = message;
        emit storageStateChanged(storageId);
    });
}

void NotesIndex::removeStorage(NoteStorage *storage)
{
    if (!storage)
        return;

    const QString storageId = storage->systemName();
    auto          it        = states_.find(storageId);
    if (it == states_.end())
        return;
    if (it->refreshJob)
        it->refreshJob->cancel();
    disconnect(storage, nullptr, this, nullptr);
    states_.erase(it);
}

void NotesIndex::markStorageReady(const NoteStorage::Ptr &storage)
{
    if (!storage)
        return;
    auto *state = stateForStorage(storage->systemName());
    if (!state)
        return;
    state->initializationFinished = true;
    state->ready                  = true;
    state->errorString.clear();
    emit storageStateChanged(storage->systemName());
    startRefresh(*state);
}

void NotesIndex::markStorageInitializationFailed(const NoteStorage::Ptr &storage, const StorageError &error)
{
    if (!storage)
        return;
    auto *state = stateForStorage(storage->systemName());
    if (!state)
        return;
    state->initializationFinished = true;
    state->ready                  = storage->isAccessible();
    state->errorString            = error.message.isEmpty() ? tr("Failed to initialize storage") : error.message;
    emit storageStateChanged(storage->systemName());
    // If an offline snapshot was already loaded while the network probe was
    // pending, keep it. Starting another refresh here would only cancel or
    // duplicate the same cache read. A storage without a snapshot still gets
    // its first offline refresh after the initialization failure.
    if (state->ready && !state->snapshotValid && !state->loading)
        startRefresh(*state);
}

void NotesIndex::refreshStorage(const QString &storageId)
{
    auto *state = stateForStorage(storageId);
    if (state && state->ready)
        startRefresh(*state);
}

NoteListJob *NotesIndex::refreshAll(const std::list<NoteStorage::Ptr> &storages, int count, QObject *owner)
{
    auto *aggregate = new NoteListJob(owner ? owner : this);
    aggregate->start();

    QList<NoteListJob *> jobs;
    for (const auto &storage : storages) {
        if (!storage)
            continue;
        auto *state = stateForStorage(storage->systemName());
        if (!state || !state->ready)
            continue;
        if (auto *job = startRefresh(*state))
            jobs.append(job);
    }

    if (jobs.isEmpty()) {
        aggregate->complete({});
        return aggregate;
    }

    struct AggregateState {
        int                       pending { 0 };
        QList<Note>               notes;
        StorageError              firstError;
        QSet<const NoteListJob *> processedJobs;
    };
    auto aggregateState     = QSharedPointer<AggregateState>::create();
    aggregateState->pending = jobs.size();

    for (auto *job : std::as_const(jobs)) {
        const auto handleFinished = [aggregate, aggregateState, job, count]() {
            if (aggregateState->processedJobs.contains(job))
                return;
            aggregateState->processedJobs.insert(job);

            if (job->state() == StorageJob::Succeeded)
                aggregateState->notes += job->result();
            else if (!aggregateState->firstError && job->state() != StorageJob::Cancelled)
                aggregateState->firstError = job->error();

            if (--aggregateState->pending != 0 || aggregate->isFinished())
                return;

            std::sort(aggregateState->notes.begin(), aggregateState->notes.end(), noteListItemModifyComparer);
            if (count >= 0)
                aggregateState->notes = aggregateState->notes.mid(0, count);
            if (!aggregateState->notes.isEmpty() || !aggregateState->firstError)
                aggregate->complete(aggregateState->notes);
            else
                aggregate->fail(aggregateState->firstError);
        };
        connect(job, &StorageJob::finished, aggregate, handleFinished);
        if (job->isFinished())
            handleFinished();
    }
    return aggregate;
}

QList<Note> NotesIndex::notes(const QString &storageId) const
{
    const auto *state = stateForStorage(storageId);
    return state ? state->notes : QList<Note>();
}

QList<Note> NotesIndex::allNotes(const std::list<NoteStorage::Ptr> &storages, int count) const
{
    QList<Note> result;
    for (const auto &storage : storages) {
        if (!storage)
            continue;
        result += notes(storage->systemName());
    }
    std::sort(result.begin(), result.end(), noteListItemModifyComparer);
    return count >= 0 ? result.mid(0, count) : result;
}

int NotesIndex::noteCount(const QString &storageId) const
{
    const auto *state = stateForStorage(storageId);
    return state ? state->notes.size() : 0;
}

bool NotesIndex::isLoading(const QString &storageId) const
{
    const auto *state = stateForStorage(storageId);
    return state && state->loading;
}

bool NotesIndex::hasSnapshot(const QString &storageId) const
{
    const auto *state = stateForStorage(storageId);
    return state && state->snapshotValid;
}

QString NotesIndex::errorString(const QString &storageId) const
{
    const auto *state = stateForStorage(storageId);
    return state ? state->errorString : QString();
}

NoteListJob *NotesIndex::startRefresh(StorageState &state)
{
    if (!state.storage || !state.ready)
        return nullptr;

    state.refreshPending = false;
    ++state.refreshGeneration;
    const quint64 generation = state.refreshGeneration;
    if (state.refreshJob)
        state.refreshJob->cancel();

    state.loading = true;
    state.errorString.clear();
    const QString storageId = state.storage->systemName();
    emit          storageStateChanged(storageId);

    auto *job                 = state.storage->refreshNotesAsync(0, this);
    state.refreshJob          = job;
    const auto handleFinished = [this, storageId, generation, job]() {
        auto *current = stateForStorage(storageId);
        if (!current || current->refreshGeneration != generation || current->refreshJob != job) {
            job->deleteLater();
            return;
        }

        current->refreshJob.clear();
        current->loading = false;
        if (job->state() == StorageJob::Succeeded) {
            current->notes         = indexSummaries(job->result());
            current->snapshotValid = true;
            current->errorString.clear();
            emit storageNotesChanged(storageId);
        } else if (job->state() != StorageJob::Cancelled) {
            current->errorString = job->error().message.isEmpty() ? tr("Failed to load notes") : job->error().message;
        }
        const bool refreshAgain
            = current->refreshPending && current->ready && current->storage && current->storage->isAccessible();
        current->refreshPending = false;
        emit storageStateChanged(storageId);
        job->deleteLater();
        if (refreshAgain)
            QTimer::singleShot(0, this, [this, storageId]() { refreshStorage(storageId); });
    };
    connect(job, &StorageJob::finished, this, handleFinished);
    if (job->isFinished())
        QTimer::singleShot(0, this, handleFinished);
    return job;
}

void NotesIndex::upsertNote(const Note &note, const QString &oldNoteId)
{
    if (note.isNull() || note.id().isEmpty() || !note.storage())
        return;
    const QString storageId = note.storage()->systemName();
    auto         *state     = stateForStorage(storageId);
    if (!state)
        return;

    if (!oldNoteId.isEmpty() && oldNoteId != note.id()) {
        for (int i = state->notes.size() - 1; i >= 0; --i) {
            if (state->notes.at(i).id() == oldNoteId)
                state->notes.removeAt(i);
        }
    }

    const Note summary  = indexSummary(note);
    bool       replaced = false;
    for (int i = 0; i < state->notes.size(); ++i) {
        if (state->notes.at(i).id() == summary.id()) {
            state->notes[i] = summary;
            replaced        = true;
            break;
        }
    }
    if (!replaced)
        state->notes.append(summary);
    std::sort(state->notes.begin(), state->notes.end(), noteListItemModifyComparer);
    emit storageNotesChanged(storageId);
}

void NotesIndex::removeNote(const Note &note)
{
    if (note.isNull() || note.id().isEmpty() || !note.storage())
        return;
    const QString storageId = note.storage()->systemName();
    auto         *state     = stateForStorage(storageId);
    if (!state)
        return;

    for (int i = state->notes.size() - 1; i >= 0; --i) {
        if (state->notes.at(i).id() == note.id())
            state->notes.removeAt(i);
    }
    emit storageNotesChanged(storageId);
}

NotesIndex::StorageState *NotesIndex::stateForStorage(const QString &storageId)
{
    auto it = states_.find(storageId);
    return it == states_.end() ? nullptr : &it.value();
}

const NotesIndex::StorageState *NotesIndex::stateForStorage(const QString &storageId) const
{
    auto it = states_.constFind(storageId);
    return it == states_.constEnd() ? nullptr : &it.value();
}

} // namespace AnyKeep
