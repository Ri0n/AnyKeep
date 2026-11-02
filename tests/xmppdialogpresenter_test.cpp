#include "xmppdialogpresenter.h"
#include "xmppkeyresolutioncontroller.h"

#include <QPointer>
#include <QQuickItem>
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

    void recoveryShowsLiveSynchronizationProgress_data()
    {
        QTest::addColumn<QSize>("windowSize");
        QTest::newRow("desktop") << QSize(800, 620);
        QTest::newRow("narrow") << QSize(360, 640);
        QTest::newRow("minimum") << QSize(320, 360);
    }

    void recoveryShowsLiveSynchronizationProgress()
    {
        QFETCH(QSize, windowSize);
        XmppDialogPresenter                          presenter;
        XmppKeyResolutionController::RekeyCompletion rekeyCompletion;
        XmppDeviceInfo                               device;
        device.deviceId   = 42;
        device.keyId      = QByteArray("known-device");
        device.trustLevel = XmppTrustLevel::ManuallyTrusted;
        XmppKeyResolutionController controller(
            true, { device }, {},
            [](const QList<QByteArray> &, auto completion) {
                XmppStatusResult result;
                result.ok = true;
                completion(result);
            },
            {},
            [](auto completion) {
                XmppKeyAuditResult audit;
                audit.ok = true;
                audit.candidates.append(
                    { QStringLiteral("Desktop"), QByteArray(32, 'k'), QByteArray("id"), 10, false });
                completion(audit);
            },
            [&](const QList<QByteArray> &, const QByteArray &, auto completion) {
                rekeyCompletion = std::move(completion);
            });
        presenter.presentKeyResolution(&controller);
        controller.next();
        controller.next();
        controller.next();
        controller.next();
        QVERIFY(controller.busy());
        QQuickWindow *dialog = nullptr;
        for (auto *window : QGuiApplication::topLevelWindows()) {
            if (auto *quick = qobject_cast<QQuickWindow *>(window); quick && quick->isVisible())
                dialog = quick;
        }
        QVERIFY(dialog);
        dialog->resize(windowSize);
        auto *summary = presenter.findChild<QObject *>(QStringLiteral("xmppSyncSummary"));
        auto *events  = presenter.findChild<QObject *>(QStringLiteral("xmppSyncEvents"));
        auto *counter = presenter.findChild<QObject *>(QStringLiteral("xmppSyncCounter"));
        QVERIFY(summary);
        QVERIFY(events);
        QVERIFY(counter);
        QVERIFY(!counter->property("visible").toBool());
        QVERIFY(!events->property("visible").toBool());
        auto progress = controller.progressCallback();
        progress({ QStringLiteral("Downloading note 4 of 10"), 3, 10 });
        QTRY_COMPARE(summary->property("text").toString(), QStringLiteral("Downloading note 4 of 10"));
        QTRY_VERIFY(events->property("text").toString().contains(QStringLiteral("Downloading note 4 of 10")));
        QTRY_COMPARE(counter->property("value").toDouble(), 0.3);
        QCOMPARE(counter->property("to").toDouble(), 1.0);
        QVERIFY(counter->property("visible").toBool());
        auto *logViewport  = presenter.findChild<QQuickItem *>(QStringLiteral("xmppSyncLogViewport"));
        auto *toggle       = presenter.findChild<QQuickItem *>(QStringLiteral("xmppSyncLogToggle"));
        auto *panel        = presenter.findChild<QQuickItem *>(QStringLiteral("xmppRecoveryProgress"));
        auto *scroll       = presenter.findChild<QQuickItem *>(QStringLiteral("xmppRecoveryScroll"));
        auto *cancelButton = presenter.findChild<QQuickItem *>(QStringLiteral("xmppRecoveryCancelButton"));
        auto *backButton   = presenter.findChild<QQuickItem *>(QStringLiteral("xmppRecoveryBackButton"));
        auto *nextButton   = presenter.findChild<QQuickItem *>(QStringLiteral("xmppRecoveryNextButton"));
        QVERIFY(logViewport);
        QVERIFY(toggle);
        QVERIFY(panel);
        QVERIFY(scroll);
        QVERIFY(cancelButton);
        QVERIFY(backButton);
        QVERIFY(nextButton);
        auto footerFits = [&] {
            for (auto *button : { cancelButton, backButton, nextButton }) {
                const auto bounds = button->mapRectToScene(button->boundingRect());
                if (!button->isVisible() || bounds.top() < 0 || bounds.bottom() > dialog->height() || bounds.left() < 0
                    || bounds.right() > dialog->width())
                    return false;
            }
            return true;
        };
        QTRY_VERIFY(footerFits());
        auto *flickable = scroll->property("contentItem").value<QObject *>();
        QVERIFY(flickable);
        for (bool expanded : { true, false, true, false, true }) {
            // Bring the toggle into view in a short window, then use a real
            // click so the test covers the checked/expanded bindings too.
            QCoreApplication::processEvents();
            flickable->setProperty(
                "contentY",
                qMax(0.0, qMin(panel->y(), flickable->property("contentHeight").toDouble() - scroll->height())));
            QCoreApplication::processEvents();
            QSignalSpy frame(dialog, &QQuickWindow::afterAnimating);
            dialog->requestUpdate();
            QVERIFY(frame.wait());
            QTest::mouseClick(dialog, Qt::LeftButton, Qt::NoModifier,
                              toggle->mapToScene(QPointF(toggle->width() / 2, toggle->height() / 2)).toPoint());
            QTRY_COMPARE(panel->property("expanded").toBool(), expanded);
            QTRY_COMPARE(logViewport->isVisible(), expanded);
            QTRY_VERIFY(footerFits());
        }
        QTRY_VERIFY(logViewport->height() > 0);
        for (int i = 0; i < 60; ++i)
            progress({ QStringLiteral("Synchronizing note %1: ").arg(i) + QString(300, QLatin1Char('x')), 3, 10 });
        QTRY_VERIFY(events->property("text").toString().contains(QStringLiteral("Synchronizing note 59:")));
        QTRY_VERIFY(logViewport->height() <= 150);
        QTRY_VERIFY(footerFits());
        QTRY_VERIFY(events->property("contentHeight").toDouble() > logViewport->height());
        auto *elapsed = presenter.findChild<QObject *>(QStringLiteral("xmppSyncElapsed"));
        QVERIFY(elapsed);
        const auto initialElapsed = elapsed->property("text").toString();
        QTRY_VERIFY_WITH_TIMEOUT(elapsed->property("text").toString() != initialElapsed, 2500);
        XmppRekeyResult result;
        result.ok    = true;
        result.total = result.unchanged = 10;
        rekeyCompletion(result);
        QTRY_VERIFY(nextButton->isEnabled());
        QTRY_VERIFY(footerFits());
        QTest::mouseClick(dialog, Qt::LeftButton, Qt::NoModifier,
                          nextButton->mapToScene(QPointF(nextButton->width() / 2, nextButton->height() / 2)).toPoint());
        QTRY_VERIFY(controller.completed());
    }

    void closingResultWithFailedKeyInstallationKeepsRetryAvailable()
    {
        XmppDialogPresenter         presenter;
        XmppKeyResolutionController controller(true, {}, {}, {}, {}, {}, {}, 1000, [](auto completion) {
            completion({ true }, QByteArray(32, 'k'), QByteArrayLiteral("key-id"));
        });
        int                         attempts = 0;
        controller.setKeyInstaller([&](QByteArray, bool, auto completion) {
            QTimer::singleShot(0, &controller, [&, completion = std::move(completion)]() mutable {
                XmppStatusResult result;
                result.ok    = ++attempts > 1;
                result.error = result.ok ? QString {} : QStringLiteral("Keychain unavailable");
                completion(result);
            });
        });
        presenter.presentKeyResolution(&controller);
        controller.startFresh();
        controller.next();
        QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ResultPage));
        QQuickWindow *window = nullptr;
        for (auto *candidate : QGuiApplication::topLevelWindows()) {
            if (auto *quick = qobject_cast<QQuickWindow *>(candidate); quick && quick->isVisible())
                window = quick;
        }
        QVERIFY(window);
        window->close();
        QTRY_COMPARE(attempts, 1);
        QTRY_VERIFY(window->isVisible());
        QVERIFY(!controller.completed());
        QVERIFY(controller.resultText().contains(QStringLiteral("Keychain unavailable")));
        controller.next();
        QTRY_VERIFY(controller.completed());
        QCOMPARE(attempts, 2);
    }
};

QTEST_MAIN(XmppDialogPresenterTest)
#include "xmppdialogpresenter_test.moc"
