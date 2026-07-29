#include <QtTest>

#include "ptfstorage.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>

using namespace QtNote;

class PTFStorageTest : public QObject {
    Q_OBJECT

private slots:
    void noteSurvivesFreshStorageInstance()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QSettings  settings;
        const auto previousPath    = settings.value(QStringLiteral("storage.ptf.path"));
        const bool hadPreviousPath = settings.contains(QStringLiteral("storage.ptf.path"));

        {
            PTFStorage writer;
            QVERIFY(writer.setStoragePath(directory.path()));

            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Persistent note"));
            note.setText(QStringLiteral("Persistent body"), Note::Markdown);
            QVERIFY(writer.saveNote(note));
            QCOMPARE(writer.noteList().size(), 1);
        }

        {
            PTFStorage reader;
            QVERIFY(reader.setStoragePath(directory.path()));
            const auto notes = reader.noteList();
            QCOMPARE(notes.size(), 1);
            QCOMPARE(notes.first().title(), QStringLiteral("Persistent note"));

            Note loaded = notes.first();
            QVERIFY(loaded.load());
            QCOMPARE(loaded.text(), QStringLiteral("Persistent body"));
            QCOMPARE(loaded.format(), Note::Markdown);
        }

        if (hadPreviousPath)
            settings.setValue(QStringLiteral("storage.ptf.path"), previousPath);
        else
            settings.remove(QStringLiteral("storage.ptf.path"));
    }

    void noteListDoesNotRetainPreInitDirectory()
    {
        QTemporaryDir storageDirectory;
        QTemporaryDir unrelatedDirectory;
        QVERIFY(storageDirectory.isValid());
        QVERIFY(unrelatedDirectory.isValid());

        QSettings settings;
        struct EnvironmentGuard {
            QVariant previousPath;
            bool     hadPreviousPath;
            QString  previousWorkingDirectory;

            ~EnvironmentGuard()
            {
                QSettings restoreSettings;
                if (hadPreviousPath)
                    restoreSettings.setValue(QStringLiteral("storage.ptf.path"), previousPath);
                else
                    restoreSettings.remove(QStringLiteral("storage.ptf.path"));
                QDir::setCurrent(previousWorkingDirectory);
            }
        } guard { settings.value(QStringLiteral("storage.ptf.path")),
                  settings.contains(QStringLiteral("storage.ptf.path")), QDir::currentPath() };

        {
            PTFStorage writer;
            QVERIFY(writer.setStoragePath(storageDirectory.path()));
            Note note = writer.createNote();
            note.setTitle(QStringLiteral("Initialized note"));
            note.setText(QStringLiteral("Persistent body"), Note::Markdown);
            QVERIFY(writer.saveNote(note));
        }

        QVERIFY(QDir::setCurrent(unrelatedDirectory.path()));
        PTFStorage reader;

        // A direct pre-init read uses QDir's default path, but FileStorage no
        // longer retains that result after init() selects the real directory.
        QCOMPARE(reader.noteList().size(), 0);

        QVERIFY(reader.init());
        const auto notes = reader.noteList();
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.first().title(), QStringLiteral("Initialized note"));
    }

    void noteListReflectsExternalFileChanges()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        QCOMPARE(storage.noteList().size(), 0);

        QFile external(directory.filePath(QStringLiteral("external.md")));
        QVERIFY(external.open(QIODevice::WriteOnly | QIODevice::Text));
        QCOMPARE(external.write("External title\nExternal body"), qint64(28));
        external.close();

        const auto notes = storage.noteList();
        QCOMPARE(notes.size(), 1);
        QCOMPARE(notes.first().id(), QStringLiteral("external"));
        QCOMPARE(notes.first().title(), QStringLiteral("External title"));
    }

    void requestedModificationTimeSurvivesSave()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));

        Note note = storage.createNote();
        note.setTitle(QStringLiteral("Ordered note"));
        note.setText(QStringLiteral("Body"), Note::Markdown);
        QVERIFY(storage.saveNote(note));

        note = storage.noteList().first();
        QVERIFY(note.load());
        const auto requested = QDateTime::fromMSecsSinceEpoch(1'720'000'000'123, QTimeZone::UTC);
        note.setLastChangeUTC(requested);
        note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), requested);
        QVERIFY(storage.saveNote(note));

        const auto saved = storage.noteList().first();
        QCOMPARE(saved.lastChangeUTC().toMSecsSinceEpoch(), requested.toMSecsSinceEpoch());
        QVERIFY(!saved.backendValue(QString::fromLatin1(RequestedModificationTimeBackendKey)).isValid());
    }

    void reorderChangesOnlyFileTimesAndPreservesBlockOrder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        QVERIFY(storage.supportsNoteReordering());

        const auto baseTime = QDateTime::currentDateTimeUtc().addSecs(-10);
        const auto saveAt   = [&storage](const QString &title, const QDateTime &modified) {
            Note note = storage.createNote();
            note.setTitle(title);
            note.setText(title + QStringLiteral(" body"), Note::Markdown);
            note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), modified);
            return storage.saveNote(note);
        };
        QVERIFY(saveAt(QStringLiteral("First"), baseTime));
        QVERIFY(saveAt(QStringLiteral("Second"), baseTime.addSecs(-1)));
        QVERIFY(saveAt(QStringLiteral("Third"), baseTime.addSecs(-2)));

        const auto before = storage.noteList();
        QCOMPARE(before.size(), 3);
        QCOMPARE(before.at(0).title(), QStringLiteral("First"));
        QCOMPARE(before.at(1).title(), QStringLiteral("Second"));
        QCOMPARE(before.at(2).title(), QStringLiteral("Third"));

        QHash<QString, QByteArray> contents;
        for (const auto &note : before) {
            QFile file(note.backendValue(QStringLiteral("fileName")).toString());
            QVERIFY(file.open(QIODevice::ReadOnly));
            contents.insert(note.id(), file.readAll());
        }

        QSignalSpy invalidated(&storage, &NoteStorage::invalidated);
        auto      *job = storage.reorderNotesAsync({ before.at(0).id(), before.at(1).id() }, before.at(2).id());
        QVERIFY(job->isFinished());
        QCOMPARE(job->state(), StorageJob::Succeeded);
        QCOMPARE(invalidated.size(), 1);
        delete job;

        const auto after = storage.noteList();
        QCOMPARE(after.size(), 3);
        QCOMPARE(after.at(0).title(), QStringLiteral("Third"));
        QCOMPARE(after.at(1).title(), QStringLiteral("First"));
        QCOMPARE(after.at(2).title(), QStringLiteral("Second"));
        for (const auto &note : after) {
            QFile file(note.backendValue(QStringLiteral("fileName")).toString());
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), contents.value(note.id()));
        }
    }

    void reorderUsesTheGapBeforeMovingNeighborTimestamps()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        const auto baseTime = QDateTime::currentDateTimeUtc().addSecs(-10);
        const auto saveAt   = [&storage](const QString &title, const QDateTime &modified) {
            Note note = storage.createNote();
            note.setTitle(title);
            note.setText(title + QStringLiteral(" body"), Note::Markdown);
            note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), modified);
            return storage.saveNote(note);
        };
        QVERIFY(saveAt(QStringLiteral("First"), baseTime));
        QVERIFY(saveAt(QStringLiteral("Second"), baseTime.addSecs(-4)));
        QVERIFY(saveAt(QStringLiteral("Third"), baseTime.addSecs(-8)));

        const auto before       = storage.noteList();
        const auto firstTimeMs  = before.at(0).lastChangeUTC().toMSecsSinceEpoch();
        const auto secondTimeMs = before.at(1).lastChangeUTC().toMSecsSinceEpoch();
        QSignalSpy invalidated(&storage, &NoteStorage::invalidated);

        auto *job = storage.reorderNoteAsync(before.at(2).id(), before.at(0).id());
        QVERIFY(job->isFinished());
        QCOMPARE(job->state(), StorageJob::Succeeded);
        QCOMPARE(invalidated.size(), 1);
        delete job;

        const auto after = storage.noteList();
        QCOMPARE(after.at(0).title(), QStringLiteral("First"));
        QCOMPARE(after.at(1).title(), QStringLiteral("Third"));
        QCOMPARE(after.at(2).title(), QStringLiteral("Second"));
        QCOMPARE(after.at(0).lastChangeUTC().toMSecsSinceEpoch(), firstTimeMs);
        QCOMPARE(after.at(2).lastChangeUTC().toMSecsSinceEpoch(), secondTimeMs);
        QVERIFY(after.at(1).lastChangeUTC() < after.at(0).lastChangeUTC());
        QVERIFY(after.at(1).lastChangeUTC() > after.at(2).lastChangeUTC());
    }

    void asyncSaveReturnsFinalIdForSubsequentReorder()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        PTFStorage storage;
        QVERIFY(storage.setStoragePath(directory.path()));
        const auto baseTime = QDateTime::currentDateTimeUtc().addSecs(-10);
        const auto saveAt   = [&storage](const QString &title, const QDateTime &modified) {
            Note note = storage.createNote();
            note.setTitle(title);
            note.setText(title + QStringLiteral(" body"), Note::Markdown);
            note.setBackendValue(QString::fromLatin1(RequestedModificationTimeBackendKey), modified);
            return storage.saveNote(note);
        };
        QVERIFY(saveAt(QStringLiteral("First"), baseTime));
        QVERIFY(saveAt(QStringLiteral("Last"), baseTime.addSecs(-1)));
        const auto before = storage.noteList();
        QCOMPARE(before.size(), 2);

        Note transferred = storage.createNote();
        QVERIFY(transferred.id().isEmpty());
        transferred.setTitle(QStringLiteral("Transferred"));
        transferred.setText(QStringLiteral("Transferred body"), Note::Markdown);
        auto *saveJob = storage.saveNoteAsync(transferred);
        QTRY_VERIFY(saveJob->isFinished());
        QCOMPARE(saveJob->state(), StorageJob::Succeeded);
        const QString finalId = saveJob->result().id();
        QVERIFY(!finalId.isEmpty());
        const auto savedNotes = storage.noteList();
        QVERIFY(std::ranges::any_of(savedNotes, [&finalId](const Note &note) { return note.id() == finalId; }));
        delete saveJob;

        auto *reorderJob = storage.reorderNoteAsync(finalId, before.constLast().id());
        QVERIFY(reorderJob->isFinished());
        QCOMPARE(reorderJob->state(), StorageJob::Succeeded);
        delete reorderJob;

        const auto after = storage.noteList();
        QCOMPARE(after.size(), 3);
        QCOMPARE(after.constLast().id(), finalId);
        QCOMPARE(after.constLast().title(), QStringLiteral("Transferred"));
    }
};

QTEST_GUILESS_MAIN(PTFStorageTest)

#include "ptfstorage_test.moc"
