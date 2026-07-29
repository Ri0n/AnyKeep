#include "nextcloudcategory.h"

#include <QtTest>

using namespace QtNote;

class NextcloudCategoryTest : public QObject {
    Q_OBJECT

private slots:
    void decodesNestedCategoryPaths();
    void emptyCategoryMeansUnsorted();
    void rejectsAmbiguousPaths();
};

void NextcloudCategoryTest::decodesNestedCategoryPaths()
{
    QStringList path;
    QString     error;
    QVERIFY(NextcloudCategory::decode(QStringLiteral(" Projects / 2026 / Receipts "), &path, &error));
    QCOMPARE(path, QStringList({ QStringLiteral("Projects"), QStringLiteral("2026"), QStringLiteral("Receipts") }));
    QVERIFY(error.isEmpty());

    QString category;
    QVERIFY(NextcloudCategory::encode(path, &category, &error));
    QCOMPARE(category, QStringLiteral("Projects/2026/Receipts"));
    QVERIFY(error.isEmpty());
}

void NextcloudCategoryTest::emptyCategoryMeansUnsorted()
{
    QStringList path { QStringLiteral("stale") };
    QString     error;
    QVERIFY(NextcloudCategory::decode(QStringLiteral("   "), &path, &error));
    QVERIFY(path.isEmpty());

    QString category = QStringLiteral("stale");
    QVERIFY(NextcloudCategory::encode({}, &category, &error));
    QVERIFY(category.isEmpty());
}

void NextcloudCategoryTest::rejectsAmbiguousPaths()
{
    QStringList path;
    QString     error;
    QVERIFY(!NextcloudCategory::decode(QStringLiteral("Projects//Receipts"), &path, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!NextcloudCategory::decode(QStringLiteral("Projects/"), &path, &error));
    QVERIFY(!error.isEmpty());

    QString category;
    QVERIFY(!NextcloudCategory::encode({ QStringLiteral("Projects/2026") }, &category, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!NextcloudCategory::encode({ QStringLiteral(" ") }, &category, &error));
    QVERIFY(!error.isEmpty());
}

QTEST_GUILESS_MAIN(NextcloudCategoryTest)
#include "nextcloudcategory_test.moc"
