#ifndef LOCALMEDIASTORE_H
#define LOCALMEDIASTORE_H

#include "mediareference.h"

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QString>

namespace AnyKeep {

struct ANYKEEP_EXPORT LocalMediaResult {
    MediaReference value;
    QString        error;
    explicit       operator bool() const { return error.isEmpty(); }
};

struct ANYKEEP_EXPORT LocalMediaDataResult {
    QByteArray value;
    QString    error;
    explicit   operator bool() const { return error.isEmpty(); }
};

struct ANYKEEP_EXPORT LocalMediaExternalSource {
    QString           fileName;
    qint64            size { 0 };
    QByteArray        checksum;
    qint64            modifiedMsecsSinceEpoch { 0 };
    quint32           chunkSize { 0 };
    QList<QByteArray> chunkHashes;
};

struct ANYKEEP_EXPORT LocalMediaExternalSourceResult {
    LocalMediaExternalSource value;
    QString                  error;
    explicit                 operator bool() const { return error.isEmpty(); }
};

class ANYKEEP_EXPORT LocalMediaStore {
public:
    explicit LocalMediaStore(const QString &rootPath = {}, const QByteArray &masterKey = {});
    static LocalMediaStore *instance();

    // Resolve the keychain-backed encryption key on the application thread.
    // Image providers may call data() from a worker thread, so the first
    // keychain access must never be deferred to requestImage().
    bool initialize(QString *error = nullptr) const;

    static constexpr qint64 ExternalChunkSize = 1024 * 1024;

    LocalMediaResult importFile(const QString &fileName, const QUuid &attachmentId = {});
    // Keep the original file in place and persist only an encrypted local locator
    // plus independently verifiable chunk fingerprints.
    LocalMediaResult referenceFile(const QString &fileName, const QUuid &attachmentId = {});
    LocalMediaResult importData(const QByteArray &data, const QString &originalName, const QString &mediaType,
                                const QUuid &attachmentId = {});

    LocalMediaDataResult           data(const QByteArray &blobId) const;
    LocalMediaExternalSourceResult externalSource(const MediaReference &reference) const;
    bool                           contains(const QByteArray &blobId) const;
    bool                           containsManagedBlob(const QByteArray &blobId) const;

private:
    QByteArray masterKey(QString *error) const;
    QString    blobPath(const QByteArray &blobId) const;
    QString    externalSourcePath(const QByteArray &blobId) const;
    LocalMediaExternalSourceResult loadExternalSource(const QByteArray &blobId) const;
    QString writeExternalSource(const QByteArray &blobId, const LocalMediaExternalSource &source) const;

    QString            rootPath_;
    mutable QMutex     masterKeyMutex_;
    mutable QByteArray masterKey_;
};

} // namespace AnyKeep

#endif // LOCALMEDIASTORE_H
