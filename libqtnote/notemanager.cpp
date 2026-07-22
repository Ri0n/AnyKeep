/*
QtNote - Simple note-taking application
Copyright (C) 2010 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

Contacts:
E-Mail: rion4ik@gmail.com XMPP: rion@jabber.ru
*/

#include "notemanager.h"

#include <QCoreApplication>
#include <QSettings>
#include <QStringList>

#include <list>

namespace QtNote {

namespace {
    bool noteMatchesFilter(const Note &note, const QString &filter)
    {
        if (note.title().contains(filter, Qt::CaseInsensitive)) {
            return true;
        }

        auto tagFilter = filter;
        if (tagFilter.startsWith(QLatin1Char('*'))) {
            tagFilter.remove(0, 1);
        }
        if (tagFilter.isEmpty()) {
            return false;
        }

        for (const auto &tag : note.tags()) {
            if (tag.contains(tagFilter, Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
    }
}

/**
 * @brief Notes search class.
 * Search object is owned by storage and self-destroyed on search-end
 */
class NoteFinder : public QObject {
    Q_OBJECT

protected:
    NoteStorage         &_storage;
    QList<Note>          pending_;
    QString              text_;
    QPointer<StorageJob> activeJob_;
    bool                 aborted_ { false };

    void loadNext();

public:
    NoteFinder(NoteStorage &storage) : QObject(&storage), _storage(storage) { }

    NoteStorage &storage() const { return _storage; }

signals:
    void found(const QString &noteId);
    void completed();

public slots:
    virtual void start(const QString &text);
    virtual void abort();
};

void NoteFinder::start(const QString &text)
{
    aborted_   = false;
    text_      = text;
    auto *job  = _storage.refreshNotesAsync(0, this);
    activeJob_ = job;
    connect(job, &StorageJob::finished, this, [this, job]() {
        activeJob_.clear();
        if (aborted_)
            return;
        if (job->state() == StorageJob::Succeeded)
            pending_ = job->result();
        loadNext();
    });
}

void NoteFinder::loadNext()
{
    if (aborted_)
        return;
    if (pending_.isEmpty()) {
        emit completed();
        deleteLater();
        return;
    }

    const auto summary = pending_.takeFirst();
    auto      *job     = _storage.loadNoteAsync(summary.id(), this);
    activeJob_         = job;
    connect(job, &StorageJob::finished, this, [this, job, id = summary.id()]() {
        activeJob_.clear();
        if (!aborted_ && job->state() == StorageJob::Succeeded
            && job->result().text().contains(text_, Qt::CaseInsensitive)) {
            emit found(id);
        }
        loadNext();
    });
}

void NoteFinder::abort()
{
    aborted_ = true;
    pending_.clear();
    if (activeJob_)
        activeJob_->cancel();
    deleteLater();
}

GlobalNoteFinder::GlobalNoteFinder(QObject *parent) : QObject(parent) { }

void GlobalNoteFinder::start(const QString &text)
{
    abort();
    for (auto &storage : NoteManager::instance()->storages()) {
        auto so = new NoteFinder(*storage);
        connect(so, SIGNAL(found(QString)), SLOT(noteFound(QString)));
        connect(so, SIGNAL(completed()), SLOT(searcherFinished()));
        searchers.insert(so, so);
        so->start(text);
    }
}

void GlobalNoteFinder::abort()
{
    for (auto s : std::as_const(searchers)) {
        if (s) {
            s->abort();
        }
    }
    searchers.clear();
}

void GlobalNoteFinder::noteFound(const QString &noteId)
{
    NoteFinder *nf = dynamic_cast<NoteFinder *>(sender());
    emit        found(nf->storage().systemName(), noteId);
}

void GlobalNoteFinder::searcherFinished()
{
    NoteFinder *nf = dynamic_cast<NoteFinder *>(sender());
    searchers.remove(nf);
    nf->disconnect(this);
    if (!searchers.count()) {
        emit completed();
    }
}

/************************************************************************************
 * NoteManager                                                                      *
 ************************************************************************************/
NoteManager::NoteManager(QObject *parent) : QObject(parent)
{
    _priorities = QSettings().value("storage.priority").toStringList();
}

void NoteManager::registerStorage(std::unique_ptr<NoteStorage> ownedStorage)
{
    auto *storage = ownedStorage.get();
    if (!storage)
        return;
    _prioCache.clear();
    _storages.insert(storage->systemName(), storage);
    _ownedStorages.insert_or_assign(storage->systemName(), std::move(ownedStorage));
    if (!_priorities.contains(storage->systemName())) {
        if (storage->systemName() == QLatin1String("ptf")) {
            _priorities.prepend(storage->systemName()); // highest priority for ptf if not set
        } else {
            _priorities.append(storage->systemName()); // lowest priority if not set
        }
    }

    connect(storage, &NoteStorage::invalidated, this, &NoteManager::handleStorageChanged);
    connect(storage, &NoteStorage::noteAdded, this, &NoteManager::handleStorageChanged);
    connect(storage, &NoteStorage::noteModified, this, &NoteManager::handleStorageChanged);
    connect(storage, &NoteStorage::noteRemoved, this, &NoteManager::handleStorageChanged);
    connect(storage, &NoteStorage::noteIdChanged, this, &NoteManager::handleStorageChanged);

    emit                        storageAdded(storage);
    auto                       *job = storage->initAsync(this);
    const QPointer<NoteStorage> storageGuard(storage);
    connect(job, &StorageJob::finished, this, [this, storageGuard, job]() {
        if (!storageGuard) {
            job->deleteLater();
            return;
        }
        if (job->state() == StorageJob::Succeeded)
            emit storageReady(storageGuard);
        emit storageChanged(storageGuard);
        job->deleteLater();
    });
}

void NoteManager::unregisterStorage(NoteStorage *storage)
{
    if (!storage)
        return;
    const auto storageId = storage->systemName();
    emit       storageAboutToBeRemoved(storage);
    _prioCache.clear();
    disconnect(storage);
    _storages.remove(storageId);
    emit storageRemoved(storage);
    storage->shutdown();
    _ownedStorages.erase(storageId);
}

bool NoteManager::loadAll()
{
    // mostly stubbed method for future use.
    if (_storages.count() == 0) {
        return false;
    }
    return true;
}

void NoteManager::handleStorageChanged()
{
    auto *changedStorage = qobject_cast<NoteStorage *>(sender());
    if (!changedStorage)
        return;

    const auto it = _storages.constFind(changedStorage->systemName());
    if (it != _storages.constEnd())
        emit storageChanged(it.value());
}

QList<Note> NoteManager::noteList(int count) const
{
    QList<Note> ret;
    for (const auto &storage : prioritizedStorages()) {
        ret += storage->noteList(count);
    }
    std::sort(ret.begin(), ret.end(), noteListItemModifyComparer);
    return ret.mid(0, count);
}

QList<Note> NoteManager::noteList(int offset, int limit, const QString &titleFilter) const
{
    offset = qMax(0, offset);
    if (limit <= 0)
        return {};

    const QString filter = titleFilter.trimmed();
    auto          notes  = filter.isEmpty() ? noteList(offset + limit) : noteList(-1);

    if (!filter.isEmpty()) {
        QList<Note> filtered;
        filtered.reserve(notes.size());
        for (const auto &note : std::as_const(notes)) {
            if (noteMatchesFilter(note, filter))
                filtered.append(note);
        }
        notes = std::move(filtered);
    }

    if (offset >= notes.size())
        return {};
    return notes.mid(offset, limit);
}

Note NoteManager::note(const QString &storageId, const QString &noteId)
{
    if (!storageId.isEmpty() && !noteId.isEmpty()) {
        NoteStorage::Ptr s = storage(storageId);
        if (s) {
            return s->note(noteId);
        }
    }
    return Note();
}

NoteListJob *NoteManager::refreshNotesAsync(int count, QObject *owner)
{
    auto *aggregate = new NoteListJob(owner ? owner : this);
    aggregate->start();
    const auto storageList = prioritizedStorages(true);
    if (storageList.empty()) {
        aggregate->complete({});
        return aggregate;
    }

    struct State {
        int          pending { 0 };
        QList<Note>  notes;
        StorageError firstError;
    };
    auto state     = QSharedPointer<State>::create();
    state->pending = int(storageList.size());
    for (const auto &storage : storageList) {
        auto *job = storage->refreshNotesAsync(count, aggregate);
        connect(job, &StorageJob::finished, aggregate, [aggregate, job, state, count]() {
            if (job->state() == StorageJob::Succeeded)
                state->notes += job->result();
            else if (!state->firstError)
                state->firstError = job->error();
            if (--state->pending != 0 || aggregate->isFinished())
                return;
            std::sort(state->notes.begin(), state->notes.end(), noteListItemModifyComparer);
            if (count >= 0)
                state->notes = state->notes.mid(0, count);
            if (!state->notes.isEmpty() || !state->firstError)
                aggregate->complete(state->notes);
            else
                aggregate->fail(state->firstError);
        });
    }
    return aggregate;
}

NoteLoadJob *NoteManager::loadNoteAsync(const QString &storageId, const QString &noteId, QObject *owner)
{
    const auto targetStorage = storage(storageId);
    if (targetStorage)
        return targetStorage->loadNoteAsync(noteId, owner ? owner : this);

    auto *job = new NoteLoadJob(owner ? owner : this);
    job->start();
    StorageError error;
    error.code    = StorageError::NotFound;
    error.message = tr("Storage was not found");
    job->fail(error);
    return job;
}

const QMap<QString, NoteStorage::Ptr> NoteManager::storages(bool withInvalid) const
{
    if (withInvalid) {
        return _storages;
    }
    decltype(_storages) ret;
    for (auto &item : _storages) {
        if (item->isAccessible()) {
            ret.insert(item->systemName(), item);
        }
    }
    return ret;
}

const std::list<NoteStorage::Ptr> NoteManager::prioritizedStorages(bool withInvalid) const
{
    if (!_prioCache.size()) {

        auto storages = _storages;

        for (auto &code : _priorities) {
            auto storage = storages.take(code);
            if (storage) {
                _prioCache.push_back(storage);
            }
        }

        for (auto &storage : storages) {
            _prioCache.push_back(storage);
        }
    }

    if (withInvalid) {
        return _prioCache;
    }

    decltype(_prioCache) ret;
    for (auto &storage : _prioCache) {
        if (storage->isAccessible()) {
            ret.push_back(storage);
        }
    }

    return ret;
}

NoteStorage::Ptr NoteManager::storage(const QString &storageId) const
{
    // returns 0 if doesn't exists (see default contstructor StorageItem)
    return _storages.value(storageId);
}

void NoteManager::setPriorities(const QStringList &storageCodes)
{
    _prioCache.clear();
    _priorities = storageCodes;
    QSettings().setValue("storage.priority", _priorities);
}

/*int NoteManager::notesAmount(const QString &storage = QString()) const
{
    int result = 0;
    if (storage.isEmpty()) {
        foreach (const StorageItem &s, storages()) {
            result += s.storage->notesAmount();
        }
    } else if (storages().contains(storage)) {
        result += storages().value(storage).storage->notesAmount();
    }
    return result;
}*/

NoteManager *NoteManager::instance()
{
    if (!NoteManager::_instance) {
        NoteManager::_instance = new NoteManager(QCoreApplication::instance());
    }
    return NoteManager::_instance;
}

NoteManager *NoteManager::_instance = NULL;

} // namespace QtNote

#include "notemanager.moc"
