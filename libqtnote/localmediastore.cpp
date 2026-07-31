#include "localmediastore.h"

#include "localdatakeystore.h"
#include "secureenvelope.h"
#include "utils.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageAuthenticationCode>
#include <QMimeDatabase>
#include <QMutexLocker>
#include <QSaveFile>
#include <QThread>

#include <utility>

namespace QtNote {
namespace {
    // Consumer schema passed to SecureEnvelope::associatedData(); see AeadContext::schema.
    constexpr quint32 AeadContextSchema = 1;
}

LocalMediaStore::LocalMediaStore(const QString &rootPath, const QByteArray &masterKey) :
    rootPath_(rootPath), masterKey_(masterKey)
{
}

LocalMediaStore *LocalMediaStore::instance()
{
    static LocalMediaStore store;
    return &store;
}

QString LocalMediaStore::blobPath(const QByteArray &blobId) const
{
    const auto hex  = blobId.toHex();
    const auto root = rootPath_.isEmpty() ? Utils::qtnoteDataDir() + QStringLiteral("/media") : rootPath_;
    return root + QLatin1Char('/') + QString::fromLatin1(hex.left(2)) + QLatin1Char('/')
        + QString::fromLatin1(hex.mid(2, 2)) + QLatin1Char('/') + QString::fromLatin1(hex) + QStringLiteral(".blob");
}

bool LocalMediaStore::initialize(QString *error) const
{
    {
        const QMutexLocker locker(&masterKeyMutex_);
        if (!masterKey_.isEmpty()) {
            if (error)
                error->clear();
            return true;
        }
    }

    auto *application = QCoreApplication::instance();
    if (application && QThread::currentThread() != application->thread()) {
        if (error) {
            *error = QCoreApplication::translate(
                "LocalMediaStore", "The local media store must be initialized on the application thread.");
        }
        return false;
    }

    QString keyError;
    auto    key = LocalDataKeyStore::loadOrCreateMasterKey(&keyError);
    if (key.isEmpty()) {
        if (error)
            *error = keyError;
        return false;
    }

    {
        const QMutexLocker locker(&masterKeyMutex_);
        if (masterKey_.isEmpty())
            masterKey_ = std::move(key);
    }
    if (error)
        error->clear();
    return true;
}

QByteArray LocalMediaStore::masterKey(QString *error) const
{
    {
        const QMutexLocker locker(&masterKeyMutex_);
        if (!masterKey_.isEmpty()) {
            if (error)
                error->clear();
            return masterKey_;
        }
    }

    if (!initialize(error))
        return {};

    const QMutexLocker locker(&masterKeyMutex_);
    return masterKey_;
}

LocalMediaResult LocalMediaStore::importFile(const QString &fileName, const QUuid &attachmentId)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
        return { {}, file.errorString() };
    const auto mediaType = QMimeDatabase().mimeTypeForFile(fileName, QMimeDatabase::MatchContent).name();
    return importData(file.readAll(), QFileInfo(fileName).fileName(), mediaType, attachmentId);
}

LocalMediaResult LocalMediaStore::importData(const QByteArray &plain, const QString &originalName,
                                             const QString &mediaType, const QUuid &attachmentId)
{
    QString    keyError;
    const auto masterKey = this->masterKey(&keyError);
    if (masterKey.isEmpty())
        return { {}, keyError };
    const auto idKey  = SecureEnvelope::deriveKey(masterKey, KeyDomain::LocalMedia);
    const auto blobId = QMessageAuthenticationCode::hash(plain, idKey, QCryptographicHash::Sha256);
    const auto path   = blobPath(blobId);
    if (!QFileInfo::exists(path)) {
        const AeadContext context { KeyDomain::LocalMedia, QStringLiteral("qtnote-local-media"),
                                    QString::fromLatin1(blobId.toHex()), AeadContextSchema,
                                    QStringLiteral("attachment") };
        const auto        sealed = SecureEnvelope::seal(plain, masterKey, context);
        if (!sealed)
            return { {}, sealed.error.message };
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return { {}, QStringLiteral("Failed to create local media directory") };
        QSaveFile file(path);
        file.setDirectWriteFallback(false);
        if (!file.open(QIODevice::WriteOnly))
            return { {}, file.errorString() };
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (file.write(sealed.value) != sealed.value.size() || !file.commit())
            return { {}, file.errorString() };
    }

    MediaReference reference;
    reference.id           = attachmentId.isNull() ? QUuid::createUuid() : attachmentId;
    reference.blobId       = blobId;
    reference.originalName = QFileInfo(originalName).fileName();
    reference.portableName = Utils::portableFileName(reference.originalName, QStringLiteral("attachment"));
    reference.mediaType    = mediaType;
    reference.size         = plain.size();
    reference.checksum     = QCryptographicHash::hash(plain, QCryptographicHash::Sha256);
    return { reference, {} };
}

LocalMediaDataResult LocalMediaStore::data(const QByteArray &blobId) const
{
    QString    keyError;
    const auto masterKey = this->masterKey(&keyError);
    if (masterKey.isEmpty())
        return { {}, keyError };
    QFile file(blobPath(blobId));
    if (!file.open(QIODevice::ReadOnly))
        return { {}, file.errorString() };
    const AeadContext context { KeyDomain::LocalMedia, QStringLiteral("qtnote-local-media"),
                                QString::fromLatin1(blobId.toHex()), AeadContextSchema, QStringLiteral("attachment") };
    const auto        opened = SecureEnvelope::open(file.readAll(), masterKey, context);
    if (!opened)
        return { {}, opened.error.message };
    return { opened.value, {} };
}

bool LocalMediaStore::contains(const QByteArray &blobId) const { return QFileInfo::exists(blobPath(blobId)); }

} // namespace QtNote
