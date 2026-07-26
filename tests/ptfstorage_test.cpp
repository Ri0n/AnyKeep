#include <QtTest>

#include "ptfstorage.h"

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
};

QTEST_GUILESS_MAIN(PTFStorageTest)

#include "ptfstorage_test.moc"
