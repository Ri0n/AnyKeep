#include <QFocusEvent>
#include <QQuickWidget>
#include <QSignalSpy>
#include <QtTest>

#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "noteblockmodel.h"
#include "noteeditor.h"

#include "editortestsupport.h"

using namespace AnyKeep;
using namespace AnyKeep::TestSupport;

class DesktopNoteEditorHostTest : public QObject {
    Q_OBJECT

private slots:
    void modelAndControllerStayShared()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        editor.model()->setBlockText(0, QStringLiteral("Changed\nBody"));
        QCOMPARE(editor.text(), QStringLiteral("Changed\nBody"));
        QVERIFY(editor.isDirty());

        editor.setMarkdown(true);
        QVERIFY(editor.isMarkdown());
        QVERIFY(host.model()->markdown());
    }

    void structuralCommandsUseTheCommonHistory()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        editor.setMarkdown(true);
        const int before = editor.model()->rowCount();
        host.insertTable();
        QCoreApplication::processEvents();
        QCOMPARE(editor.model()->rowCount(), before + 1);
        QVERIFY(editor.canUndo());
        QVERIFY(editor.undo());
        QCOMPARE(editor.model()->rowCount(), before);
    }

    void focusAdapterEmitsCheckpointSignal()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);
        QSignalSpy            received(&host, &DesktopNoteEditorHost::focusReceived);
        QSignalSpy            lost(&host, &DesktopNoteEditorHost::focusLost);

        QFocusEvent focusIn(QEvent::FocusIn, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(host.quickWidget(), &focusIn);
        QCOMPARE(received.size(), 1);

        QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(host.quickWidget(), &focusOut);
        QCOMPARE(lost.size(), 1);
    }
};

QTEST_MAIN(DesktopNoteEditorHostTest)

#include "desktopnoteeditorhost_test.moc"
