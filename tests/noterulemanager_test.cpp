#include "filenoterulestore.h"
#include "noterulemanager.h"

#include "secureenvelope.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace AnyKeep;

namespace {

NoteRule makeRule(const QString &name, const QString &storageId)
{
    NoteRule rule;
    rule.name = name;
    NoteRuleAction action;
    action.kind      = NoteRuleActionKind::SelectStorage;
    action.storageId = storageId;
    rule.actions     = { action };
    return rule;
}

std::unique_ptr<FileNoteRuleStore> makeStore(const QString &path, const QByteArray &key)
{
    return std::make_unique<FileNoteRuleStore>(path, key);
}

} // namespace

class NoteRuleManagerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void persistsRelativeOrderAndInvalidatesMarkersOnEdit();
    void forgetsApplicationMarkersForExplicitReapply();
    void batchesMarkersForSeveralNotes();
};

void NoteRuleManagerTest::initTestCase() { QVERIFY2(FileNoteRuleStore::cryptoAvailable(), "AES-256-GCM unavailable"); }

void NoteRuleManagerTest::persistsRelativeOrderAndInvalidatesMarkersOnEdit()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("rules.bin"));
    const auto key  = SecureEnvelope::generateMasterKey();

    NoteRuleManager manager(makeStore(path, key));
    QVERIFY(manager.initialize());
    const auto first  = manager.addRule(makeRule(QStringLiteral("First"), QStringLiteral("ptf")));
    const auto second = manager.addRule(makeRule(QStringLiteral("Second"), QStringLiteral("nextcloud")));
    QVERIFY2(first, qPrintable(first.error.message));
    QVERIFY2(second, qPrintable(second.error.message));
    QVERIFY(!manager.moveRuleRelative(second.value, first.value));
    const auto ordered = manager.rules();
    QCOMPARE(ordered.size(), 2);
    QCOMPARE(ordered.constFirst().id, second.value);
    QCOMPARE(ordered.constLast().id, first.value);

    NoteRuleEvaluationInput input;
    input.storageId = QStringLiteral("tomboy");
    input.noteId    = QStringLiteral("note-1");
    input.title     = QStringLiteral("Rule input");
    QVERIFY(!manager.recordApplied({ second.value }, input));
    QVERIFY(manager.wasApplied(second.value, input));

    auto edited = *manager.rule(second.value);
    edited.name = QStringLiteral("Edited second");
    QVERIFY(!manager.updateRule(edited));
    QVERIFY(!manager.wasApplied(second.value, input));

    NoteRuleManager reloaded(makeStore(path, key));
    QVERIFY(reloaded.initialize());
    const auto reloadedRules = reloaded.rules();
    QCOMPARE(reloadedRules.size(), 2);
    QCOMPARE(reloadedRules.constFirst().id, second.value);
    QCOMPARE(reloadedRules.constFirst().name, QStringLiteral("Edited second"));
}

void NoteRuleManagerTest::forgetsApplicationMarkersForExplicitReapply()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    NoteRuleManager manager(
        makeStore(directory.filePath(QStringLiteral("rules.bin")), SecureEnvelope::generateMasterKey()));
    QVERIFY(manager.initialize());
    const auto added = manager.addRule(makeRule(QStringLiteral("Only"), QStringLiteral("ptf")));
    QVERIFY2(added, qPrintable(added.error.message));

    NoteRuleEvaluationInput input;
    input.storageId = QStringLiteral("tomboy");
    input.noteId    = QStringLiteral("note-1");
    QVERIFY(!manager.recordApplied({ added.value }, input));
    QVERIFY(manager.wasApplied(added.value, input));
    QVERIFY(!manager.forgetApplied(input.storageId, input.noteId));
    QVERIFY(!manager.wasApplied(added.value, input));
}

void NoteRuleManagerTest::batchesMarkersForSeveralNotes()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    NoteRuleManager manager(
        makeStore(directory.filePath(QStringLiteral("rules.bin")), SecureEnvelope::generateMasterKey()));
    QVERIFY(manager.initialize());
    const auto first  = manager.addRule(makeRule(QStringLiteral("First"), QStringLiteral("ptf")));
    const auto second = manager.addRule(makeRule(QStringLiteral("Second"), QStringLiteral("nextcloud")));
    QVERIFY(first);
    QVERIFY(second);

    NoteRuleEvaluationInput one;
    one.storageId = QStringLiteral("ptf");
    one.noteId    = QStringLiteral("one");
    one.title     = QStringLiteral("One");
    NoteRuleEvaluationInput two;
    two.storageId = QStringLiteral("tomboy");
    two.noteId    = QStringLiteral("two");
    two.title     = QStringLiteral("Two");
    QVERIFY(!manager.recordApplied({ { { first.value, second.value }, one }, { { second.value }, two } }));
    QVERIFY(manager.wasApplied(first.value, one));
    QVERIFY(manager.wasApplied(second.value, one));
    QVERIFY(manager.wasApplied(second.value, two));
    QVERIFY(!manager.wasApplied(first.value, two));
}

QTEST_GUILESS_MAIN(NoteRuleManagerTest)
#include "noterulemanager_test.moc"
