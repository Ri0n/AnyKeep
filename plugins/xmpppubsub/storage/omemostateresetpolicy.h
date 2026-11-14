#ifndef ANYKEEP_XMPP_OMEMOSTATERESETPOLICY_H
#define ANYKEEP_XMPP_OMEMOSTATERESETPOLICY_H

#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <utility>

// Android Keystore may lose the key that sealed a *local* OMEMO identity.
// Only the corresponding local protocol/session state is reset; the XMPP
// note-storage key and remote encrypted notes must never be touched.
namespace AnyKeep::XmppOmemoStateReset {

inline const QStringList &stateSuffixes()
{
    static const QStringList suffixes { QString {}, QStringLiteral(".trust"), QStringLiteral(".iris"),
                                        QStringLiteral(".iris.trust"), QStringLiteral(".iris.jinglepub") };
    return suffixes;
}

inline bool stage(const QString &statePath, QString *stagingPath, QString *error)
{
    if (stagingPath)
        stagingPath->clear();
    if (error)
        error->clear();

    const QFileInfo base(statePath);
    const QDir root(base.absolutePath());
    if (!root.exists())
        return true;

    QStringList names;
    for (const auto &suffix : stateSuffixes()) {
        const QString name = base.fileName() + suffix;
        const QFileInfo entry(root.filePath(name));
        if (!entry.exists() && !entry.isSymLink())
            continue;
        if (!entry.isFile() || entry.isSymLink()) {
            if (error)
                *error = QStringLiteral("The local OMEMO state contains an unsupported file type");
            return false;
        }
        names.append(name);
    }
    if (names.isEmpty())
        return true;

    const QString path = root.filePath(QStringLiteral(".omemo-key-reset/")
                                       + QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(path)) {
        if (error)
            *error = QStringLiteral("Could not stage inaccessible OMEMO state");
        return false;
    }

    QStringList moved;
    for (const auto &name : std::as_const(names)) {
        if (QDir().rename(root.filePath(name), QDir(path).filePath(name))) {
            moved.append(name);
            continue;
        }
        bool rolledBack = true;
        for (auto it = moved.crbegin(); it != moved.crend(); ++it) {
            if (!QDir().rename(QDir(path).filePath(*it), root.filePath(*it)))
                rolledBack = false;
        }
        // On a partial rollback, keep any remaining staged files for manual
        // recovery; deleting the staging directory would destroy the originals.
        if (rolledBack)
            QDir(path).removeRecursively();
        if (error)
            *error = rolledBack ? QStringLiteral("Could not stage inaccessible OMEMO state")
                                : QStringLiteral("Could not restore OMEMO state after failed staging");
        return false;
    }
    if (stagingPath)
        *stagingPath = path;
    return true;
}

inline bool restore(const QString &statePath, const QString &stagingPath)
{
    if (stagingPath.isEmpty())
        return true;
    const QFileInfo base(statePath);
    const QDir root(base.absolutePath());
    const QDir stageDir(stagingPath);
    if (!stageDir.exists())
        return false;
    bool restored = true;
    for (const auto &entry : stageDir.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot)) {
        if (!QDir().rename(entry.absoluteFilePath(), root.filePath(entry.fileName())))
            restored = false;
    }
    if (restored)
        QDir(stagingPath).removeRecursively();
    return restored;
}

inline bool discard(const QString &stagingPath)
{
    return stagingPath.isEmpty() || QDir(stagingPath).removeRecursively();
}

} // namespace AnyKeep::XmppOmemoStateReset

#endif // ANYKEEP_XMPP_OMEMOSTATERESETPOLICY_H
