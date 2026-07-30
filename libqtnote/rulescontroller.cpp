#include "rulescontroller.h"

#include "foldercatalogmanager.h"
#include "notemanager.h"
#include "noterulemanager.h"
#include "notestorage.h"

#include <algorithm>

namespace QtNote {
namespace {

    QString folderDisplayName(const QString &name, int depth)
    {
        return QString(qMax(0, depth) * 2, QLatin1Char(' ')) + name;
    }

    void appendFolderChoices(const FolderCatalog &catalog, const QUuid &parentId, int depth, QVariantList *choices)
    {
        if (!choices)
            return;
        for (const auto &folder : catalog.children(parentId)) {
            choices->append(QVariantMap {
                { QStringLiteral("id"), folder.id.toString(QUuid::WithoutBraces) },
                { QStringLiteral("label"), folderDisplayName(folder.name, depth) },
                { QStringLiteral("archived"), folder.archived },
                { QStringLiteral("favorite"), folder.favorite },
            });
            appendFolderChoices(catalog, folder.id, depth + 1, choices);
        }
    }

    QString summaryForRule(const NoteRule &rule)
    {
        QStringList conditionText;
        for (const auto &condition : rule.conditions) {
            QString label;
            switch (condition.kind) {
            case NoteRuleConditionKind::TitleMatches:
                label = QObject::tr("title matches %1").arg(condition.value);
                break;
            case NoteRuleConditionKind::HasTag:
                label = QObject::tr("has tag %1").arg(condition.value);
                break;
            case NoteRuleConditionKind::TextContains:
                label = QObject::tr("text contains %1").arg(condition.value);
                break;
            case NoteRuleConditionKind::StorageIs:
                label = QObject::tr("storage is %1").arg(condition.value);
                break;
            }
            conditionText.append(condition.negated ? QObject::tr("not %1").arg(label) : label);
        }
        const auto joiner
            = rule.conditionCombiner == NoteRuleConditionCombiner::All ? QObject::tr(" and ") : QObject::tr(" or ");
        QStringList actionText;
        for (const auto &action : rule.actions) {
            switch (action.kind) {
            case NoteRuleActionKind::AssignFolder:
                actionText.append(action.folderId.isNull() ? QObject::tr("assign Unsorted")
                                                           : QObject::tr("assign a folder"));
                break;
            case NoteRuleActionKind::SelectStorage:
                actionText.append(QObject::tr("save to %1").arg(action.storageId));
                break;
            case NoteRuleActionKind::RequireEncryption:
                actionText.append(QObject::tr("require encryption (planned)"));
                break;
            }
        }
        const auto when = conditionText.isEmpty() ? QObject::tr("always") : conditionText.join(joiner);
        return QObject::tr("When %1: %2").arg(when, actionText.join(QObject::tr(", ")));
    }

} // namespace

RulesController::RulesController(NoteRuleManager *ruleManager, FolderCatalogManager *folderCatalogManager,
                                 NoteManager *noteManager, QObject *parent) :
    QAbstractListModel(parent), ruleManager_(ruleManager ? ruleManager : NoteRuleManager::instance()),
    folderCatalogManager_(folderCatalogManager ? folderCatalogManager : FolderCatalogManager::instance()),
    noteManager_(noteManager ? noteManager : NoteManager::instance())
{
    connect(ruleManager_, &NoteRuleManager::rulesChanged, this, &RulesController::resetModel);
    connect(ruleManager_, &NoteRuleManager::availabilityChanged, this, [this](bool) {
        emit availableChanged();
        resetModel();
    });
    connect(ruleManager_, &NoteRuleManager::rulesError, this, [this](const QString &message) { setError(message); });
    connect(folderCatalogManager_, &FolderCatalogManager::catalogChanged, this, &RulesController::choicesChanged);
    connect(folderCatalogManager_, &FolderCatalogManager::availabilityChanged, this,
            [this](bool) { emit choicesChanged(); });
    connect(noteManager_, &NoteManager::storageAdded, this,
            [this](const NoteStorage::Ptr &) { emit choicesChanged(); });
    connect(noteManager_, &NoteManager::storageRemoved, this,
            [this](const NoteStorage::Ptr &) { emit choicesChanged(); });
    connect(noteManager_, &NoteManager::storageChanged, this,
            [this](const NoteStorage::Ptr &) { emit choicesChanged(); });
    connect(noteManager_, &NoteManager::storageOrderChanged, this, &RulesController::choicesChanged);
}

int RulesController::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : orderedRules().size(); }

QVariant RulesController::data(const QModelIndex &index, int role) const
{
    const auto rules = orderedRules();
    if (!index.isValid() || index.row() < 0 || index.row() >= rules.size())
        return {};
    const auto &rule = rules.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return rule.name;
    case RuleIdRole:
        return rule.id.toString(QUuid::WithoutBraces);
    case EnabledRole:
        return rule.enabled;
    case SummaryRole:
        return summaryForRule(rule);
    case StopProcessingRole:
        return rule.stopProcessing;
    default:
        return {};
    }
}

QHash<int, QByteArray> RulesController::roleNames() const
{
    return {
        { RuleIdRole, "ruleId" },
        { NameRole, "name" },
        { EnabledRole, "enabled" },
        { SummaryRole, "summary" },
        { StopProcessingRole, "stopProcessing" },
    };
}

bool RulesController::available() const { return ruleManager_ && ruleManager_->isAvailable(); }

QVariantList RulesController::folderChoices() const
{
    QVariantList result;
    result.append(QVariantMap {
        { QStringLiteral("id"), QString() },
        { QStringLiteral("label"), tr("Unsorted") },
        { QStringLiteral("archived"), false },
        { QStringLiteral("favorite"), false },
    });
    if (folderCatalogManager_ && folderCatalogManager_->isAvailable())
        appendFolderChoices(folderCatalogManager_->catalog(), {}, 0, &result);
    return result;
}

QVariantList RulesController::storageChoices() const
{
    QVariantList result;
    if (!noteManager_)
        return result;
    for (const auto &storage : noteManager_->prioritizedStorages(true)) {
        if (!storage)
            continue;
        result.append(QVariantMap {
            { QStringLiteral("id"), storage->systemName() },
            { QStringLiteral("label"), storage->name() },
            { QStringLiteral("accessible"), storage->isAccessible() },
        });
    }
    return result;
}

QString RulesController::createRule()
{
    if (!available()) {
        setError(ruleManager_ ? ruleManager_->lastError() : tr("The rule store is unavailable"));
        return {};
    }
    NoteRule rule;
    rule.enabled      = true;
    rule.name         = tr("New rule");
    rule.conditions   = { { NoteRuleConditionKind::TitleMatches, QStringLiteral("*"), false } };
    rule.actions      = { { NoteRuleActionKind::AssignFolder, {}, {} } };
    const auto result = ruleManager_->addRule(std::move(rule));
    if (!result) {
        setError(result.error.message);
        return {};
    }
    setError({});
    return result.value.toString(QUuid::WithoutBraces);
}

bool RulesController::removeRule(const QString &ruleId)
{
    const auto result = ruleManager_->removeRule(QUuid(ruleId));
    setError(result.message);
    return !result;
}

bool RulesController::moveRule(int sourceRow, int destinationRow)
{
    const auto rules = orderedRules();
    if (sourceRow < 0 || sourceRow >= rules.size() || destinationRow < 0 || destinationRow >= rules.size()
        || sourceRow == destinationRow) {
        return false;
    }
    const auto  beforeIndex = destinationRow > sourceRow ? destinationRow + 1 : destinationRow;
    const QUuid beforeId    = beforeIndex >= rules.size() ? QUuid {} : rules.at(beforeIndex).id;
    const auto  result      = ruleManager_->moveRuleRelative(rules.at(sourceRow).id, beforeId);
    setError(result.message);
    return !result;
}

bool RulesController::setRuleEnabled(const QString &ruleId, bool enabled)
{
    const auto result = ruleManager_->setRuleEnabled(QUuid(ruleId), enabled);
    setError(result.message);
    return !result;
}

QVariantMap RulesController::ruleDetails(const QString &ruleId) const
{
    if (!ruleManager_)
        return {};
    const auto *rule = ruleManager_->rule(QUuid(ruleId));
    if (!rule)
        return {};
    QVariantList conditions;
    conditions.reserve(rule->conditions.size());
    for (const auto &condition : rule->conditions)
        conditions.append(conditionMap(condition));
    QVariantList actions;
    actions.reserve(rule->actions.size());
    for (const auto &action : rule->actions)
        actions.append(actionMap(action));
    return {
        { QStringLiteral("id"), rule->id.toString(QUuid::WithoutBraces) },
        { QStringLiteral("name"), rule->name },
        { QStringLiteral("enabled"), rule->enabled },
        { QStringLiteral("conditionCombiner"), int(rule->conditionCombiner) },
        { QStringLiteral("conditions"), conditions },
        { QStringLiteral("actions"), actions },
        { QStringLiteral("stopProcessing"), rule->stopProcessing },
        { QStringLiteral("summary"), summaryForRule(*rule) },
    };
}

bool RulesController::updateRule(const QString &ruleId, const QString &name, int conditionCombiner,
                                 const QVariantList &conditions, const QVariantList &actions, bool stopProcessing)
{
    setError({});
    if (!ruleManager_) {
        setError(tr("The rule store is unavailable"));
        return false;
    }
    const auto *existing = ruleManager_->rule(QUuid(ruleId));
    if (!existing) {
        setError(tr("The rule was not found"));
        return false;
    }

    NoteRule updated          = *existing;
    updated.name              = name;
    updated.conditionCombiner = NoteRuleConditionCombiner(conditionCombiner);
    updated.stopProcessing    = stopProcessing;
    updated.conditions.clear();
    updated.actions.clear();
    for (const auto &value : conditions) {
        NoteRuleCondition condition;
        QString           error;
        if (!conditionFromMap(value, &condition, &error)) {
            setError(error);
            return false;
        }
        updated.conditions.append(std::move(condition));
    }
    for (const auto &value : actions) {
        NoteRuleAction action;
        QString        error;
        if (!actionFromMap(value, &action, &error)) {
            setError(error);
            return false;
        }
        if (action.kind == NoteRuleActionKind::AssignFolder && !checkFolderTarget(action.folderId)) {
            return false;
        }
        updated.actions.append(std::move(action));
    }
    const auto result = ruleManager_->updateRule(std::move(updated));
    setError(result.message);
    return !result;
}

QString RulesController::conditionLabel(int kind) const
{
    switch (NoteRuleConditionKind(kind)) {
    case NoteRuleConditionKind::TitleMatches:
        return tr("Title matches pattern");
    case NoteRuleConditionKind::HasTag:
        return tr("Has tag");
    case NoteRuleConditionKind::TextContains:
        return tr("Text contains");
    case NoteRuleConditionKind::StorageIs:
        return tr("Storage is");
    }
    return tr("Unknown condition");
}

QString RulesController::actionLabel(int kind) const
{
    switch (NoteRuleActionKind(kind)) {
    case NoteRuleActionKind::AssignFolder:
        return tr("Assign folder");
    case NoteRuleActionKind::SelectStorage:
        return tr("Save to storage");
    case NoteRuleActionKind::RequireEncryption:
        return tr("Require encryption (planned)");
    }
    return tr("Unknown action");
}

QList<NoteRule> RulesController::orderedRules() const
{
    return ruleManager_ ? ruleManager_->rules() : QList<NoteRule> {};
}

void RulesController::resetModel()
{
    beginResetModel();
    endResetModel();
}

void RulesController::setError(const QString &message)
{
    if (errorString_ == message)
        return;
    errorString_ = message;
    emit errorStringChanged();
}

bool RulesController::checkFolderTarget(const QUuid &folderId)
{
    if (folderId.isNull())
        return true;
    if (!folderCatalogManager_ || !folderCatalogManager_->isAvailable()) {
        setError(tr("The folder catalog is unavailable"));
        return false;
    }
    if (!folderCatalogManager_->catalog().folder(folderId)) {
        setError(tr("The selected folder no longer exists"));
        return false;
    }
    return true;
}

QVariantMap RulesController::conditionMap(const NoteRuleCondition &condition)
{
    return {
        { QStringLiteral("kind"), int(condition.kind) },
        { QStringLiteral("value"), condition.value },
        { QStringLiteral("negated"), condition.negated },
    };
}

QVariantMap RulesController::actionMap(const NoteRuleAction &action)
{
    return {
        { QStringLiteral("kind"), int(action.kind) },
        { QStringLiteral("folderId"), action.folderId.toString(QUuid::WithoutBraces) },
        { QStringLiteral("storageId"), action.storageId },
    };
}

bool RulesController::conditionFromMap(const QVariant &value, NoteRuleCondition *condition, QString *error)
{
    if (!condition) {
        if (error)
            *error = tr("A rule condition could not be read");
        return false;
    }
    const auto map = value.toMap();
    if (map.isEmpty()) {
        if (error)
            *error = tr("A rule condition is invalid");
        return false;
    }
    condition->kind    = NoteRuleConditionKind(map.value(QStringLiteral("kind")).toInt());
    condition->value   = map.value(QStringLiteral("value")).toString();
    condition->negated = map.value(QStringLiteral("negated")).toBool();
    return true;
}

bool RulesController::actionFromMap(const QVariant &value, NoteRuleAction *action, QString *error)
{
    if (!action) {
        if (error)
            *error = tr("A rule action could not be read");
        return false;
    }
    const auto map = value.toMap();
    if (map.isEmpty()) {
        if (error)
            *error = tr("A rule action is invalid");
        return false;
    }
    action->kind      = NoteRuleActionKind(map.value(QStringLiteral("kind")).toInt());
    action->folderId  = QUuid(map.value(QStringLiteral("folderId")).toString());
    action->storageId = map.value(QStringLiteral("storageId")).toString();
    return true;
}

} // namespace QtNote
