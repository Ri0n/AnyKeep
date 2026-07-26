#include "xmppkeyresolutioncontroller.h"

#include <QSignalSpy>
#include <QTest>
#include <QXmppTrustLevel.h>

using namespace QtNote;

class XmppKeyResolutionControllerTest : public QObject {
    Q_OBJECT

private slots:
    void completesRecovery();
    void requiresRecognizedDevice();
    void reportsPartialDeviceRefresh();
};

void XmppKeyResolutionControllerTest::completesRecovery()
{
    const QByteArray  deviceKey("device-key");
    const QByteArray  storageKey(32, 'k');
    QList<QByteArray> trustedKeys;
    QList<QByteArray> rekeyKeys;
    QByteArray        canonical;

    XmppDeviceInfo device;
    device.label      = QStringLiteral("Laptop");
    device.deviceId   = 42;
    device.keyId      = deviceKey;
    device.trustLevel = int(QXmpp::TrustLevel::Undecided);

    XmppKeyResolutionController controller(
        true, { device }, {},
        [&trustedKeys](const QList<QByteArray> &keys, auto completion) {
            trustedKeys = keys;
            XmppStatusResult result;
            result.ok = true;
            completion(result);
        },
        [storageKey](auto completion) {
            XmppKeyAuditResult audit;
            audit.ok              = true;
            audit.totalIndexItems = 3;
            audit.candidates.append({ QStringLiteral("QtNote-desktop"), storageKey, QByteArray("key-id"), 3, false });
            completion(audit);
        },
        [&rekeyKeys, &canonical](const QList<QByteArray> &keys, const QByteArray &selected, auto completion) {
            rekeyKeys = keys;
            canonical = selected;
            XmppRekeyResult result;
            result.ok       = true;
            result.migrated = 3;
            result.total    = 3;
            completion(result);
        });

    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ProblemPage));
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::DevicesPage));

    controller.setDeviceSelected(0, true);
    controller.next();
    QCOMPARE(trustedKeys, QList<QByteArray> { deviceKey });
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::KeysPage));
    QCOMPARE(controller.selectedKeyIndex(), 0);

    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ReviewPage));
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ResultPage));
    QCOMPARE(rekeyKeys, QList<QByteArray> { storageKey });
    QCOMPARE(canonical, storageKey);
    QVERIFY(controller.rekeyResult().ok);

    QSignalSpy finished(&controller, &XmppKeyResolutionController::finished);
    controller.next();
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().first().toBool(), true);
}

void XmppKeyResolutionControllerTest::requiresRecognizedDevice()
{
    XmppDeviceInfo device;
    device.label      = QStringLiteral("Unknown");
    device.deviceId   = 7;
    device.keyId      = QByteArray("unknown-key");
    device.trustLevel = int(QXmpp::TrustLevel::Undecided);

    bool                        trustCalled = false;
    XmppKeyResolutionController controller(
        true, { device }, {}, [&trustCalled](const QList<QByteArray> &, auto) { trustCalled = true; }, [](auto) {},
        [](const QList<QByteArray> &, const QByteArray &, auto) {});

    controller.next();
    controller.next();
    QVERIFY(!trustCalled);
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::DevicesPage));
    QVERIFY(controller.deviceStatus().contains(QStringLiteral("Select at least one")));
}

void XmppKeyResolutionControllerTest::reportsPartialDeviceRefresh()
{
    XmppDeviceInfo available;
    available.label      = QStringLiteral("Desktop");
    available.deviceId   = 42;
    available.keyId      = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    available.trustLevel = int(QXmpp::TrustLevel::ManuallyTrusted);

    XmppDeviceInfo stale;
    stale.label    = QStringLiteral("Old device");
    stale.deviceId = 7;

    const QString               warning = QStringLiteral("Could not obtain the OMEMO fingerprint for 1 device(s)");
    XmppKeyResolutionController controller(true, { available, stale }, warning, {}, {}, {});

    QCOMPARE(controller.devicesModel()->rowCount(), 2);
    QVERIFY(controller.deviceStatus().contains(QStringLiteral("Found 2 OMEMO device(s)")));
    QVERIFY(controller.deviceStatus().contains(warning));
}

QTEST_GUILESS_MAIN(XmppKeyResolutionControllerTest)

#include "xmppkeyresolutioncontroller_test.moc"
