#include <QQuickItem>
#include <QQuickWidget>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "draftstore.h"
#include "noteblockmodel.h"
#include "notedata.h"
#include "noteeditor.h"

using namespace QtNote;

namespace {
class MemoryDraftStore final : public DraftStore {
public:
    DraftStoreError write(const DraftRecord &record) override
    {
        records_.insert(record.id, record);
        return {};
    }
    DraftStoreResult<DraftRecord> load(const QUuid &id) const override
    {
        const auto it = records_.constFind(id);
        if (it == records_.cend())
            return { {}, { DraftStoreError::NotFound, QStringLiteral("not found") } };
        return { it.value(), {} };
    }
    DraftStoreResult<QList<DraftRecord>> records() const override { return { records_.values(), {} }; }
    DraftStoreError                      transition(const QUuid &id, DraftRecord::State state) override
    {
        auto it = records_.find(id);
        if (it == records_.end())
            return { DraftStoreError::NotFound, QStringLiteral("not found") };
        it->state = state;
        return {};
    }
    DraftStoreError remove(const QUuid &id) override
    {
        return records_.remove(id) ? DraftStoreError {}
                                   : DraftStoreError { DraftStoreError::NotFound, QStringLiteral("not found") };
    }

private:
    QHash<QUuid, DraftRecord> records_;
};

Note plainNote()
{
    Note note(new NoteData(nullptr));
    note.setTitle(QStringLiteral("Title"));
    note.setText(QStringLiteral("Body"), Note::PlainText);
    return note;
}
}

class DesktopNoteEditorHostTest : public QObject {
    Q_OBJECT

private slots:
    void loadsSharedQmlShell()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        auto *quick = host.quickWidget();
        QVERIFY(quick);
        QCOMPARE(quick->status(), QQuickWidget::Ready);
        QVERIFY(quick->rootObject());
        QVERIFY(quick->rootObject()->property("blockEditor").value<QObject *>());
        QCOMPARE(host.model(), editor.model());
    }

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
        QSignalSpy            lost(&host, &DesktopNoteEditorHost::focusLost);

        host.show();
        QTest::qWait(20);
        QFocusEvent event(QEvent::FocusOut, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(host.quickWidget(), &event);
        QCOMPARE(lost.size(), 1);
    }
};

QTEST_MAIN(DesktopNoteEditorHostTest)

#include "desktopnoteeditorhost_test.moc"
