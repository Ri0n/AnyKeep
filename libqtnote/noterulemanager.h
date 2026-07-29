#ifndef NOTERULEMANAGER_H
#define NOTERULEMANAGER_H

#include "noterule.h"
#include "qtnote_export.h"

#include <QObject>

#include <functional>
#include <memory>

namespace QtNote {

class FileNoteRuleStore;

/**
 * Owns the application's local rule set and encrypted application markers.
 * A damaged rule file safely disables automatic routing until the user
 * explicitly restores a backup or recreates the file.
 */
class QTNOTE_EXPORT NoteRuleManager final : public QObject {
    Q_OBJECT
public:
    static NoteRuleManager *instance();

    explicit NoteRuleManager(QObject *parent = nullptr);
    NoteRuleManager(std::unique_ptr<FileNoteRuleStore> store, QObject *parent = nullptr);
    ~NoteRuleManager() override;

    bool initialize(QString *errorText = nullptr);

    bool    isAvailable() const { return available_; }
    bool    needsRecovery() const { return needsRecovery_; }
    bool    hasRecoveryBackup() const;
    QString lastError() const { return lastError_; }

    const NoteRuleSnapshot &snapshot() const { return snapshot_; }
    QList<NoteRule>         rules() const;
    const NoteRule         *rule(const QUuid &id) const;

    NoteRuleResult<QUuid> addRule(NoteRule rule);
    NoteRuleError         updateRule(NoteRule rule);
    NoteRuleError         removeRule(const QUuid &id);
    NoteRuleError         moveRuleRelative(const QUuid &id, const QUuid &beforeId = {});
    NoteRuleError         setRuleEnabled(const QUuid &id, bool enabled);

    NoteRuleEvaluation evaluate(const NoteRuleEvaluationInput &input) const;
    bool               wasApplied(const QUuid &ruleId, const NoteRuleEvaluationInput &input) const;
    NoteRuleError      recordApplied(const QList<QUuid> &ruleIds, const NoteRuleEvaluationInput &input);
    /** Persists multiple note markers in one encrypted atomic replacement. */
    NoteRuleError      recordApplied(const QList<NoteRuleApplication> &applications);
    NoteRuleError      forgetApplied(const QString &storageId, const QString &noteId);

    NoteRuleError restoreBackup(QString *preservedPath = nullptr);
    NoteRuleError recreate(QString *preservedPath = nullptr);

signals:
    void rulesChanged();
    void availabilityChanged(bool available);
    void recoveryRequired(const QString &message, bool backupAvailable);
    void rulesError(const QString &message);

private:
    using Mutation = std::function<NoteRuleError(NoteRuleSnapshot &)>;

    std::unique_ptr<FileNoteRuleStore> store_;
    NoteRuleSnapshot                   snapshot_;
    QString                            lastError_;
    bool                               available_ { false };
    bool                               needsRecovery_ { false };

    NoteRuleError        mutate(const Mutation &mutation);
    NoteRuleError        replaceWith(NoteRuleSnapshot snapshot);
    bool                 loadCurrentStore(QString *errorText = nullptr);
    void                 becomeUnavailable(const NoteRuleError &error, bool clearProjection);
    void                 notifyRulesChanged();
    static void          normalizeRuleOrder(NoteRuleSnapshot *snapshot);
    static void          pruneMarkers(NoteRuleSnapshot *snapshot);
    static bool          sameSnapshot(const NoteRuleSnapshot &left, const NoteRuleSnapshot &right);
    static NoteRuleError unavailableError(const QString &message);
};

} // namespace QtNote

#endif // NOTERULEMANAGER_H
