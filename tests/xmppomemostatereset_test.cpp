#include <QtTest>

#include "../plugins/xmpppubsub/storage/omemostateresetpolicy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

using AnyKeep::XmppOmemoStateReset::discard;
using AnyKeep::XmppOmemoStateReset::restore;
using AnyKeep::XmppOmemoStateReset::stage;

class XmppOmemoStateResetTest final : public QObject {
    Q_OBJECT

    static bool put(const QString &path)
    {
        if (!QDir().mkpath(QFileInfo(path).absolutePath()))
            return false;
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write("state", 5) == 5;
    }

private slots:
    void missingStateDoesNotCreateFiles()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QString staged, error;
        QVERIFY(stage(directory.filePath(QStringLiteral("xmpp-omemo-device.state")), &staged, &error));
        QVERIFY(staged.isEmpty());
        QVERIFY(error.isEmpty());
        QVERIFY(discard(staged));
    }

    void stageAndRollbackOnlyOwnOmemoFiles()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString state = directory.filePath(QStringLiteral("xmpp-omemo-12345678.state"));
        const QStringList suffixes { QString {}, QStringLiteral(".trust"), QStringLiteral(".iris"),
                                     QStringLiteral(".iris.trust"), QStringLiteral(".iris.jinglepub") };
        for (const auto &suffix : suffixes)
            QVERIFY(put(state + suffix));
        const QString ptf = directory.filePath(QStringLiteral("ptf/Note.md"));
        const QString noteKey = directory.filePath(QStringLiteral("xmpp-storage-master-key"));
        const QString differentAccount = directory.filePath(QStringLiteral("xmpp-omemo-99999.state"));
        const QString localMedia = directory.filePath(QStringLiteral("media/blob.blob"));
        QVERIFY(put(ptf));
        QVERIFY(put(noteKey));
        QVERIFY(put(differentAccount));
        QVERIFY(put(localMedia));

        QString staged, error;
        QVERIFY2(stage(state, &staged, &error), qPrintable(error));
        QVERIFY(!staged.isEmpty());
        for (const auto &suffix : suffixes) {
            QVERIFY(!QFileInfo::exists(state + suffix));
            QVERIFY(QFileInfo::exists(QDir(staged).filePath(QFileInfo(state).fileName() + suffix)));
        }
        QVERIFY(QFileInfo::exists(ptf));
        QVERIFY(QFileInfo::exists(noteKey));
        QVERIFY(QFileInfo::exists(differentAccount));
        QVERIFY(QFileInfo::exists(localMedia));

        QVERIFY(restore(state, staged));
        for (const auto &suffix : suffixes)
            QVERIFY(QFileInfo::exists(state + suffix));
        QVERIFY(!QDir(staged).exists());
    }

    void successfulResetDiscardsOldSessionState()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString state = directory.filePath(QStringLiteral("xmpp-omemo-current.state"));
        QVERIFY(put(state));
        QVERIFY(put(state + QStringLiteral(".iris")));
        QString staged, error;
        QVERIFY2(stage(state, &staged, &error), qPrintable(error));
        QVERIFY(!staged.isEmpty());
        QVERIFY(discard(staged));
        QVERIFY(!QDir(staged).exists());
        QVERIFY(!QFileInfo::exists(state));
        QVERIFY(!QFileInfo::exists(state + QStringLiteral(".iris")));
    }

    void rejectsSymbolicLinkWithoutTouchingState()
    {
#if defined(Q_OS_WIN)
        QSKIP("Unprivileged Windows builds cannot reliably create symlinks");
#else
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString state = directory.filePath(QStringLiteral("xmpp-omemo-current.state"));
        const QString outside = directory.filePath(QStringLiteral("other.key"));
        QVERIFY(put(outside));
        QVERIFY(QFile::link(outside, state));
        QString staged, error;
        QVERIFY(!stage(state, &staged, &error));
        QVERIFY(staged.isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(QFileInfo::exists(outside));
        QVERIFY(QFileInfo(state).isSymLink());
#endif
    }
};

QTEST_MAIN(XmppOmemoStateResetTest)
#include "xmppomemostatereset_test.moc"
