#include "noterulemanager.h"

#include "filenoterulestore.h"
#include "localdatakeystore.h"
#include "utils.h"

#include <QCoreApplication>

#include <algorithm>

namespace AnyKeep {
namespace {

    QString markerKey(const QUuid &ruleId, const QString &storageId, const QString &noteId)
    {
        return ruleId.toString(QUuid::WithoutBraces) + QLatin1Char('\0') + storageId + QLatin1Char('\0') + noteId;
    }

    bool ruleLess(const NoteRule &left, const NoteRule &right)
    {
        if (left.sortOrder != right.sortOrder)
            return left.sortOrder < right.sortOrder;
        return left.id.toString(QUuid::WithoutBraces) < right.id.toString(QUuid::WithoutBraces);
    }

    bool sameCondition(const NoteRuleCondition &left, const NoteRuleCondition &right)
    {
        return left.kind == right.kind && left.value == right.value && left.negated == right.negated;
    }

    bool sameAction(const NoteRuleAction &left, const NoteRuleAction &right)
    {
        return left.kind == right.kind && left.folderId == right.folderId && left.storageId == right.storageId;
    }

    bool sameRule(const NoteRule &left, const NoteRule &right)
    {
        if (left.id != right.id || left.enabled != right.enabled || left.sortOrder != right.sortOrder
            || left.name != right.name || left.conditionCombiner != right.conditionCombiner
            || left.stopProcessing != right.stopProcessing || left.revision != right.revision
            || left.modifiedAt != right.modifiedAt || left.conditions.size() != right.conditions.size()
            || left.actions.size() != right.actions.size()) {
            return false;
        }
        for (qsizetype index = 0; index < left.conditions.size(); ++index) {
            if (!sameCondition(left.conditions.at(index), right.conditions.at(index)))
                return false;
        }
        for (qsizetype index = 0; index < left.actions.size(); ++index) {
            if (!sameAction(left.actions.at(index), right.actions.at(index)))
                return false;
        }
        return true;
    }

    bool sameMarker(const NoteRuleApplicationMarker &left, const NoteRuleApplicationMarker &right)
    {
        return left.ruleId == right.ruleId && left.ruleRevision == right.ruleRevision
            && left.storageId == right.storageId && left.noteId == right.noteId
            && left.inputFingerprint == right.inputFingerprint && left.appliedAt == right.appliedAt;
    }

} // namespace

NoteRuleManager::NoteRuleManager(QObject *parent) : QObject(parent) { }

NoteRuleManager::NoteRuleManager(std::unique_ptr<FileNoteRuleStore> store, QObject *parent) :
    QObject(parent), store_(std::move(store))
{
}

NoteRuleManager::~NoteRuleManager() = default;

NoteRuleManager *NoteRuleManager::instance()
{
    static auto *manager = new NoteRuleManager(QCoreApplication::instance());
    return manager;
}

bool NoteRuleManager::initialize(QString *errorText)
{
    if (available_) {
        if (errorText)
            errorText->clear();
        return true;
    }

    if (!store_) {
        QString keyError;
        auto    key = LocalDataKeyStore::loadOrCreateMasterKey(&keyError);
        if (key.isEmpty()) {
            becomeUnavailable({ NoteRuleError::Locked, keyError }, true);
            if (errorText)
                *errorText = lastError_;
            return false;
        }
        store_ = std::make_unique<FileNoteRuleStore>(Utils::anykeepDataDir() + QStringLiteral("/note-rules.bin"),
                                                     std::move(key));
    }
    return loadCurrentStore(errorText);
}

bool NoteRuleManager::hasRecoveryBackup() const { return store_ && store_->hasBackup(); }

QList<NoteRule> NoteRuleManager::rules() const
{
    auto ordered = snapshot_.rules;
    std::sort(ordered.begin(), ordered.end(), ruleLess);
    return ordered;
}

const NoteRule *NoteRuleManager::rule(const QUuid &id) const
{
    for (const auto &candidate : snapshot_.rules) {
        if (candidate.id == id)
            return &candidate;
    }
    return nullptr;
}

NoteRuleResult<QUuid> NoteRuleManager::addRule(NoteRule rule)
{
    if (!available_)
        return { {}, unavailableError(lastError_) };
    if (rule.id.isNull())
        rule.id = QUuid::createUuid();
    if (this->rule(rule.id))
        return { {}, { NoteRuleError::AlreadyExists, tr("A rule with this ID already exists") } };
    rule.name       = rule.name.trimmed();
    rule.name       = rule.name.isEmpty() ? tr("New rule") : rule.name;
    rule.revision   = std::max<quint64>(rule.revision, 1);
    rule.modifiedAt = QDateTime::currentDateTimeUtc();
    if (rule.sortOrder == 0 && !snapshot_.rules.isEmpty()) {
        const auto ordered = rules();
        rule.sortOrder     = ordered.constLast().sortOrder + 1024;
    }
    if (const auto validation = NoteRuleEvaluator::validate(rule))
        return { {}, validation };
    const auto id     = rule.id;
    const auto result = mutate([rule = std::move(rule)](NoteRuleSnapshot &snapshot) mutable {
        snapshot.rules.append(std::move(rule));
        std::sort(snapshot.rules.begin(), snapshot.rules.end(), ruleLess);
        NoteRuleManager::normalizeRuleOrder(&snapshot);
        return NoteRuleError {};
    });
    return result ? NoteRuleResult<QUuid> { {}, result } : NoteRuleResult<QUuid> { id, {} };
}

NoteRuleError NoteRuleManager::updateRule(NoteRule rule)
{
    if (!available_)
        return unavailableError(lastError_);
    if (rule.id.isNull())
        return { NoteRuleError::InvalidArgument, tr("A rule ID is required") };
    const auto *existing = this->rule(rule.id);
    if (!existing)
        return { NoteRuleError::NotFound, tr("The rule was not found") };
    rule.name       = rule.name.trimmed();
    rule.name       = rule.name.isEmpty() ? tr("New rule") : rule.name;
    rule.sortOrder  = existing->sortOrder;
    rule.revision   = existing->revision + 1;
    rule.modifiedAt = QDateTime::currentDateTimeUtc();
    if (const auto validation = NoteRuleEvaluator::validate(rule))
        return validation;

    return mutate([rule = std::move(rule)](NoteRuleSnapshot &snapshot) mutable {
        for (auto &candidate : snapshot.rules) {
            if (candidate.id != rule.id)
                continue;
            candidate = std::move(rule);
            break;
        }
        NoteRuleManager::pruneMarkers(&snapshot);
        return NoteRuleError {};
    });
}

NoteRuleError NoteRuleManager::removeRule(const QUuid &id)
{
    if (!available_)
        return unavailableError(lastError_);
    if (id.isNull())
        return { NoteRuleError::InvalidArgument, tr("A rule ID is required") };
    if (!rule(id))
        return { NoteRuleError::NotFound, tr("The rule was not found") };
    return mutate([id](NoteRuleSnapshot &snapshot) {
        std::sort(snapshot.rules.begin(), snapshot.rules.end(), ruleLess);
        snapshot.rules.erase(std::remove_if(snapshot.rules.begin(), snapshot.rules.end(),
                                            [id](const NoteRule &candidate) { return candidate.id == id; }),
                             snapshot.rules.end());
        NoteRuleManager::pruneMarkers(&snapshot);
        NoteRuleManager::normalizeRuleOrder(&snapshot);
        return NoteRuleError {};
    });
}

NoteRuleError NoteRuleManager::moveRuleRelative(const QUuid &id, const QUuid &beforeId)
{
    if (!available_)
        return unavailableError(lastError_);
    if (id.isNull())
        return { NoteRuleError::InvalidArgument, tr("A rule ID is required") };
    if (!rule(id))
        return { NoteRuleError::NotFound, tr("The rule was not found") };
    if (!beforeId.isNull() && !rule(beforeId))
        return { NoteRuleError::NotFound, tr("The insertion rule was not found") };
    if (id == beforeId)
        return {};

    return mutate([id, beforeId](NoteRuleSnapshot &snapshot) {
        std::sort(snapshot.rules.begin(), snapshot.rules.end(), ruleLess);
        const auto source = std::find_if(snapshot.rules.begin(), snapshot.rules.end(),
                                         [id](const NoteRule &candidate) { return candidate.id == id; });
        NoteRule   moved  = *source;
        snapshot.rules.erase(source);
        auto destination = snapshot.rules.end();
        if (!beforeId.isNull()) {
            destination = std::find_if(snapshot.rules.begin(), snapshot.rules.end(),
                                       [beforeId](const NoteRule &candidate) { return candidate.id == beforeId; });
        }
        snapshot.rules.insert(destination, std::move(moved));
        NoteRuleManager::normalizeRuleOrder(&snapshot);
        return NoteRuleError {};
    });
}

NoteRuleError NoteRuleManager::setRuleEnabled(const QUuid &id, bool enabled)
{
    if (!available_)
        return unavailableError(lastError_);
    const auto *existing = rule(id);
    if (!existing)
        return { NoteRuleError::NotFound, tr("The rule was not found") };
    if (existing->enabled == enabled)
        return {};
    auto updated    = *existing;
    updated.enabled = enabled;
    return updateRule(std::move(updated));
}

NoteRuleEvaluation NoteRuleManager::evaluate(const NoteRuleEvaluationInput &input) const
{
    if (!available_) {
        NoteRuleEvaluation output;
        output.error = unavailableError(lastError_);
        return output;
    }
    return NoteRuleEvaluator::evaluate(rules(), input);
}

bool NoteRuleManager::wasApplied(const QUuid &ruleId, const NoteRuleEvaluationInput &input) const
{
    const auto *current = rule(ruleId);
    if (!available_ || !current || input.storageId.isEmpty() || input.noteId.isEmpty())
        return false;
    const auto fingerprint = NoteRuleEvaluator::inputFingerprint(input);
    for (const auto &marker : snapshot_.markers) {
        if (marker.ruleId == ruleId && marker.ruleRevision == current->revision && marker.storageId == input.storageId
            && marker.noteId == input.noteId && marker.inputFingerprint == fingerprint) {
            return true;
        }
    }
    return false;
}

NoteRuleError NoteRuleManager::recordApplied(const QList<QUuid> &ruleIds, const NoteRuleEvaluationInput &input)
{
    return recordApplied({ { ruleIds, input } });
}

NoteRuleError NoteRuleManager::recordApplied(const QList<NoteRuleApplication> &applications)
{
    if (!available_)
        return unavailableError(lastError_);
    if (applications.isEmpty())
        return {};
    for (const auto &application : applications) {
        if (application.input.storageId.isEmpty() || application.input.noteId.isEmpty()) {
            return { NoteRuleError::InvalidArgument, tr("A storage and note ID are required for rule tracking") };
        }
    }
    return mutate([this, applications](NoteRuleSnapshot &snapshot) {
        const auto appliedAt = QDateTime::currentDateTimeUtc();
        for (const auto &application : applications) {
            if (application.ruleIds.isEmpty())
                continue;
            const auto fingerprint = NoteRuleEvaluator::inputFingerprint(application.input);
            for (const auto &id : application.ruleIds) {
                const auto current = std::find_if(snapshot.rules.cbegin(), snapshot.rules.cend(),
                                                  [&id](const NoteRule &rule) { return rule.id == id; });
                // A queued batch can race with an edit or deletion from the
                // settings page. The new rule revision needs no old marker.
                if (current == snapshot.rules.cend())
                    continue;
                const auto key = markerKey(id, application.input.storageId, application.input.noteId);
                auto found = std::find_if(snapshot.markers.begin(), snapshot.markers.end(), [&key](const auto &marker) {
                    return markerKey(marker.ruleId, marker.storageId, marker.noteId) == key;
                });
                NoteRuleApplicationMarker marker;
                marker.ruleId           = id;
                marker.ruleRevision     = current->revision;
                marker.storageId        = application.input.storageId;
                marker.noteId           = application.input.noteId;
                marker.inputFingerprint = fingerprint;
                marker.appliedAt        = appliedAt;
                if (found == snapshot.markers.end())
                    snapshot.markers.append(std::move(marker));
                else
                    *found = std::move(marker);
            }
        }
        NoteRuleManager::pruneMarkers(&snapshot);
        return NoteRuleError {};
    });
}

NoteRuleError NoteRuleManager::forgetApplied(const QString &storageId, const QString &noteId)
{
    if (!available_)
        return unavailableError(lastError_);
    if (storageId.isEmpty() || noteId.isEmpty())
        return { NoteRuleError::InvalidArgument, tr("A storage and note ID are required for rule tracking") };
    return mutate([storageId, noteId](NoteRuleSnapshot &snapshot) {
        snapshot.markers.erase(std::remove_if(snapshot.markers.begin(), snapshot.markers.end(),
                                              [&storageId, &noteId](const auto &marker) {
                                                  return marker.storageId == storageId && marker.noteId == noteId;
                                              }),
                               snapshot.markers.end());
        return NoteRuleError {};
    });
}

NoteRuleError NoteRuleManager::restoreBackup(QString *preservedPath)
{
    if (preservedPath)
        preservedPath->clear();
    if (!store_)
        return unavailableError(lastError_);
    if (const auto restoreError = store_->restoreBackup(preservedPath)) {
        becomeUnavailable(restoreError, restoreError.code == NoteRuleError::Corrupt);
        return restoreError;
    }
    return loadCurrentStore() ? NoteRuleError {} : unavailableError(lastError_);
}

NoteRuleError NoteRuleManager::recreate(QString *preservedPath)
{
    if (preservedPath)
        preservedPath->clear();
    if (!store_)
        return unavailableError(lastError_);
    if (const auto recreateError = store_->recreate(preservedPath)) {
        becomeUnavailable(recreateError, recreateError.code == NoteRuleError::Corrupt);
        return recreateError;
    }
    return loadCurrentStore() ? NoteRuleError {} : unavailableError(lastError_);
}

NoteRuleError NoteRuleManager::mutate(const Mutation &mutation)
{
    if (!available_)
        return unavailableError(lastError_);
    auto candidate = snapshot_;
    if (const auto mutationError = mutation(candidate))
        return mutationError;
    return replaceWith(std::move(candidate));
}

NoteRuleError NoteRuleManager::replaceWith(NoteRuleSnapshot snapshot)
{
    if (!available_)
        return unavailableError(lastError_);
    pruneMarkers(&snapshot);
    if (const auto validation = NoteRuleEvaluator::validate(snapshot))
        return validation;
    if (sameSnapshot(snapshot_, snapshot))
        return {};
    if (const auto saveError = store_->save(snapshot)) {
        becomeUnavailable(saveError, saveError.code == NoteRuleError::Corrupt);
        return saveError;
    }
    snapshot_ = std::move(snapshot);
    notifyRulesChanged();
    return {};
}

bool NoteRuleManager::loadCurrentStore(QString *errorText)
{
    const auto loaded = store_->load();
    if (!loaded) {
        becomeUnavailable(loaded.error, true);
        if (errorText)
            *errorText = lastError_;
        return false;
    }
    snapshot_      = loaded.value;
    available_     = true;
    needsRecovery_ = false;
    lastError_.clear();
    emit availabilityChanged(true);
    notifyRulesChanged();
    if (errorText)
        errorText->clear();
    return true;
}

void NoteRuleManager::becomeUnavailable(const NoteRuleError &error, bool clearProjection)
{
    const bool wasAvailable = available_;
    available_              = false;
    needsRecovery_          = error.code == NoteRuleError::Corrupt;
    lastError_              = error.message;
    if (clearProjection)
        snapshot_ = {};
    if (wasAvailable)
        emit availabilityChanged(false);
    if (needsRecovery_)
        emit recoveryRequired(lastError_, hasRecoveryBackup());
    else if (!lastError_.isEmpty())
        emit rulesError(lastError_);
    if (clearProjection)
        notifyRulesChanged();
}

void NoteRuleManager::notifyRulesChanged() { emit rulesChanged(); }

void NoteRuleManager::normalizeRuleOrder(NoteRuleSnapshot *snapshot)
{
    if (!snapshot)
        return;
    const auto now = QDateTime::currentDateTimeUtc();
    for (qsizetype index = 0; index < snapshot->rules.size(); ++index) {
        auto      &rule     = snapshot->rules[index];
        const auto newOrder = qint64(index) * 1024;
        if (rule.sortOrder == newOrder)
            continue;
        rule.sortOrder  = newOrder;
        rule.revision   = std::max<quint64>(rule.revision + 1, 1);
        rule.modifiedAt = now;
    }
}

void NoteRuleManager::pruneMarkers(NoteRuleSnapshot *snapshot)
{
    if (!snapshot)
        return;
    snapshot->markers.erase(
        std::remove_if(snapshot->markers.begin(), snapshot->markers.end(),
                       [snapshot](const auto &marker) {
                           const auto rule = std::find_if(
                               snapshot->rules.cbegin(), snapshot->rules.cend(),
                               [&marker](const NoteRule &candidate) { return candidate.id == marker.ruleId; });
                           return rule == snapshot->rules.cend() || rule->revision != marker.ruleRevision;
                       }),
        snapshot->markers.end());
}

bool NoteRuleManager::sameSnapshot(const NoteRuleSnapshot &left, const NoteRuleSnapshot &right)
{
    if (left.rules.size() != right.rules.size() || left.markers.size() != right.markers.size())
        return false;
    for (qsizetype index = 0; index < left.rules.size(); ++index) {
        if (!sameRule(left.rules.at(index), right.rules.at(index)))
            return false;
    }
    for (qsizetype index = 0; index < left.markers.size(); ++index) {
        if (!sameMarker(left.markers.at(index), right.markers.at(index)))
            return false;
    }
    return true;
}

NoteRuleError NoteRuleManager::unavailableError(const QString &message)
{
    return { NoteRuleError::Locked, message.isEmpty() ? tr("The rule store is unavailable") : message };
}

} // namespace AnyKeep
