#ifndef NOTERULE_H
#define NOTERULE_H

#include "anykeep_export.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <optional>

namespace AnyKeep {

/** Conditions supported by the first, deliberately bounded rule evaluator. */
enum class NoteRuleConditionKind : quint8 { TitleMatches, HasTag, TextContains, StorageIs };

/** Actions are declarative; RequireEncryption is retained for a later policy. */
enum class NoteRuleActionKind : quint8 { AssignFolder, SelectStorage, RequireEncryption };

enum class NoteRuleConditionCombiner : quint8 { All, Any };

struct ANYKEEP_EXPORT NoteRuleCondition {
    NoteRuleConditionKind kind { NoteRuleConditionKind::TitleMatches };
    QString               value;
    bool                  negated { false };
};

struct ANYKEEP_EXPORT NoteRuleAction {
    NoteRuleActionKind kind { NoteRuleActionKind::AssignFolder };
    /// A null ID explicitly means the virtual Unsorted folder.
    QUuid   folderId;
    QString storageId;
};

/** One ordered, local-only note routing rule. */
struct ANYKEEP_EXPORT NoteRule {
    QUuid                     id;
    bool                      enabled { true };
    qint64                    sortOrder { 0 };
    QString                   name;
    NoteRuleConditionCombiner conditionCombiner { NoteRuleConditionCombiner::All };
    QList<NoteRuleCondition>  conditions;
    QList<NoteRuleAction>     actions;
    bool                      stopProcessing { false };
    quint64                   revision { 0 };
    QDateTime                 modifiedAt;
};

/**
 * Persistent idempotence marker. It contains a SHA-256 input fingerprint,
 * never note body text, and is encrypted with the containing rule store.
 */
struct ANYKEEP_EXPORT NoteRuleApplicationMarker {
    QUuid      ruleId;
    quint64    ruleRevision { 0 };
    QString    storageId;
    QString    noteId;
    QByteArray inputFingerprint;
    QDateTime  appliedAt;
};

struct ANYKEEP_EXPORT NoteRuleSnapshot {
    QList<NoteRule>                  rules;
    QList<NoteRuleApplicationMarker> markers;
};

struct ANYKEEP_EXPORT NoteRuleError {
    enum Code { None, InvalidArgument, NotFound, AlreadyExists, Conflict, Io, CryptoUnavailable, Locked, Corrupt };

    Code    code { None };
    QString message;

    explicit operator bool() const { return code != None; }
};

template <typename T> struct NoteRuleResult {
    T             value;
    NoteRuleError error;

    explicit operator bool() const { return !error; }
};

/** Immutable input used by the pure evaluator. */
struct ANYKEEP_EXPORT NoteRuleEvaluationInput {
    QString     storageId;
    QString     noteId;
    QString     title;
    QStringList tags;
    QString     text;
    /** False for summaries; text-based rules are deferred rather than guessed. */
    bool textAvailable { false };
};

/** One or more rules recorded atomically for one evaluated note input. */
struct ANYKEEP_EXPORT NoteRuleApplication {
    QList<QUuid>            ruleIds;
    NoteRuleEvaluationInput input;
};

/**
 * A declarative result. Applying it is intentionally separate: the caller
 * can first move storage, then assign a folder, and record a marker only after
 * the underlying storage action has completed.
 */
struct ANYKEEP_EXPORT NoteRuleEvaluation {
    QList<QUuid>         matchedRuleIds;
    std::optional<QUuid> folderId;
    QString              storageId;
    bool                 requiresEncryption { false };
    bool                 stopped { false };
    bool                 requiresText { false };
    NoteRuleError        error;

    explicit operator bool() const { return !error; }
};

class ANYKEEP_EXPORT NoteRuleEvaluator final {
public:
    /** Checks structural validity without looking up a folder or storage. */
    static NoteRuleError validate(const NoteRule &rule);
    static NoteRuleError validate(const NoteRuleSnapshot &snapshot);

    /**
     * Evaluates rules in `sortOrder` order. A text condition on an unloaded
     * note suspends processing at that rule, so a possible earlier stop rule
     * cannot be bypassed by a later summary-only action.
     */
    static NoteRuleEvaluation evaluate(const QList<NoteRule> &rules, const NoteRuleEvaluationInput &input);

    /** Stable input marker for idempotence; no text is exposed in diagnostics. */
    static QByteArray inputFingerprint(const NoteRuleEvaluationInput &input);
};

} // namespace AnyKeep

#endif // NOTERULE_H
