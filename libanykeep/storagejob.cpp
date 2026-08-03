#include "storagejob.h"

#include <utility>

namespace AnyKeep {

StorageJob::StorageJob(QObject *parent) : QObject(parent) { }

void StorageJob::start()
{
    if (state_ != Pending)
        return;
    state_ = Running;
    emit stateChanged(state_);
}

void StorageJob::cancel()
{
    StorageError error;
    error.code    = StorageError::Cancelled;
    error.message = tr("Operation cancelled");
    setTerminalState(Cancelled, error);
}

bool StorageJob::complete() { return setTerminalState(Succeeded); }

bool StorageJob::fail(const StorageError &error) { return setTerminalState(Failed, error); }

bool StorageJob::setTerminalState(State state, const StorageError &error)
{
    if (isFinished())
        return false;
    state_ = state;
    error_ = error;
    emit stateChanged(state_);
    emit finished();
    return true;
}

bool NoteListJob::complete(QList<Note> result)
{
    if (isFinished())
        return false;
    result_ = std::move(result);
    return StorageJob::complete();
}

bool NoteLoadJob::complete(const Note &result)
{
    if (isFinished())
        return false;
    result_ = result;
    return StorageJob::complete();
}

bool NoteSaveJob::complete(const Note &result)
{
    if (isFinished())
        return false;
    result_ = result;
    return StorageJob::complete();
}

bool NoteFolderChangeJob::complete(const Note &result)
{
    if (isFinished())
        return false;
    result_ = result;
    return StorageJob::complete();
}

} // namespace AnyKeep
