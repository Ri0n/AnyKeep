#ifndef PORTABLEBACKUPARCHIVE_H
#define PORTABLEBACKUPARCHIVE_H

#include "anykeep_export.h"

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QString>

namespace AnyKeep {

// The manifest deliberately contains portable identities, not local paths,
// Android content URIs, encryption keys or provider credentials.
struct ANYKEEP_EXPORT PortableBackupEntry {
    QString    path;
    QString    mimeType;
    QString    kind;
    QString    id;
    QString    noteId;
    QString    providerId;
    qint64     size { 0 };
    QByteArray sha256;
};

struct ANYKEEP_EXPORT PortableBackupSource {
    PortableBackupEntry entry;
    // The caller owns the readable device and supplies its remaining byte count.
    // Devices can be sequential; the archive writer reads each one only once.
    QIODevice *device { nullptr };
};

struct ANYKEEP_EXPORT PortableBackupManifest {
    static constexpr int CurrentVersion = 1;
    QList<PortableBackupEntry> entries;
};

class ANYKEEP_EXPORT PortableBackupArchive final {
public:
    // A plain, uncompressed USTAR archive with manifest.json last. It is
    // inspectable using ordinary archive tools; SHA-256 detects accidental
    // corruption, NOT malicious modification. Do not put secrets in this mode.
    static constexpr qint64 MaxEntryBytes    = (qint64(1) << 33) - 1;
    static constexpr qint64 MaxTotalBytes    = qint64(128) * 1024 * 1024 * 1024;
    static constexpr qint64 MaxManifestBytes = 4 * 1024 * 1024;
    static constexpr int    MaxEntries       = 50000;

    static bool safeEntryPath(const QString &path);

    // Caller must write into a staging file, then publish it only after success.
    // On failure, the output may contain a truncated, invalid archive.
    static bool writeReadable(QIODevice *output, const QList<PortableBackupSource> &sources,
                              QString *error = nullptr);

    // Performs a complete streaming content/hash/manifest verification without
    // writing any files. Importers must validate before applying any changes.
    // Does not support the future password-protected envelope.
    static bool inspectReadable(QIODevice *input, PortableBackupManifest *manifest,
                                QString *error = nullptr);
};

} // namespace AnyKeep

#endif // PORTABLEBACKUPARCHIVE_H
