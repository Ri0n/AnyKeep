#ifndef FILEREMOTECACHESTORE_H
#define FILEREMOTECACHESTORE_H

#include "remotecachestore.h"

#include <QByteArray>

namespace AnyKeep {

class ANYKEEP_EXPORT FileRemoteCacheStore final : public RemoteCacheStore {
public:
    FileRemoteCacheStore(QString filePath, QString instanceId, QByteArray masterKey);

    RemoteCacheResult<QList<RemoteCacheRecord>> records() const override;
    RemoteCacheError                            replaceRecords(const QList<RemoteCacheRecord> &records) override;
    RemoteCacheError                            put(const RemoteCacheRecord &record) override;
    RemoteCacheError                            remove(const QString &id) override;

private:
    QString    filePath_;
    QString    instanceId_;
    QByteArray masterKey_;
};

} // namespace AnyKeep

#endif // FILEREMOTECACHESTORE_H
