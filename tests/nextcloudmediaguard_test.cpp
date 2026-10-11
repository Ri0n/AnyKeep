#include "nextcloudstorage.h"
#include <QtTest>
using namespace AnyKeep;
class NextcloudMediaGuardTest final : public QObject {
    Q_OBJECT
private slots:
    void mediaManifestCannotBeDroppedOnSave();
};
void NextcloudMediaGuardTest::mediaManifestCannotBeDroppedOnSave()
{
    NextcloudStorage storage;
    auto note = storage.createNote();
    note.setText(QStringLiteral("Plain text"), Note::Markdown);
    MediaReference reference;
    reference.id = QUuid::createUuid();
    reference.portableName = QStringLiteral("picture.png");
    note.setMedia({ reference });
    auto *job = storage.saveNoteAsync(note);
    QVERIFY(job->isFinished());
    QCOMPARE(job->state(), StorageJob::Failed);
    QCOMPARE(job->error().code, StorageError::Other);
    QVERIFY(!job->error().retryable);
    job->deleteLater();
}
QTEST_GUILESS_MAIN(NextcloudMediaGuardTest)
#include "nextcloudmediaguard_test.moc"
