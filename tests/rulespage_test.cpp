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

using namespace QtNote;

class RulesPageTest : public QObject {
    Q_OBJECT

private slots:
    void loadsWithAnEmptyRuleStore();
    void loadsWithAnEditableRule();
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
        Q_ASSERT(folders.initialize());
        Q_ASSERT(rules.initialize());
    }
};

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

QTEST_MAIN(RulesPageTest)
#include "rulespage_test.moc"
