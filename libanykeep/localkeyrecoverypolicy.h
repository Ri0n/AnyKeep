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
        if (path.startsWith(QStringLiteral("drafts/"))
            || path.startsWith(QStringLiteral("drafts-unrecoverable-"))
            || path.startsWith(QStringLiteral("media/"))
            || path.startsWith(QStringLiteral("remote-cache/"))
            || path.startsWith(QStringLiteral("xmpp-omemo-"))
            || path.startsWith(QStringLiteral("folder-catalog.bin"))
            || path.startsWith(QStringLiteral("note-rules.bin"))) {
            if (category)
                *category = path.section(QLatin1Char('/'), 0, 0);
            return true;
        }
    }
    return false;
}

} // namespace AnyKeep::LocalKeyRecoveryPolicy

#endif // ANYKEEP_LOCALKEYRECOVERYPOLICY_H
