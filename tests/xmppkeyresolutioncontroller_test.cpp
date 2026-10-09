#include "xmppkeyresolutioncontroller.h"

#include <QSignalSpy>
#include <QTest>

using namespace AnyKeep;

class XmppKeyResolutionControllerTest : public QObject {
    Q_OBJECT

private slots:
    void completesRecovery();
    void requiresRecognizedDevice();
    void reportsPartialDeviceRefresh();
    void timesOutAndIgnoresLateDeviceResult();
    void removesPublishedDevice();
    void createsNewKeyOnlyWhenNoNotesExist();
    void startsFreshWithoutChangingUnreadableNotes();
    void firstInstallCanCreateKeyWithoutOmemoDiscovery();
    void firstInstallBackDiscardsGeneratedKey();
    void firstInstallKeyGenerationFailureCanRetry();
    void firstInstallCancelledDoesNotInstallKey();
    void existingLocalKeyCannotStartFresh();
    void freshKeyMismatchPolicyIsScopedToExplicitChoice();
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
    device.trustLevel = XmppTrustLevel::Undecided;

    XmppKeyResolutionController controller(
        true, { device }, {},
        [&trustedKeys](const QList<QByteArray> &keys, auto completion) {
            trustedKeys = keys;
            XmppStatusResult result;
            result.ok = true;
            completion(result);
        },
        {},
        [storageKey](auto completion) {
            XmppKeyAuditResult audit;
            audit.ok              = true;
            audit.totalIndexItems = 3;
            audit.candidates.append({ QStringLiteral("AnyKeep-desktop"), storageKey, QByteArray("key-id"), 3, false });
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
    QCOMPARE(controller.nextText(), QStringLiteral("Sync notes"));
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ResultPage));
    QCOMPARE(controller.nextText(), QStringLiteral("Finish"));
    QVERIFY(controller.resultText().contains(QStringLiteral("Synchronization completed")));
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
    device.trustLevel = XmppTrustLevel::Undecided;

    bool                        trustCalled = false;
    XmppKeyResolutionController controller(
        true, { device }, {}, [&trustCalled](const QList<QByteArray> &, auto) { trustCalled = true; }, {}, [](auto) {},
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
    available.trustLevel = XmppTrustLevel::ManuallyTrusted;

    XmppDeviceInfo stale;
    stale.label    = QStringLiteral("Old device");
    stale.deviceId = 7;

    const QString               warning = QStringLiteral("Could not obtain the OMEMO fingerprint for 1 device(s)");
    XmppKeyResolutionController controller(true, { available, stale }, warning, {}, {}, {}, {});

    QCOMPARE(controller.devicesModel()->rowCount(), 2);
    QVERIFY(controller.deviceStatus().contains(QStringLiteral("Found 2 OMEMO device(s)")));
    QVERIFY(controller.deviceStatus().contains(warning));
}

void XmppKeyResolutionControllerTest::timesOutAndIgnoresLateDeviceResult()
{
    XmppDeviceInfo device { QStringLiteral("Laptop"), 42, QByteArray("device-key"), XmppTrustLevel::Undecided };
    XmppKeyResolutionController::StatusCompletion lateCompletion;
    XmppKeyResolutionController                   controller(
        true, { device }, {},
        [&lateCompletion](const QList<QByteArray> &, auto completion) { lateCompletion = std::move(completion); }, {},
        [](auto) {}, [](const QList<QByteArray> &, const QByteArray &, auto) {}, 5);

    controller.next();
    controller.setDeviceSelected(0, true);
    controller.next();
    QVERIFY(controller.busy());
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 100);
    QVERIFY(controller.deviceStatus().contains(QStringLiteral("Timed out")));
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::DevicesPage));

    XmppStatusResult result;
    result.ok = true;
    lateCompletion(result);
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::DevicesPage));
}

void XmppKeyResolutionControllerTest::removesPublishedDevice()
{
    XmppDeviceInfo device { QStringLiteral("Old laptop"), 77, QByteArray("device-key"), XmppTrustLevel::Undecided };
    quint32        removedId = 0;
    XmppKeyResolutionController controller(
        true, { device }, {}, {},
        [&removedId](quint32 deviceId, auto completion) {
            removedId = deviceId;
            XmppStatusResult result;
            result.ok = true;
            completion(result);
        },
        {}, {}, 1000);

    controller.next();
    controller.removeDevice(0);
    QCOMPARE(removedId, quint32(77));
    QCOMPARE(controller.devicesModel()->rowCount(), 0);
    QVERIFY(controller.deviceStatus().contains(QStringLiteral("was removed")));
}

void XmppKeyResolutionControllerTest::createsNewKeyOnlyWhenNoNotesExist()
{
    const QByteArray generatedKey(32, 'n');
    XmppDeviceInfo device { QStringLiteral("Desktop"), 42, QByteArray("device-key"), XmppTrustLevel::ManuallyTrusted };
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
            completion(audit);
        },
        [](const QList<QByteArray> &, const QByteArray &, auto) {}, 1000,
        [generatedKey](auto completion) {
            XmppStatusResult result;
            result.ok = true;
            completion(result, generatedKey, QByteArray("generated-key-id"));
        });

    controller.next();
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::KeysPage));
    QVERIFY(controller.canCreateNewKey());
    controller.createNewKey();
    QCOMPARE(controller.canonicalKey(), generatedKey);
    QVERIFY(!controller.canCreateNewKey());
    QVERIFY(controller.keyStatus().contains(QStringLiteral("new storage key is ready")));
}

void XmppKeyResolutionControllerTest::startsFreshWithoutChangingUnreadableNotes()
{
    const QByteArray generatedKey(32, 'f');
    XmppDeviceInfo device { QStringLiteral("Desktop"), 42, QByteArray("device-key"), XmppTrustLevel::ManuallyTrusted };
    bool           rekeyCalled = false;
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
            audit.ok              = true;
            audit.totalIndexItems = 2;
            completion(audit);
        },
        [&rekeyCalled](const QList<QByteArray> &, const QByteArray &, auto) { rekeyCalled = true; }, 1000,
        [generatedKey](auto completion) {
            XmppStatusResult result;
            result.ok = true;
            completion(result, generatedKey, QByteArray("fresh-key-id"));
        });

    controller.next();
    controller.next();
    QVERIFY(controller.canStartFresh());
    QVERIFY(!controller.canCreateNewKey());
    controller.startFresh();
    QVERIFY(controller.freshStart());
    QCOMPARE(controller.canonicalKey(), generatedKey);
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ReviewPage));
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ResultPage));
    QVERIFY(controller.rekeyResult().ok);
    QVERIFY(!rekeyCalled);
}

void XmppKeyResolutionControllerTest::firstInstallCanCreateKeyWithoutOmemoDiscovery()
{
    const QByteArray newKey(32, 'n');
    int auditCalls = 0;
    int trustCalls = 0;
    int rekeyCalls = 0;
    int generateCalls = 0;
    XmppKeyResolutionController controller(
        true, {}, {},
        [&trustCalls](const QList<QByteArray> &, auto) { ++trustCalls; },
        {},
        [&auditCalls](auto) { ++auditCalls; },
        [&rekeyCalls](const QList<QByteArray> &, const QByteArray &, auto) { ++rekeyCalls; },
        1000,
        [&generateCalls, newKey](auto completion) {
            ++generateCalls;
            XmppStatusResult status;
            status.ok = true;
            completion(status, newKey, QByteArray("fresh-onboarding"));
        });

    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ProblemPage));
    QVERIFY(controller.canStartFresh());
    QVERIFY(!controller.canCreateNewKey());
    controller.startFresh();
    QCOMPARE(generateCalls, 1);
    QCOMPARE(auditCalls, 0);
    QCOMPARE(trustCalls, 0);
    QVERIFY(controller.freshStart());
    QCOMPARE(controller.canonicalKey(), newKey);
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ReviewPage));
    QVERIFY(controller.summary().contains(QStringLiteral("if any")));
    QVERIFY(!controller.summary().contains(QStringLiteral("unchanged: 0")));

    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ResultPage));
    QVERIFY(controller.rekeyResult().ok);
    QCOMPARE(rekeyCalls, 0);
    QVERIFY(controller.resultText().contains(QStringLiteral("if any")));
    QSignalSpy finished(&controller, &XmppKeyResolutionController::finished);
    controller.next();
    QCOMPARE(finished.size(), 1);
    QVERIFY(finished.first().first().toBool());
}

void XmppKeyResolutionControllerTest::firstInstallBackDiscardsGeneratedKey()
{
    int generated = 0;
    XmppKeyResolutionController controller(
        true, {}, {}, {}, {}, {}, {}, 1000,
        [&generated](auto completion) {
            ++generated;
            XmppStatusResult status;
            status.ok = true;
            completion(status, QByteArray(32, 'x'), QByteArray("generated-id"));
        });
    controller.startFresh();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ReviewPage));
    QVERIFY(!controller.canonicalKey().isEmpty());

    controller.back();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ProblemPage));
    QVERIFY(controller.canonicalKey().isEmpty());
    QVERIFY(!controller.freshStart());
    controller.next();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::DevicesPage));
    QVERIFY(controller.canonicalKey().isEmpty());
    QCOMPARE(generated, 1);
}

void XmppKeyResolutionControllerTest::firstInstallKeyGenerationFailureCanRetry()
{
    int attempts = 0;
    XmppKeyResolutionController controller(
        true, {}, {}, {}, {}, {}, {}, 1000,
        [&attempts](auto completion) {
            ++attempts;
            XmppStatusResult status;
            status.ok = attempts > 1;
            status.error = status.ok ? QString() : QStringLiteral("Key generator unavailable");
            completion(status, status.ok ? QByteArray(32, 'k') : QByteArray(),
                       status.ok ? QByteArray("recovered-generator") : QByteArray());
        });
    controller.startFresh();
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ProblemPage));
    QVERIFY(controller.keyStatus().contains(QStringLiteral("Key generator unavailable")));
    QVERIFY(controller.canStartFresh());
    controller.startFresh();
    QCOMPARE(attempts, 2);
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ReviewPage));
}

void XmppKeyResolutionControllerTest::firstInstallCancelledDoesNotInstallKey()
{
    int generateCalls = 0;
    XmppKeyResolutionController controller(
        true, {}, {}, {}, {}, {}, {}, 1000,
        [&generateCalls](auto completion) {
            ++generateCalls;
            XmppStatusResult status;
            status.ok = true;
            completion(status, QByteArray(32, 'k'), QByteArray("new-key"));
        });
    QSignalSpy finished(&controller, &XmppKeyResolutionController::finished);
    controller.startFresh();
    controller.cancel();
    QCOMPARE(generateCalls, 1);
    QCOMPARE(finished.size(), 1);
    QVERIFY(!finished.first().first().toBool());
    QVERIFY(!controller.rekeyResult().ok);
}

void XmppKeyResolutionControllerTest::existingLocalKeyCannotStartFresh()
{
    int generated = 0;
    XmppKeyResolutionController controller(
        false, {}, {}, {}, {}, {}, {}, 1000,
        [&generated](auto completion) { ++generated; });
    QVERIFY(!controller.canStartFresh());
    controller.startFresh();
    QCOMPARE(generated, 0);
    QCOMPARE(controller.currentPage(), int(XmppKeyResolutionController::ProblemPage));
}

void XmppKeyResolutionControllerTest::freshKeyMismatchPolicyIsScopedToExplicitChoice()
{
    XmppConfig config;
    // Ordinary key recovery must still be triggered when all published
    // encrypted indices belong to a foreign key.
    QVERIFY(config.foreignKeyOnlyIsError(0, 3));
    QVERIFY(!config.foreignKeyOnlyIsError(1, 3));
    QVERIFY(!config.foreignKeyOnlyIsError(0, 0));

    // A user-selected fresh local storage preserves and ignores old indices,
    // instead of immediately reopening recovery after installing the key.
    config.allowForeignKeyIndices = true;
    QVERIFY(!config.foreignKeyOnlyIsError(0, 3));
    QVERIFY(!config.foreignKeyOnlyIsError(4, 3));
}

QTEST_GUILESS_MAIN(XmppKeyResolutionControllerTest)

#include "xmppkeyresolutioncontroller_test.moc"
