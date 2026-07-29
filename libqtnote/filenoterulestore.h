#ifndef FILENOTERULESTORE_H
#define FILENOTERULESTORE_H

#include "noterule.h"

#include <QByteArray>
#include <QString>

namespace QtNote {

/**
 * Encrypted, atomic persistence for the local rule set and its idempotence
 * markers. A successful replacement keeps the previous authenticated payload
 * as a sibling backup.
 */
class QTNOTE_EXPORT FileNoteRuleStore final {
public:
    FileNoteRuleStore(QString filePath, QByteArray masterKey);

    static bool cryptoAvailable();

    NoteRuleResult<NoteRuleSnapshot> load() const;
    NoteRuleResult<NoteRuleSnapshot> loadBackup() const;
    NoteRuleError                    save(const NoteRuleSnapshot &snapshot);

    bool    hasBackup() const;
    QString filePath() const { return filePath_; }
    QString backupFilePath() const;

    NoteRuleError restoreBackup(QString *preservedPath = nullptr);
    NoteRuleError recreate(QString *preservedPath = nullptr);

private:
    QString    filePath_;
    QByteArray masterKey_;

    NoteRuleResult<NoteRuleSnapshot> loadPath(const QString &path, bool absentIsEmpty) const;
    NoteRuleResult<QByteArray>       readRaw(const QString &path) const;
    NoteRuleError                    writeRaw(const QString &path, const QByteArray &bytes) const;
    NoteRuleError                    validateKey() const;
    NoteRuleError                    quarantineExisting(QString *preservedPath) const;
};

} // namespace QtNote

#endif // FILENOTERULESTORE_H
