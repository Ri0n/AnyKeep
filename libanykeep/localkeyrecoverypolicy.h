#ifndef ANYKEEP_LOCALKEYRECOVERYPOLICY_H
#define ANYKEEP_LOCALKEYRECOVERYPOLICY_H

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QStringList>
#include <QUuid>

#include <utility>
#include <QString>

// Internal policy: a missing Android Keystore private key may be replaced
// automatically only if no encrypted local AnyKeep content can be orphaned.
// Plain PTF files, UI settings and other unrelated app files are not keyed by
// LocalDataKeyStore, so their presence must not block an otherwise safe repair.
namespace AnyKeep::LocalKeyRecoveryPolicy {

inline QString protectedCategory(const QString &path)
{
    if (path.startsWith(QStringLiteral("drafts/"))
        || path.startsWith(QStringLiteral("drafts-unrecoverable-")))
        return QStringLiteral("drafts");
    if (path.startsWith(QStringLiteral("media/")))
        return QStringLiteral("media");
    if (path.startsWith(QStringLiteral("remote-cache/")))
        return QStringLiteral("remote-cache");
    if (path.startsWith(QStringLiteral("xmpp-omemo-")))
        return QStringLiteral("xmpp-omemo");
    if (path.startsWith(QStringLiteral("folder-catalog.bin")))
        return QStringLiteral("folder-catalog");
    if (path.startsWith(QStringLiteral("note-rules.bin")))
        return QStringLiteral("note-rules");
    return {};
}

inline bool containsProtectedFiles(const QString &rootPath, QString *category = nullptr)
{
    if (category)
        category->clear();
    const QDir root(rootPath);
    QDirIterator files(rootPath, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto kind = protectedCategory(root.relativeFilePath(files.next()));
        if (!kind.isEmpty()) {
            if (category)
                *category = kind;
            return true;
        }
    }
    return false;
}

// Preserve potentially valuable old ciphertext instead of deleting it.
// In the verified missing-private-key case, we can safely start a new editing
// session with a different keychain alias while retaining both the old
// ciphertext and the old Keystore alias for possible future recovery.
// Each move stays within AppDataLocation, on the same filesystem. Nothing
// under ptf/ is ever touched.
inline bool quarantineProtectedFiles(const QString &rootPath, QString *backupPath, QString *error)
{
    if (backupPath)
        backupPath->clear();
    if (error)
        error->clear();
    const QDir root(rootPath);
    if (!root.exists()) {
        if (error)
            *error = QStringLiteral("Local application data directory is missing");
        return false;
    }

    QStringList toMove;
    const auto entries = root.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        const auto name = entry.fileName();
        const auto category = protectedCategory(name + (entry.isDir() ? QStringLiteral("/") : QString()));
        if (category.isEmpty())
            continue;
        if (entry.isSymLink()) {
            if (error)
                *error = QStringLiteral("Protected local data includes a symbolic link");
            return false;
        }
        toMove.append(name);
    }
    if (toMove.isEmpty())
        return true;

    const QString relativeBackup = QStringLiteral("lost-key-backup/")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(root.filePath(relativeBackup))) {
        if (error)
            *error = QStringLiteral("Cannot create backup directory for inaccessible local encrypted data");
        return false;
    }

    QStringList moved;
    for (const auto &name : std::as_const(toMove)) {
        if (QDir().rename(root.filePath(name), root.filePath(relativeBackup + QLatin1Char('/') + name))) {
            moved.append(name);
            continue;
        }
        // No key is touched until all files are safely quarantined.
        bool restored = true;
        for (auto it = moved.crbegin(); it != moved.crend(); ++it) {
            if (!QDir().rename(root.filePath(relativeBackup + QLatin1Char('/') + *it), root.filePath(*it)))
                restored = false;
        }
        if (error)
            *error = restored ? QStringLiteral("Cannot preserve existing encrypted local data")
                              : QStringLiteral("Failed to preserve encrypted local data and roll back moves");
        return false;
    }
    if (backupPath)
        *backupPath = root.filePath(relativeBackup);
    return true;
}

inline bool restoreQuarantinedFiles(const QString &rootPath, const QString &backupPath)
{
    if (backupPath.isEmpty())
        return true;
    const QDir root(rootPath);
    const QDir backup(backupPath);
    if (!backup.exists())
        return false;
    bool restored = true;
    const auto entries = backup.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
    for (const auto &entry : entries) {
        if (!QDir().rename(entry.absoluteFilePath(), root.filePath(entry.fileName())))
            restored = false;
    }
    return restored;
}

} // namespace AnyKeep::LocalKeyRecoveryPolicy

#endif // ANYKEEP_LOCALKEYRECOVERYPOLICY_H
