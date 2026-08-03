#ifndef FILEDRAFTSTORE_H
#define FILEDRAFTSTORE_H

#include "draftstore.h"

#include <QByteArray>

namespace AnyKeep {

class ANYKEEP_EXPORT FileDraftStore final : public DraftStore {
public:
    FileDraftStore(QString rootPath, QByteArray masterKey);

    static constexpr int MasterKeySize = 32;
    static QByteArray    generateMasterKey();
    static bool          cryptoAvailable();

    DraftStoreError                      write(const DraftRecord &record) override;
    DraftStoreResult<DraftRecord>        load(const QUuid &id) const override;
    DraftStoreResult<QList<DraftRecord>> records() const override;
    DraftStoreError                      transition(const QUuid &id, DraftRecord::State state) override;
    DraftStoreError                      remove(const QUuid &id) override;

private:
    QString    rootPath_;
    QByteArray masterKey_;

    QString                      pathFor(const QUuid &id, DraftRecord::State state) const;
    QString                      findPath(const QUuid &id, DraftRecord::State *state = nullptr) const;
    DraftStoreError              ensureDirectories() const;
    DraftStoreResult<QByteArray> encrypt(const QUuid &id, const QByteArray &plainText) const;
    DraftStoreResult<QByteArray> decrypt(const QUuid &id, const QByteArray &envelope) const;
};

} // namespace AnyKeep

#endif // FILEDRAFTSTORE_H
