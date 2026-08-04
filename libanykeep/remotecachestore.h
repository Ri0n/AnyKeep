#ifndef REMOTECACHESTORE_H
#define REMOTECACHESTORE_H

#include "anykeep_export.h"
#include "note.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVariantMap>

namespace AnyKeep {

struct ANYKEEP_EXPORT RemoteCacheRecord {
    enum SyncState { Synced, Stale, PendingUpload, PendingDelete, Conflict };

    QString               id;
    QString               title;
    QStringList           tags;
    QDateTime             modified;
    Note::Format          format { Note::PlainText };
    QString               body;
    bool                  bodyPresent { false };
    QUuid                 folderId;
    QVariantMap           backendData;
    SyncState             syncState { Synced };
    QDateTime             lastOpenedAt;
    QDateTime             cachedAt;
    QList<MediaReference> media;
};

struct ANYKEEP_EXPORT RemoteCacheError {
    enum Code { None, InvalidArgument, Locked, CryptoUnavailable, Corrupt, Io };

    Code     code { None };
    QString  message;
    explicit operator bool() const { return code != None; }
};

template <typename T> struct RemoteCacheResult {
    T                value;
    RemoteCacheError error;
    explicit         operator bool() const { return !error; }
};

class ANYKEEP_EXPORT RemoteCacheStore {
public:
    virtual ~RemoteCacheStore() = default;

    virtual RemoteCacheResult<QList<RemoteCacheRecord>> records() const                                         = 0;
    virtual RemoteCacheError                            replaceRecords(const QList<RemoteCacheRecord> &records) = 0;
    virtual RemoteCacheError                            put(const RemoteCacheRecord &record)                    = 0;
    virtual RemoteCacheError                            remove(const QString &id)                               = 0;
};

} // namespace AnyKeep

#endif // REMOTECACHESTORE_H
