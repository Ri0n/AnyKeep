#include "noterule.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace AnyKeep {
namespace {

    constexpr qsizetype MaximumConditionsPerRule = 32;
    constexpr qsizetype MaximumActionsPerRule    = 8;
    constexpr qsizetype MaximumPatternLength     = 512;
    constexpr int       FingerprintSize          = 32;

    NoteRuleError error(NoteRuleError::Code code, const QString &message) { return { code, message }; }

    QString normalizedTagCondition(QString value)
    {
        value = value.trimmed();
        if (value.startsWith(QLatin1Char('*')))
            value = value.mid(1).trimmed();
        return value;
    }

    enum class ConditionState { Matches, DoesNotMatch, NeedsText };

    bool validConditionKind(NoteRuleConditionKind kind)
    {
        switch (kind) {
        case NoteRuleConditionKind::TitleMatches:
        case NoteRuleConditionKind::HasTag:
        case NoteRuleConditionKind::TextContains:
        case NoteRuleConditionKind::StorageIs:
            return true;
        }
        return false;
    }

    bool validActionKind(NoteRuleActionKind kind)
    {
        switch (kind) {
        case NoteRuleActionKind::AssignFolder:
        case NoteRuleActionKind::SelectStorage:
        case NoteRuleActionKind::RequireEncryption:
            return true;
        }
        return false;
    }

    bool validCombiner(NoteRuleConditionCombiner combiner)
    {
        return combiner == NoteRuleConditionCombiner::All || combiner == NoteRuleConditionCombiner::Any;
    }

    ConditionState evaluateCondition(const NoteRuleCondition &condition, const NoteRuleEvaluationInput &input)
    {
        const auto value   = condition.value.trimmed();
        bool       matches = false;
        switch (condition.kind) {
        case NoteRuleConditionKind::TitleMatches: {
            const auto expression = QRegularExpression(QRegularExpression::wildcardToRegularExpression(value),
                                                       QRegularExpression::CaseInsensitiveOption);
            matches               = expression.isValid() && expression.match(input.title).hasMatch();
            break;
        }
        case NoteRuleConditionKind::HasTag: {
            const QString expectedTag = normalizedTagCondition(value);
            for (const auto &tag : input.tags) {
                if (tag.compare(expectedTag, Qt::CaseInsensitive) == 0) {
                    matches = true;
                    break;
                }
            }
            break;
        }
        case NoteRuleConditionKind::TextContains:
            if (!input.textAvailable)
                return ConditionState::NeedsText;
            matches = input.text.contains(value, Qt::CaseInsensitive);
            break;
        case NoteRuleConditionKind::StorageIs:
            matches = input.storageId.compare(value, Qt::CaseInsensitive) == 0;
            break;
        }
        if (condition.negated)
            matches = !matches;
        return matches ? ConditionState::Matches : ConditionState::DoesNotMatch;
    }

    ConditionState evaluateConditions(const NoteRule &rule, const NoteRuleEvaluationInput &input)
    {
        if (rule.conditions.isEmpty())
            return ConditionState::Matches;

        bool needsText = false;
        for (const auto &condition : rule.conditions) {
            const auto result = evaluateCondition(condition, input);
            if (rule.conditionCombiner == NoteRuleConditionCombiner::All) {
                if (result == ConditionState::DoesNotMatch)
                    return ConditionState::DoesNotMatch;
                needsText = needsText || result == ConditionState::NeedsText;
            } else {
                if (result == ConditionState::Matches)
                    return ConditionState::Matches;
                needsText = needsText || result == ConditionState::NeedsText;
            }
        }
        if (needsText)
            return ConditionState::NeedsText;
        return rule.conditionCombiner == NoteRuleConditionCombiner::All ? ConditionState::Matches
                                                                        : ConditionState::DoesNotMatch;
    }

    bool ruleLess(const NoteRule &left, const NoteRule &right)
    {
        if (left.sortOrder != right.sortOrder)
            return left.sortOrder < right.sortOrder;
        return left.id.toString(QUuid::WithoutBraces) < right.id.toString(QUuid::WithoutBraces);
    }

} // namespace

NoteRuleError NoteRuleEvaluator::validate(const NoteRule &rule)
{
    if (rule.id.isNull())
        return error(NoteRuleError::InvalidArgument, QStringLiteral("A rule ID is required"));
    if (!validCombiner(rule.conditionCombiner))
        return error(NoteRuleError::InvalidArgument, QStringLiteral("The rule condition combiner is invalid"));
    if (rule.conditions.size() > MaximumConditionsPerRule)
        return error(NoteRuleError::InvalidArgument, QStringLiteral("A rule has too many conditions"));
    if (rule.actions.isEmpty())
        return error(NoteRuleError::InvalidArgument, QStringLiteral("A rule needs at least one action"));
    if (rule.actions.size() > MaximumActionsPerRule)
        return error(NoteRuleError::InvalidArgument, QStringLiteral("A rule has too many actions"));
    if (rule.revision == 0)
        return error(NoteRuleError::InvalidArgument, QStringLiteral("A rule revision is required"));
    if (!rule.modifiedAt.isValid())
        return error(NoteRuleError::InvalidArgument, QStringLiteral("A rule modification time is required"));

    for (const auto &condition : rule.conditions) {
        const auto value = condition.value.trimmed();
        if (!validConditionKind(condition.kind) || value.isEmpty() || value.size() > MaximumPatternLength
            || (condition.kind == NoteRuleConditionKind::HasTag && normalizedTagCondition(value).isEmpty())) {
            return error(NoteRuleError::InvalidArgument, QStringLiteral("A rule condition is invalid"));
        }
        if (condition.kind == NoteRuleConditionKind::TitleMatches) {
            const auto expression = QRegularExpression(QRegularExpression::wildcardToRegularExpression(value));
            if (!expression.isValid())
                return error(NoteRuleError::InvalidArgument, QStringLiteral("A title pattern is invalid"));
        }
    }

    bool hasFolderAction     = false;
    bool hasStorageAction    = false;
    bool hasEncryptionAction = false;
    for (const auto &action : rule.actions) {
        if (!validActionKind(action.kind))
            return error(NoteRuleError::InvalidArgument, QStringLiteral("A rule action is invalid"));
        switch (action.kind) {
        case NoteRuleActionKind::AssignFolder:
            if (hasFolderAction)
                return error(NoteRuleError::InvalidArgument,
                             QStringLiteral("A rule cannot assign more than one folder"));
            hasFolderAction = true;
            break;
        case NoteRuleActionKind::SelectStorage:
            if (hasStorageAction || action.storageId.trimmed().isEmpty()) {
                return error(NoteRuleError::InvalidArgument, QStringLiteral("A rule storage action is invalid"));
            }
            hasStorageAction = true;
            break;
        case NoteRuleActionKind::RequireEncryption:
            if (hasEncryptionAction) {
                return error(NoteRuleError::InvalidArgument,
                             QStringLiteral("A rule cannot require encryption more than once"));
            }
            hasEncryptionAction = true;
            break;
        }
    }
    return {};
}

NoteRuleError NoteRuleEvaluator::validate(const NoteRuleSnapshot &snapshot)
{
    QSet<QUuid> ruleIds;
    for (const auto &rule : snapshot.rules) {
        if (const auto validation = validate(rule))
            return validation;
        if (ruleIds.contains(rule.id))
            return error(NoteRuleError::AlreadyExists, QStringLiteral("Duplicate rule ID"));
        ruleIds.insert(rule.id);
    }

    QSet<QString> markerKeys;
    for (const auto &marker : snapshot.markers) {
        if (marker.ruleId.isNull() || marker.ruleRevision == 0 || marker.storageId.isEmpty() || marker.noteId.isEmpty()
            || marker.inputFingerprint.size() != FingerprintSize || !marker.appliedAt.isValid()) {
            return error(NoteRuleError::Corrupt, QStringLiteral("An application marker is invalid"));
        }
        const auto key = marker.ruleId.toString(QUuid::WithoutBraces) + QLatin1Char('\0') + marker.storageId
            + QLatin1Char('\0') + marker.noteId;
        if (markerKeys.contains(key))
            return error(NoteRuleError::AlreadyExists, QStringLiteral("Duplicate application marker"));
        markerKeys.insert(key);
    }
    return {};
}

NoteRuleEvaluation NoteRuleEvaluator::evaluate(const QList<NoteRule> &rules, const NoteRuleEvaluationInput &input)
{
    NoteRuleEvaluation output;
    auto               orderedRules = rules;
    std::sort(orderedRules.begin(), orderedRules.end(), ruleLess);
    for (const auto &rule : orderedRules) {
        if (!rule.enabled)
            continue;
        if (const auto validation = validate(rule)) {
            output.error = validation;
            return output;
        }

        const auto conditionResult = evaluateConditions(rule, input);
        if (conditionResult == ConditionState::NeedsText) {
            output.requiresText = true;
            return output;
        }
        if (conditionResult != ConditionState::Matches)
            continue;

        output.matchedRuleIds.append(rule.id);
        for (const auto &action : rule.actions) {
            switch (action.kind) {
            case NoteRuleActionKind::AssignFolder:
                output.folderId = action.folderId;
                break;
            case NoteRuleActionKind::SelectStorage:
                output.storageId = action.storageId.trimmed();
                break;
            case NoteRuleActionKind::RequireEncryption:
                output.requiresEncryption = true;
                break;
            }
        }
        if (rule.stopProcessing) {
            output.stopped = true;
            return output;
        }
    }
    return output;
}

QByteArray NoteRuleEvaluator::inputFingerprint(const NoteRuleEvaluationInput &input)
{
    QByteArray  encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_5_10);
    auto tags = input.tags;
    std::sort(tags.begin(), tags.end(), [](const QString &left, const QString &right) {
        const auto insensitive = left.compare(right, Qt::CaseInsensitive);
        return insensitive != 0 ? insensitive < 0 : left < right;
    });
    stream << input.storageId << input.noteId << input.title << tags << input.textAvailable;
    if (input.textAvailable)
        stream << input.text;
    return QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
}

} // namespace AnyKeep
