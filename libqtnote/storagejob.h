#ifndef STORAGEJOB_H
#define STORAGEJOB_H

#include <QList>
#include <QObject>
#include <QString>

#include "note.h"
#include "qtnote_export.h"

namespace QtNote {

struct QTNOTE_EXPORT StorageError {
    enum Code { None, Cancelled, NotConfigured, NotFound, Conflict, Unavailable, Io, Network, Authentication, Other };

    Code     code { None };
    QString  message;
    bool     retryable { false };
    explicit operator bool() const { return code != None; }
};

class QTNOTE_EXPORT StorageJob : public QObject {
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
    void stateChanged(QtNote::StorageJob::State state);
    void finished();

protected:
    bool complete();
    bool fail(const StorageError &error);

private:
    bool setTerminalState(State state, const StorageError &error = {});

    State        state_ { Pending };
    StorageError error_;
};

class QTNOTE_EXPORT StorageInitJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::complete;
    using StorageJob::fail;
    using StorageJob::StorageJob;
};

class QTNOTE_EXPORT NoteListJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::StorageJob;

    const QList<Note> &result() const { return result_; }
    bool               complete(QList<Note> result);
    using StorageJob::fail;

private:
    QList<Note> result_;
};

class QTNOTE_EXPORT NoteLoadJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::StorageJob;

    const Note &result() const { return result_; }
    bool        complete(const Note &result);
    using StorageJob::fail;

private:
    Note result_;
};

class QTNOTE_EXPORT NoteSaveJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::StorageJob;

    const Note &result() const { return result_; }
    bool        complete(const Note &result);
    using StorageJob::fail;

private:
    Note result_;
};

class QTNOTE_EXPORT NoteRemoveJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::complete;
    using StorageJob::fail;
    using StorageJob::StorageJob;
};

class QTNOTE_EXPORT NoteReorderJob final : public StorageJob {
    Q_OBJECT
public:
    using StorageJob::complete;
    using StorageJob::fail;
    using StorageJob::StorageJob;
};

} // namespace QtNote

#endif // STORAGEJOB_H
