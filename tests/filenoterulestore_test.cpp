#include "filenoterulestore.h"

#include "secureenvelope.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace QtNote;

namespace {

NoteRuleSnapshot snapshot(const QString &name, const QString &noteId)
{
    NoteRule rule;
    rule.id         = QUuid::createUuid();
    rule.name       = name;
    rule.revision   = 1;
    rule.modifiedAt = QDateTime::currentDateTimeUtc();
    rule.conditions = { { NoteRuleConditionKind::TextContains, QStringLiteral("private phrase"), false } };
    NoteRuleAction action;
    action.kind      = NoteRuleActionKind::SelectStorage;
    action.storageId = QStringLiteral("ptf");
    rule.actions     = { action };

    NoteRuleApplicationMarker marker;
    marker.ruleId           = rule.id;
    marker.ruleRevision     = rule.revision;
    marker.storageId        = QStringLiteral("tomboy");
    marker.noteId           = noteId;
    marker.inputFingerprint = QByteArray(32, '\x7f');
    marker.appliedAt        = rule.modifiedAt;
    return { { rule }, { marker } };
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

} // namespace

class FileNoteRuleStoreTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void roundTripsWithoutPlaintextLeakage();
    void restoresAuthenticatedBackupAfterCorruption();
};

void FileNoteRuleStoreTest::initTestCase()
{
    QVERIFY2(FileNoteRuleStore::cryptoAvailable(), "AES-256-GCM unavailable");
}

void FileNoteRuleStoreTest::roundTripsWithoutPlaintextLeakage()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto        path = directory.filePath(QStringLiteral("rules.bin"));
    FileNoteRuleStore store(path, SecureEnvelope::generateMasterKey());
    const auto        expected = snapshot(QStringLiteral("Route private notes"), QStringLiteral("secret-note-id"));
    QVERIFY(!store.save(expected));

    const auto bytes = readFile(path);
    QVERIFY(!bytes.isEmpty());
    QVERIFY(!bytes.contains(QByteArrayLiteral("Route private notes")));
    QVERIFY(!bytes.contains(QByteArrayLiteral("private phrase")));
    QVERIFY(!bytes.contains(QByteArrayLiteral("secret-note-id")));

    const auto loaded = store.load();
    QVERIFY2(loaded, qPrintable(loaded.error.message));
    QCOMPARE(loaded.value.rules.size(), 1);
    QCOMPARE(loaded.value.markers.size(), 1);
    QCOMPARE(loaded.value.rules.constFirst().name, expected.rules.constFirst().name);
    QCOMPARE(loaded.value.rules.constFirst().conditions.constFirst().value,
             expected.rules.constFirst().conditions.constFirst().value);
    QCOMPARE(loaded.value.markers.constFirst().noteId, expected.markers.constFirst().noteId);
}

void FileNoteRuleStoreTest::restoresAuthenticatedBackupAfterCorruption()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto        path = directory.filePath(QStringLiteral("rules.bin"));
    FileNoteRuleStore store(path, SecureEnvelope::generateMasterKey());
    const auto        first  = snapshot(QStringLiteral("First"), QStringLiteral("note-one"));
    const auto        second = snapshot(QStringLiteral("Second"), QStringLiteral("note-two"));
    QVERIFY(!store.save(first));
    QVERIFY(!store.save(second));
    QVERIFY(store.hasBackup());

    QFile corrupt(path);
    QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(corrupt.write("not an encrypted rule store"), qint64(27));
    corrupt.close();
    QVERIFY(!store.load());

    QString preserved;
    QVERIFY(!store.restoreBackup(&preserved));
    QVERIFY(!preserved.isEmpty());
    const auto restored = store.load();
    QVERIFY2(restored, qPrintable(restored.error.message));
    QCOMPARE(restored.value.rules.constFirst().name, first.rules.constFirst().name);
    QCOMPARE(restored.value.markers.constFirst().noteId, first.markers.constFirst().noteId);
}

QTEST_GUILESS_MAIN(FileNoteRuleStoreTest)
#include "filenoterulestore_test.moc"
