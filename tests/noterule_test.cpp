#include "noterule.h"

#include <QtTest>

using namespace AnyKeep;

namespace {

NoteRule makeRule(qint64 order)
{
    NoteRule rule;
    rule.id         = QUuid::createUuid();
    rule.sortOrder  = order;
    rule.name       = QStringLiteral("Test rule");
    rule.revision   = 1;
    rule.modifiedAt = QDateTime::currentDateTimeUtc();
    return rule;
}

NoteRuleAction folderAction(const QUuid &folderId)
{
    NoteRuleAction action;
    action.kind     = NoteRuleActionKind::AssignFolder;
    action.folderId = folderId;
    return action;
}

NoteRuleAction storageAction(const QString &storageId)
{
    NoteRuleAction action;
    action.kind      = NoteRuleActionKind::SelectStorage;
    action.storageId = storageId;
    return action;
}

} // namespace

class NoteRuleTest : public QObject {
    Q_OBJECT

private slots:
    void appliesActionsInOrderAndStops();
    void defersTextRuleBeforeLaterSummaryRule();
    void supportsAnyAndNegatedConditions();
    void acceptsOptionalTagMarkerInCondition();
    void rejectsAmbiguousRuleActions();
    void fingerprintsSemanticInputDeterministically();
};

void NoteRuleTest::appliesActionsInOrderAndStops()
{
    const auto firstFolder  = QUuid::createUuid();
    const auto secondFolder = QUuid::createUuid();
    auto       first        = makeRule(10);
    first.conditions        = { { NoteRuleConditionKind::TitleMatches, QStringLiteral("Work*"), false } };
    first.actions           = { folderAction(firstFolder) };

    auto second           = makeRule(20);
    second.conditions     = { { NoteRuleConditionKind::HasTag, QStringLiteral("urgent"), false } };
    second.actions        = { storageAction(QStringLiteral("ptf")), folderAction(secondFolder) };
    second.stopProcessing = true;

    NoteRuleEvaluationInput input;
    input.storageId = QStringLiteral("tomboy");
    input.noteId    = QStringLiteral("note-1");
    input.title     = QStringLiteral("Work planning");
    input.tags      = { QStringLiteral("Urgent") };

    const auto result = NoteRuleEvaluator::evaluate({ second, first }, input);
    QVERIFY2(result, qPrintable(result.error.message));
    const QList<QUuid> expectedMatches { first.id, second.id };
    QCOMPARE(result.matchedRuleIds, expectedMatches);
    QVERIFY(result.folderId.has_value());
    QCOMPARE(*result.folderId, secondFolder);
    QCOMPARE(result.storageId, QStringLiteral("ptf"));
    QVERIFY(result.stopped);
}

void NoteRuleTest::defersTextRuleBeforeLaterSummaryRule()
{
    auto first           = makeRule(10);
    first.conditions     = { { NoteRuleConditionKind::TextContains, QStringLiteral("confidential"), false } };
    first.actions        = { storageAction(QStringLiteral("ptf")) };
    first.stopProcessing = true;

    auto second    = makeRule(20);
    second.actions = { storageAction(QStringLiteral("nextcloud")) };

    NoteRuleEvaluationInput summary;
    summary.storageId     = QStringLiteral("tomboy");
    summary.noteId        = QStringLiteral("note-1");
    summary.textAvailable = false;
    const auto deferred   = NoteRuleEvaluator::evaluate({ first, second }, summary);
    QVERIFY2(deferred, qPrintable(deferred.error.message));
    QVERIFY(deferred.requiresText);
    QVERIFY(deferred.matchedRuleIds.isEmpty());
    QVERIFY(deferred.storageId.isEmpty());

    summary.text          = QStringLiteral("This is not confidential");
    summary.textAvailable = true;
    const auto evaluated  = NoteRuleEvaluator::evaluate({ first, second }, summary);
    QVERIFY2(evaluated, qPrintable(evaluated.error.message));
    QCOMPARE(evaluated.storageId, QStringLiteral("ptf"));
    QVERIFY(evaluated.stopped);
}

void NoteRuleTest::supportsAnyAndNegatedConditions()
{
    const auto target      = QUuid::createUuid();
    auto       rule        = makeRule(0);
    rule.conditionCombiner = NoteRuleConditionCombiner::Any;
    rule.conditions        = {
        { NoteRuleConditionKind::TitleMatches, QStringLiteral("Invoice*"), false },
        { NoteRuleConditionKind::HasTag, QStringLiteral("archived"), true },
    };
    rule.actions = { folderAction(target) };

    NoteRuleEvaluationInput input;
    input.title       = QStringLiteral("Meeting notes");
    input.tags        = { QStringLiteral("work") };
    const auto result = NoteRuleEvaluator::evaluate({ rule }, input);
    QVERIFY2(result, qPrintable(result.error.message));
    QVERIFY(result.folderId.has_value());
    QCOMPARE(*result.folderId, target);

    input.tags           = { QStringLiteral("archived") };
    const auto unmatched = NoteRuleEvaluator::evaluate({ rule }, input);
    QVERIFY2(unmatched, qPrintable(unmatched.error.message));
    QVERIFY(!unmatched.folderId.has_value());
}

void NoteRuleTest::acceptsOptionalTagMarkerInCondition()
{
    auto rule       = makeRule(10);
    rule.conditions = { { NoteRuleConditionKind::HasTag, QStringLiteral("*tb"), false } };
    rule.actions    = { storageAction(QStringLiteral("tomboy")) };

    NoteRuleEvaluationInput input;
    input.storageId = QStringLiteral("ptf");
    input.tags      = { QStringLiteral("tb") };

    const auto result = NoteRuleEvaluator::evaluate({ rule }, input);
    QVERIFY2(result, qPrintable(result.error.message));
    QCOMPARE(result.matchedRuleIds, QList<QUuid> { rule.id });
    QCOMPARE(result.storageId, QStringLiteral("tomboy"));
}

void NoteRuleTest::rejectsAmbiguousRuleActions()
{
    auto rule             = makeRule(0);
    rule.actions          = { folderAction(QUuid::createUuid()), folderAction(QUuid::createUuid()) };
    const auto validation = NoteRuleEvaluator::validate(rule);
    QVERIFY(validation);
    QCOMPARE(validation.code, NoteRuleError::InvalidArgument);
}

void NoteRuleTest::fingerprintsSemanticInputDeterministically()
{
    NoteRuleEvaluationInput first;
    first.storageId     = QStringLiteral("ptf");
    first.noteId        = QStringLiteral("note-1");
    first.title         = QStringLiteral("Title");
    first.tags          = { QStringLiteral("one"), QStringLiteral("two") };
    first.text          = QStringLiteral("Sensitive content");
    first.textAvailable = true;

    auto reordered = first;
    reordered.tags = { QStringLiteral("two"), QStringLiteral("one") };
    QCOMPARE(NoteRuleEvaluator::inputFingerprint(first), NoteRuleEvaluator::inputFingerprint(reordered));

    reordered.text = QStringLiteral("Changed content");
    QVERIFY(NoteRuleEvaluator::inputFingerprint(first) != NoteRuleEvaluator::inputFingerprint(reordered));
}

QTEST_GUILESS_MAIN(NoteRuleTest)
#include "noterule_test.moc"
