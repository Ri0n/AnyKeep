#include "filefoldercatalogstore.h"
#include "filenoterulestore.h"
#include "foldercatalogmanager.h"
#include "noterulemanager.h"
#include "rulescontroller.h"
#include "secureenvelope.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace AnyKeep;

namespace {

FolderRecord folder(const QString &name)
{
    FolderRecord result;
    result.id   = QUuid::createUuid();
    result.name = name;
    return result;
}

} // namespace

class RulesControllerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void editsAndReordersPersistentRules();
    void rejectsUnknownFolderTargets();
};

void RulesControllerTest::initTestCase()
{
    QVERIFY2(FileFolderCatalogStore::cryptoAvailable(), "AES-256-GCM unavailable");
    QVERIFY2(FileNoteRuleStore::cryptoAvailable(), "AES-256-GCM unavailable");
}

void RulesControllerTest::editsAndReordersPersistentRules()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto key = SecureEnvelope::generateMasterKey();

    FolderCatalogManager folders(
        std::make_unique<FileFolderCatalogStore>(directory.filePath(QStringLiteral("folders.bin")), key));
    QVERIFY(folders.initialize());
    const auto inbox = folders.addFolder(folder(QStringLiteral("Inbox")));
    QVERIFY2(inbox, qPrintable(inbox.error.message));

    NoteRuleManager rules(std::make_unique<FileNoteRuleStore>(directory.filePath(QStringLiteral("rules.bin")), key));
    QVERIFY(rules.initialize());
    RulesController controller(&rules, &folders);
    QCOMPARE(controller.rowCount(), 0);

    const auto firstId = controller.createRule();
    QVERIFY2(!firstId.isEmpty(), qPrintable(controller.errorString()));
    QCOMPARE(controller.rowCount(), 1);
    QVERIFY(controller.data(controller.index(0, 0), RulesController::EnabledRole).toBool());

    const QVariantList conditions {
        QVariantMap {
            { QStringLiteral("kind"), int(NoteRuleConditionKind::TitleMatches) },
            { QStringLiteral("value"), QStringLiteral("Invoice*") },
            { QStringLiteral("negated"), false },
        },
    };
    const QVariantList actions {
        QVariantMap {
            { QStringLiteral("kind"), int(NoteRuleActionKind::AssignFolder) },
            { QStringLiteral("folderId"), inbox.value.toString(QUuid::WithoutBraces) },
        },
    };
    QVERIFY2(controller.updateRule(firstId, QStringLiteral("Invoices"), int(NoteRuleConditionCombiner::All), conditions,
                                   actions, true),
             qPrintable(controller.errorString()));
    QVERIFY(controller.setRuleEnabled(firstId, true));

    const auto firstDetails = controller.ruleDetails(firstId);
    QCOMPARE(firstDetails.value(QStringLiteral("name")).toString(), QStringLiteral("Invoices"));
    QCOMPARE(firstDetails.value(QStringLiteral("conditions")).toList().size(), 1);
    QCOMPARE(firstDetails.value(QStringLiteral("actions")).toList().size(), 1);
    QCOMPARE(rules.rule(QUuid(firstId))->actions.constFirst().folderId, inbox.value);

    const auto secondId = controller.createRule();
    QVERIFY(!secondId.isEmpty());
    QVERIFY(controller.moveRule(1, 0));
    QCOMPARE(rules.rules().constFirst().id, QUuid(secondId));
    QCOMPARE(rules.rules().constLast().id, QUuid(firstId));
}

void RulesControllerTest::rejectsUnknownFolderTargets()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto key = SecureEnvelope::generateMasterKey();

    FolderCatalogManager folders(
        std::make_unique<FileFolderCatalogStore>(directory.filePath(QStringLiteral("folders.bin")), key));
    QVERIFY(folders.initialize());

    NoteRuleManager rules(std::make_unique<FileNoteRuleStore>(directory.filePath(QStringLiteral("rules.bin")), key));
    QVERIFY(rules.initialize());
    RulesController controller(&rules, &folders);
    const auto      ruleId = controller.createRule();
    QVERIFY(!ruleId.isEmpty());

    const QVariantList conditions {
        QVariantMap {
            { QStringLiteral("kind"), int(NoteRuleConditionKind::TitleMatches) },
            { QStringLiteral("value"), QStringLiteral("*") },
        },
    };
    const QVariantList actions {
        QVariantMap {
            { QStringLiteral("kind"), int(NoteRuleActionKind::AssignFolder) },
            { QStringLiteral("folderId"), QUuid::createUuid().toString(QUuid::WithoutBraces) },
        },
    };
    QVERIFY(!controller.updateRule(ruleId, QStringLiteral("Missing folder"), int(NoteRuleConditionCombiner::All),
                                   conditions, actions, false));
    QCOMPARE(controller.errorString(), QStringLiteral("The selected folder no longer exists"));
    QCOMPARE(rules.rule(QUuid(ruleId))->name, QStringLiteral("New rule"));
}

QTEST_GUILESS_MAIN(RulesControllerTest)
#include "rulescontroller_test.moc"
