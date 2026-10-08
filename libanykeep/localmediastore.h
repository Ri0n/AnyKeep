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

struct ANYKEEP_EXPORT LocalMediaRangeResult {
    QByteArray value;
    qint64     totalSize { -1 };
    QString    error;
    explicit   operator bool() const { return error.isEmpty(); }
};

struct ANYKEEP_EXPORT LocalMediaExternalSource {
    QString           fileName;
    qint64            size { 0 };
    qint64            modifiedMsecsSinceEpoch { 0 };
    QByteArray        blobId;
    QByteArray        checksum;
    quint32           chunkSize { 0 };
    QList<QByteArray> chunkHashes;

    bool hasFingerprint() const
    {
        return blobId.size() == 32 && checksum.size() == 32 && chunkSize > 0
            && chunkHashes.size() == (size == 0 ? 0 : (size + qint64(chunkSize) - 1) / qint64(chunkSize));
    }
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

    static constexpr qint64 ManagedChunkSize  = 1024 * 1024;
    static constexpr qint64 ExternalChunkSize = 1024 * 1024;

    LocalMediaResult importFile(const QString &fileName, const QUuid &attachmentId = {});
    // Keep the original file in place. Linking is metadata-only: content
    // fingerprinting is deferred until publication/integrity requires it.
    LocalMediaResult referenceFile(const QString &fileName, const QUuid &attachmentId = {});
    // Return a reference with blobId/checksum populated, reusing the encrypted
    // cached fingerprint when the cheap source revision still matches.
    LocalMediaResult fingerprintExternalFile(const MediaReference &reference);
    LocalMediaResult importData(const QByteArray &data, const QString &originalName, const QString &mediaType,
                                const QUuid &attachmentId = {});

    LocalMediaDataResult           data(const QByteArray &blobId) const;
    LocalMediaDataResult           data(const MediaReference &reference) const;
    LocalMediaRangeResult          readManagedRange(const QByteArray &blobId, qint64 offset, qint64 maxSize) const;
    LocalMediaExternalSourceResult externalSource(const MediaReference &reference) const;
    bool                           contains(const QByteArray &blobId) const;
    bool                           contains(const MediaReference &reference) const;
    bool                           containsManagedBlob(const QByteArray &blobId) const;
    bool                           isChunkedManagedBlob(const QByteArray &blobId) const;

private:
    friend class IrisProgressiveMediaLiveTest;
    QByteArray                     masterKey(QString *error) const;
    QString                        blobPath(const QByteArray &blobId) const;
    QString                        externalSourcePath(const QUuid &attachmentId) const;
    QString                        legacyExternalSourcePath(const QByteArray &blobId) const;
    LocalMediaExternalSourceResult loadExternalSource(const MediaReference &reference) const;
    QString writeExternalSource(const QUuid &attachmentId, const LocalMediaExternalSource &source) const;

    QString            rootPath_;
    mutable QMutex     masterKeyMutex_;
    mutable QByteArray masterKey_;
};

} // namespace AnyKeep

#endif // LOCALMEDIASTORE_H
