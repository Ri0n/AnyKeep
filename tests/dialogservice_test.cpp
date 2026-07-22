#include <QSignalSpy>
#include <QtTest>

#include "dialogservice.h"

using namespace QtNote;

class DialogServiceTest : public QObject {
    Q_OBJECT

private slots:
    void queuesRequestsAndCompletesThemInOrder()
    {
        DialogService dialogs;
        QSignalSpy    completed(&dialogs, &DialogService::completed);

        const auto first  = dialogs.confirm(QStringLiteral("Delete"), QStringLiteral("Delete note?"),
                                            QStringLiteral("Delete"), QStringLiteral("Cancel"), true);
        const auto second = dialogs.inform(QStringLiteral("Done"), QStringLiteral("Exported"));

        QVERIFY(first != 0);
        QVERIFY(second > first);
        QVERIFY(dialogs.active());
        QCOMPARE(dialogs.requestId(), first);
        QCOMPARE(dialogs.title(), QStringLiteral("Delete"));
        QCOMPARE(dialogs.acceptText(), QStringLiteral("Delete"));
        QCOMPARE(dialogs.rejectText(), QStringLiteral("Cancel"));
        QVERIFY(dialogs.destructive());

        dialogs.reject();
        QCOMPARE(completed.size(), 1);
        QCOMPARE(completed.at(0).at(0).toULongLong(), first);
        QVERIFY(!completed.at(0).at(1).toBool());
        QVERIFY(dialogs.active());
        QCOMPARE(dialogs.requestId(), second);
        QCOMPARE(dialogs.title(), QStringLiteral("Done"));
        QCOMPARE(dialogs.rejectText(), QString());
        QVERIFY(!dialogs.destructive());

        dialogs.accept();
        QCOMPARE(completed.size(), 2);
        QCOMPARE(completed.at(1).at(0).toULongLong(), second);
        QVERIFY(completed.at(1).at(1).toBool());
        QVERIFY(!dialogs.active());
        QCOMPARE(dialogs.requestId(), quint64(0));
    }

    void ignoresCompletionWithoutActiveRequest()
    {
        DialogService dialogs;
        QSignalSpy    completed(&dialogs, &DialogService::completed);

        dialogs.accept();
        dialogs.reject();

        QVERIFY(completed.isEmpty());
        QVERIFY(!dialogs.active());
    }
};

QTEST_MAIN(DialogServiceTest)
#include "dialogservice_test.moc"
