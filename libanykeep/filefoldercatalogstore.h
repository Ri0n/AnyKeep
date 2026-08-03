#ifndef FILEFOLDERCATALOGSTORE_H
#define FILEFOLDERCATALOGSTORE_H

#include "foldercatalog.h"

#include <QByteArray>
#include <QString>

namespace AnyKeep {

/**
 * Encrypted, atomic persistent storage for the application-wide folder
 * catalog. Each successful replacement retains the previous valid encrypted
 * payload as a sibling backup.
 */
class ANYKEEP_EXPORT FileFolderCatalogStore final {
public:
    FileFolderCatalogStore(QString filePath, QByteArray masterKey);

    static bool cryptoAvailable();

    FolderCatalogResult<FolderCatalogSnapshot> load() const;
    FolderCatalogResult<FolderCatalogSnapshot> loadBackup() const;
    FolderCatalogError                         save(const FolderCatalogSnapshot &snapshot);

    bool    hasBackup() const;
    QString filePath() const { return filePath_; }
    QString backupFilePath() const;

    /// Validates and restores the backup while preserving existing files under
    /// an unrecoverable suffix. The optional result names the preserved primary.
    FolderCatalogError restoreBackup(QString *preservedPath = nullptr);
    /// Preserves unreadable data under an unrecoverable suffix and creates an
    /// empty catalog. This is an explicit recovery action, never automatic.
    FolderCatalogError recreate(QString *preservedPath = nullptr);

private:
    QString    filePath_;
    QByteArray masterKey_;

    FolderCatalogResult<FolderCatalogSnapshot> loadPath(const QString &path, bool absentIsEmpty) const;
    FolderCatalogResult<QByteArray>            readRaw(const QString &path) const;
    FolderCatalogError                         writeRaw(const QString &path, const QByteArray &bytes) const;
    FolderCatalogError                         validateKey() const;
    FolderCatalogError                         quarantineExisting(QString *preservedPath) const;
};

} // namespace AnyKeep

#endif // FILEFOLDERCATALOGSTORE_H
