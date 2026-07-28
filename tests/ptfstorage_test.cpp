#include <QtTest>

#include "ptfstorage.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

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
};

QTEST_GUILESS_MAIN(PTFStorageTest)

#include "ptfstorage_test.moc"
