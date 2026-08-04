#ifndef STORAGEJOB_H
#define STORAGEJOB_H

#include <QList>
#include <QObject>
#include <QString>

#include "anykeep_export.h"
#include "note.h"

namespace AnyKeep {

struct ANYKEEP_EXPORT StorageError {
    enum Code { None, Cancelled, NotConfigured, NotFound, Conflict, Unavailable, Io, Network, Authentication, Other };

    Code     code { None };
    QString  message;
    bool     retryable { false };
    explicit operator bool() const { return code != None; }
};

class ANYKEEP_EXPORT StorageJob : public QObject {
    Q_OBJECT
public:
    enum State { Pending, Running, Succeeded, Failed, Cancelled };
    Q_ENUM(State)

    explicit StorageJob(QObject *parent = nullptr);

    State        state() const { return state_; }
    StorageError error() const { return error_; }
    bool         isFinished() const { return state_ == Succeeded || state_ == Failed || state_ == Cancelled; }

    void         start();
    virtual void cancel();

signals:
    void stateChanged(AnyKeep::StorageJob::State state);
    void finished();

protected:
    bool complete();
    bool fail(const StorageError &error);

private:
    bool setTerminalState(State state, const StorageError &error = {});

    State        state_ { Pending };
    StorageError error_;
};

class ANYKEEP_EXPORT StorageInitJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::complete;
    using StorageJob::fail;
    using StorageJob::StorageJob;
};

class ANYKEEP_EXPORT NoteListJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::StorageJob;

    const QList<Note> &result() const { return result_; }
    bool               complete(QList<Note> result);
    using StorageJob::fail;

private:
    QList<Note> result_;
};

class ANYKEEP_EXPORT NoteLoadJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::StorageJob;

    const Note &result() const { return result_; }
    bool        complete(const Note &result);
    using StorageJob::fail;

private:
    Note result_;
};

class ANYKEEP_EXPORT NoteSaveJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::StorageJob;

    const Note &result() const { return result_; }
    bool        complete(const Note &result);
    using StorageJob::fail;

private:
    Note result_;
};

/**
 * Completes a metadata-only folder change and returns the canonical note
 * summary.  It deliberately mirrors NoteSaveJob rather than reusing it: a
 * storage can update folder metadata without rewriting the note body.
 */
class ANYKEEP_EXPORT NoteFolderChangeJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::StorageJob;

    const Note &result() const { return result_; }
    bool        complete(const Note &result);
    using StorageJob::fail;

private:
    Note result_;
};

/**
 * Signals completion of a native folder-catalog replacement.  The catalog is
 * supplied by the caller, so a separate result payload is unnecessary.
 */
class ANYKEEP_EXPORT FolderCatalogJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::complete;
    using StorageJob::fail;
    using StorageJob::StorageJob;
};

class ANYKEEP_EXPORT NoteRemoveJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::complete;
    using StorageJob::fail;
    using StorageJob::StorageJob;
};

class ANYKEEP_EXPORT NoteReorderJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::complete;
    using StorageJob::fail;
    using StorageJob::StorageJob;
};

} // namespace AnyKeep

#endif // STORAGEJOB_H
