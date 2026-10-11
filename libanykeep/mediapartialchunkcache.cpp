#include "mediapartialchunkcache.h"

#include "secureenvelope.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <limits>
#include <utility>

namespace AnyKeep {
namespace {
constexpr qint64 MaxChunk = 16LL * 1024 * 1024;
constexpr qint64 MaxEnvelopeOverhead = 64 * 1024;

AeadContext context(const QString &identity, qint64 size, qint64 chunkSize, quint64 index)
{
    return { KeyDomain::LocalMedia, QStringLiteral("anykeep-partial-media"),
             identity + QLatin1Char(':') + QString::number(index),
             1, QStringLiteral("local-range:%1:%2").arg(size).arg(chunkSize) };
}
}

MediaPartialChunkCache::MediaPartialChunkCache(Format format, QString directory, QString contentIdentity,
                                               qint64 totalPlainSize, qint64 chunkSize, QByteArray masterKey) :
    format_(format), directory_(std::move(directory)), identity_(std::move(contentIdentity)),
    totalPlainSize_(totalPlainSize), chunkSize_(chunkSize), masterKey_(std::move(masterKey))
{
}

bool MediaPartialChunkCache::isValid() const
{
    if (directory_.isEmpty())
        return false;
    if (format_ == Format::VerifiedOpaque)
        return masterKey_.isEmpty();
    return !identity_.isEmpty() && masterKey_.size() == SecureEnvelope::MasterKeySize
        && totalPlainSize_ > 0 && chunkSize_ > 0 && chunkSize_ <= MaxChunk;
}

qint64 MediaPartialChunkCache::expectedPlainSize(quint64 index) const
{
    if (format_ != Format::LocallySealed || !isValid())
        return -1;
    const quint64 count = quint64(totalPlainSize_ - 1) / quint64(chunkSize_) + 1;
    if (index >= count)
        return -1;
    const qint64 offset = qint64(index * quint64(chunkSize_));
    return qMin(chunkSize_, totalPlainSize_ - offset);
}

bool MediaPartialChunkCache::requestValid(quint64 index, qint64 expectedBytes) const
{
    return isValid() && expectedBytes > 0 && expectedBytes <= MaxChunk + 4096
        && (format_ == Format::VerifiedOpaque || expectedPlainSize(index) == expectedBytes);
}

QString MediaPartialChunkCache::path(quint64 index) const
{
    return directory_ + QLatin1Char('/') + QString::number(index);
}

void MediaPartialChunkCache::track(quint64 index, qint64 size, bool available) const
{
    if (format_ != Format::LocallySealed)
        return;
    if (available && !verified_.contains(index)) {
        verified_.insert(index);
        verifiedBytes_ += size;
    } else if (!available && verified_.remove(index)) {
        verifiedBytes_ -= size;
    }
}

QStringList MediaPartialChunkCache::storedChunkNames() const
{
    if (!isValid())
        return {};
    return QDir(directory_).entryList(QDir::Files | QDir::NoSymLinks, QDir::Name);
}

QByteArray MediaPartialChunkCache::read(quint64 index, qint64 expectedBytes, Verify verify, QString *error) const
{
    if (error)
        error->clear();
    if (!requestValid(index, expectedBytes)
        || (format_ == Format::VerifiedOpaque && !verify)) {
        if (error)
            *error = QStringLiteral("Invalid partial-media cache read request");
        return {};
    }

    QFile file(path(index));
    if (!file.exists()) {
        track(index, expectedBytes, false);
        return {};
    }
    const qint64 limit = expectedBytes + (format_ == Format::LocallySealed ? MaxEnvelopeOverhead : 0);
    if (!file.open(QIODevice::ReadOnly) || !QFileInfo(file).isFile() || QFileInfo(file).isSymLink()
        || file.size() <= 0 || file.size() > limit) {
        file.close();
        QFile::remove(path(index));
        track(index, expectedBytes, false);
        if (error)
            *error = QStringLiteral("Unusable partial-media cache record");
        return {};
    }
    const QByteArray stored = file.read(limit + 1);
    const bool fullRead = stored.size() == file.size();
    file.close();

    QByteArray plain;
    if (fullRead && format_ == Format::LocallySealed) {
        const auto opened = SecureEnvelope::open(
            stored, masterKey_, context(identity_, totalPlainSize_, chunkSize_, index));
        if (opened)
            plain = opened.value;
    } else if (fullRead) {
        plain = stored;
    }
    if (plain.size() == expectedBytes && (!verify || verify(index, plain))) {
        track(index, expectedBytes, true);
        return plain;
    }

    QFile::remove(path(index));
    track(index, expectedBytes, false);
    if (error)
        *error = QStringLiteral("Partial-media cache record failed authentication");
    return {};
}

bool MediaPartialChunkCache::write(quint64 index, const QByteArray &bytes, qint64 expectedBytes,
                                   Verify verify, QString *error) const
{
    if (error)
        error->clear();
    if (!requestValid(index, expectedBytes) || bytes.size() != expectedBytes
        || (format_ == Format::VerifiedOpaque && (!verify || !verify(index, bytes)))) {
        if (error)
            *error = QStringLiteral("Invalid or unauthenticated partial-media cache write");
        return false;
    }
    QByteArray stored = bytes;
    if (format_ == Format::LocallySealed) {
        const auto sealed = SecureEnvelope::seal(
            bytes, masterKey_, context(identity_, totalPlainSize_, chunkSize_, index));
        if (!sealed) {
            if (error)
                *error = sealed.error.message;
            return false;
        }
        stored = sealed.value;
    }
    if (!QDir().mkpath(directory_)) {
        if (error)
            *error = QStringLiteral("Cannot create partial-media cache directory");
        return false;
    }
    QSaveFile file(path(index));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(stored) != stored.size() || !file.commit()) {
        if (error)
            *error = QStringLiteral("Could not commit authenticated media record");
        return false;
    }
    track(index, expectedBytes, true);
    return true;
}

qint64 MediaPartialChunkCache::restoreVerifiedBytes() const
{
    if (!isValid() || format_ != Format::LocallySealed)
        return 0;
    if (restored_)
        return verifiedBytes_;
    restored_ = true;
    verified_.clear();
    verifiedBytes_ = 0;
    for (const QString &name : storedChunkNames()) {
        bool ok = false;
        const quint64 index = name.toULongLong(&ok);
        if (!ok || QString::number(index) != name)
            continue;
        const qint64 expected = expectedPlainSize(index);
        if (expected > 0)
            read(index, expected);
    }
    return verifiedBytes_;
}
} // namespace AnyKeep
