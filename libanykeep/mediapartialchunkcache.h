#ifndef ANYKEEP_MEDIAPARTIALCHUNKCACHE_H
#define ANYKEEP_MEDIAPARTIALCHUNKCACHE_H

#include "anykeep_export.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <functional>

namespace AnyKeep {

// Provider-neutral, individually verifiable persistent media records.
//
// VerifiedOpaque preserves the original XMPP cache format and stores only
// wire records that the provider's verifier has authenticated. LocallySealed
// stores plaintext downloaded from HTTPS/WebDAV *only after* encrypting each
// record with SecureEnvelope and a locally held profile master key. Neither
// format ever exposes stored bytes before verification.
class ANYKEEP_EXPORT MediaPartialChunkCache {
public:
    enum class Format { VerifiedOpaque, LocallySealed };
    using Verify = std::function<bool(quint64 index, const QByteArray &bytes)>;

    MediaPartialChunkCache(Format format, QString directory, QString contentIdentity = {},
                           qint64 totalPlainSize = 0, qint64 chunkSize = 0,
                           QByteArray masterKey = {});

    bool isValid() const;
    QByteArray read(quint64 index, qint64 expectedBytes, Verify verify = {}, QString *error = nullptr) const;
    bool write(quint64 index, const QByteArray &bytes, qint64 expectedBytes,
               Verify verify = {}, QString *error = nullptr) const;

    // Existing XMPP cache files retain their numeric names and ciphertext.
    QStringList storedChunkNames() const;

    // Restore and authenticate on the caller's worker thread. Only sealed
    // records have known plaintext chunk geometry; no remote fetch occurs.
    qint64 restoreVerifiedBytes() const;
    qint64 expectedPlainSize(quint64 index) const;

private:
    QString path(quint64 index) const;
    bool requestValid(quint64 index, qint64 expectedBytes) const;
    QString recordIdentity(quint64 index) const;

    Format format_;
    QString directory_;
    QString identity_;
    qint64 totalPlainSize_ { 0 };
    qint64 chunkSize_ { 0 };
    QByteArray masterKey_;
};

} // namespace AnyKeep
#endif // ANYKEEP_MEDIAPARTIALCHUNKCACHE_H
