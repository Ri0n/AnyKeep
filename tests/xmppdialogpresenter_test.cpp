#include "xmppdialogpresenter.h"

#include <QPointer>
#include <QQuickWindow>
#include <QtTest>

using namespace AnyKeep;

class XmppDialogPresenterTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { Q_INIT_RESOURCE(xmppsettings); }

    void trustRequestsArePresentedAndQueued()
    {
        XmppDialogPresenter presenter;
        int                 approved = 0;
        int                 rejected = 0;
        presenter.presentTrustRequest(
            QStringLiteral("first"), QByteArray::fromHex("abcdef"), [&] { ++approved; }, [&] { ++rejected; });
        // Invalid initial QML properties must not silently reject the request.
        QCOMPARE(rejected, 0);
        QPointer<XmppKeySyncPrompt> first = presenter.findChild<XmppKeySyncPrompt *>();
        QVERIFY(first);
        QCOMPARE(first->fingerprint(), QStringLiteral("abcdef"));
        QVERIFY(!first->completed());

        presenter.presentTrustRequest(
            QStringLiteral("second"), QByteArray::fromHex("123456"), [&] { ++approved; }, [&] { ++rejected; });
        QCOMPARE(presenter.findChildren<XmppKeySyncPrompt *>().size(), 1);
        first->accept();
        QCOMPARE(approved, 1);
        QTRY_VERIFY(first.isNull());
        QPointer<XmppKeySyncPrompt> second;
        QTRY_VERIFY((second = presenter.findChild<XmppKeySyncPrompt *>()) && !second->completed());
        QCOMPARE(second->fingerprint(), QStringLiteral("123456"));
        second->reject();
        QCOMPARE(rejected, 1);
        QTRY_VERIFY(second.isNull());
    }

    void closingTrustWindowRejectsTheRequest()
    {
        XmppDialogPresenter presenter;
        int                 rejected = 0;
        presenter.presentTrustRequest(
            QStringLiteral("close"), QByteArray::fromHex("abcdef"), [] {}, [&] { ++rejected; });
        QCOMPARE(rejected, 0);
        QQuickWindow *dialog = nullptr;
        for (auto *window : QGuiApplication::topLevelWindows()) {
            if (auto *quick = qobject_cast<QQuickWindow *>(window); quick && quick->isVisible())
                dialog = quick;
        }
        QVERIFY(dialog);
        dialog->close();
        QTRY_COMPARE(rejected, 1);
    }
};

QTEST_MAIN(XmppDialogPresenterTest)
#include "xmppdialogpresenter_test.moc"
