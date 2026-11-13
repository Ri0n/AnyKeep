#ifndef ANYKEEP_LOCALKEYRECOVERYPOLICY_H
#define ANYKEEP_LOCALKEYRECOVERYPOLICY_H

#include <QDir>
#include <QDirIterator>
#include <QString>

// Internal policy: a missing Android Keystore private key may be replaced
// automatically only if no encrypted local AnyKeep content can be orphaned.
// Plain PTF files, UI settings and other unrelated app files are not keyed by
// LocalDataKeyStore, so their presence must not block an otherwise safe repair.
namespace AnyKeep::LocalKeyRecoveryPolicy {

inline bool containsProtectedFiles(const QString &rootPath, QString *category = nullptr)
{
    if (category)
        category->clear();
    const QDir root(rootPath);
    QDirIterator files(rootPath, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = root.relativeFilePath(files.next());
        QString kind;
        if (path.startsWith(QStringLiteral("drafts/"))
            || path.startsWith(QStringLiteral("drafts-unrecoverable-")))
            kind = QStringLiteral("drafts");
        else if (path.startsWith(QStringLiteral("media/")))
            kind = QStringLiteral("media");
        else if (path.startsWith(QStringLiteral("remote-cache/")))
            kind = QStringLiteral("remote-cache");
        else if (path.startsWith(QStringLiteral("xmpp-omemo-")))
            kind = QStringLiteral("xmpp-omemo");
        else if (path.startsWith(QStringLiteral("folder-catalog.bin")))
            kind = QStringLiteral("folder-catalog");
        else if (path.startsWith(QStringLiteral("note-rules.bin")))
            kind = QStringLiteral("note-rules");

        if (!kind.isEmpty()) {
            if (category)
                *category = kind;
            return true;
        }
    }
    return false;
}

} // namespace AnyKeep::LocalKeyRecoveryPolicy

#endif // ANYKEEP_LOCALKEYRECOVERYPOLICY_H
