#include "filefoldercatalogstore.h"
#include "filenoterulestore.h"
#include "foldercatalogmanager.h"
#include "noterulemanager.h"
#include "rulescontroller.h"
#include "secureenvelope.h"

#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWidget>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace AnyKeep;

class RulesPageTest : public QObject {
    Q_OBJECT

private slots:
    void loadsWithAnEmptyRuleStore();
    void loadsWithAnEditableRule();
    void savesPendingEditsThroughPublicMethod();
    void reordersRulesWithSharedAnimation();
};

namespace {

struct RuleEnvironment {
    QTemporaryDir        directory;
    QByteArray           key { SecureEnvelope::generateMasterKey() };
    FolderCatalogManager folders { std::make_unique<FileFolderCatalogStore>(
        directory.filePath(QStringLiteral("folders.bin")), key) };
    NoteRuleManager rules { std::make_unique<FileNoteRuleStore>(directory.filePath(QStringLiteral("rules.bin")), key) };

    RuleEnvironment()
    {
        Q_ASSERT(directory.isValid());
        const bool foldersInitialized = folders.initialize();
        const bool rulesInitialized   = rules.initialize();
        Q_ASSERT(foldersInitialized);
        Q_ASSERT(rulesInitialized);
        Q_UNUSED(foldersInitialized);
        Q_UNUSED(rulesInitialized);
    }
};

QQuickItem *quickItemByName(QQuickItem *root, const QString &objectName)
{
    if (!root)
        return nullptr;
    if (root->objectName() == objectName)
        return root;
    for (auto *child : root->childItems())
        if (auto *match = quickItemByName(child, objectName))
            return match;
    return nullptr;
}

void verifyPageLoads(RulesController *controller)
{
    QQuickWidget view;
    view.resize(760, 520);
    view.setResizeMode(QQuickWidget::SizeRootObjectToView);
    view.rootContext()->setContextProperty(QStringLiteral("rulesController"), controller);
    view.setSource(QUrl(QStringLiteral("qrc:/qml/RulesPage.qml")));
    QCOMPARE(view.status(), QQuickWidget::Ready);
    QVERIFY(view.rootObject());
    QCOMPARE(view.rootObject()->objectName(), QStringLiteral("rulesPage"));
}

} // namespace

void RulesPageTest::loadsWithAnEmptyRuleStore()
{
    QVERIFY2(FileFolderCatalogStore::cryptoAvailable(), "AES-256-GCM unavailable");
    QVERIFY2(FileNoteRuleStore::cryptoAvailable(), "AES-256-GCM unavailable");
    RuleEnvironment environment;
    RulesController controller(&environment.rules, &environment.folders);
    verifyPageLoads(&controller);
}

void RulesPageTest::loadsWithAnEditableRule()
{
    RuleEnvironment environment;
    RulesController controller(&environment.rules, &environment.folders);
    QVERIFY(!controller.createRule().isEmpty());
    verifyPageLoads(&controller);
}

void RulesPageTest::savesPendingEditsThroughPublicMethod()
{
    RuleEnvironment environment;
    FolderRecord    folder;
    folder.name            = QStringLiteral("Confidential");
    const auto addedFolder = environment.folders.addFolder(folder);
    QVERIFY2(addedFolder, qPrintable(addedFolder.error.message));

    RulesController controller(&environment.rules, &environment.folders);
    const auto      ruleId = controller.createRule();
    QVERIFY(!ruleId.isEmpty());

    QQuickWidget view;
    view.resize(760, 520);
    view.setResizeMode(QQuickWidget::SizeRootObjectToView);
    view.rootContext()->setContextProperty(QStringLiteral("rulesController"), &controller);
    view.setSource(QUrl(QStringLiteral("qrc:/qml/RulesPage.qml")));
    QCOMPARE(view.status(), QQuickWidget::Ready);
    auto *root = view.rootObject();
    QVERIFY(root);

    root->setProperty("selectedRuleId", ruleId);
    root->setProperty("editedName", QStringLiteral("Secret notes"));
    root->setProperty("editedConditions",
                      QVariantList {
                          QVariantMap {
                              { QStringLiteral("kind"), int(NoteRuleConditionKind::TitleMatches) },
                              { QStringLiteral("value"), QStringLiteral("*secret*") },
                              { QStringLiteral("negated"), false },
                          },
                      });
    root->setProperty("editedActions",
                      QVariantList {
                          QVariantMap {
                              { QStringLiteral("kind"), int(NoteRuleActionKind::AssignFolder) },
                              { QStringLiteral("folderId"), addedFolder.value.toString(QUuid::WithoutBraces) },
                              { QStringLiteral("storageId"), QString() },
                          },
                      });
    root->setProperty("dirty", true);

    QVariant saved;
    QVERIFY(QMetaObject::invokeMethod(root, "saveCurrent", Q_RETURN_ARG(QVariant, saved)));
    QVERIFY(saved.toBool());
    QVERIFY(!root->property("dirty").toBool());

    const auto *rule = environment.rules.rule(QUuid(ruleId));
    QVERIFY(rule);
    QVERIFY(rule->enabled);
    QCOMPARE(rule->name, QStringLiteral("Secret notes"));
    QCOMPARE(rule->conditions.constFirst().value, QStringLiteral("*secret*"));
    QCOMPARE(rule->actions.constFirst().folderId, addedFolder.value);
}

void RulesPageTest::reordersRulesWithSharedAnimation()
{
    RuleEnvironment environment;
    RulesController controller(&environment.rules, &environment.folders);
    const auto      firstId  = controller.createRule();
    const auto      secondId = controller.createRule();
    const auto      thirdId  = controller.createRule();
    QVERIFY(!firstId.isEmpty());
    QVERIFY(!secondId.isEmpty());
    QVERIFY(!thirdId.isEmpty());

    QQuickWidget view;
    view.resize(760, 520);
    view.setResizeMode(QQuickWidget::SizeRootObjectToView);
    view.rootContext()->setContextProperty(QStringLiteral("rulesController"), &controller);
    view.setSource(QUrl(QStringLiteral("qrc:/qml/RulesPage.qml")));
    QCOMPARE(view.status(), QQuickWidget::Ready);
    view.show();

    auto *root = qobject_cast<QQuickItem *>(view.rootObject());
    QVERIFY(root);
    QQuickItem *first  = nullptr;
    QQuickItem *second = nullptr;
    QQuickItem *third  = nullptr;
    QTRY_VERIFY(first = quickItemByName(root, QStringLiteral("ruleRow-%1").arg(firstId)));
    QTRY_VERIFY(second = quickItemByName(root, QStringLiteral("ruleRow-%1").arg(secondId)));
    QTRY_VERIFY(third = quickItemByName(root, QStringLiteral("ruleRow-%1").arg(thirdId)));

    const QPointF from = first->mapToItem(root, QPointF(12, first->height() / 2));
    const QPointF to   = third->mapToItem(root, QPointF(12, third->height() / 2));
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, from.toPoint());
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(&view, (from + (to - from) * (qreal(step) / 8)).toPoint(), 15);

    QTRY_VERIFY(root->property("dragging").toBool());
    QTRY_COMPARE(root->property("previewCount").toInt(), 1);
    QTRY_VERIFY(second->property("reorderOffset").toReal() < -1);

    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, to.toPoint());
    QTRY_VERIFY(!root->property("dragging").toBool());
    QVERIFY(environment.rules.rules().constFirst().id != QUuid(firstId));
}

QTEST_MAIN(RulesPageTest)
#include "rulespage_test.moc"
