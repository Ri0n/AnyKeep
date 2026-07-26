#include <QtTest>

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
        drafts.insert(record.id, record);
        return {};
    }

    DraftStoreResult<DraftRecord> load(const QUuid &id) const override
    {
        const auto draft = drafts.constFind(id);
        if (draft == drafts.cend())
            return { {}, { DraftStoreError::NotFound, QStringLiteral("not found") } };
        return { draft.value(), {} };
    }

    DraftStoreResult<QList<DraftRecord>> records() const override { return { drafts.values(), {} }; }

    DraftStoreError transition(const QUuid &id, DraftRecord::State state) override
    {
        auto draft = drafts.find(id);
        if (draft == drafts.end())
            return { DraftStoreError::NotFound, QStringLiteral("not found") };
        draft->state = state;
        return {};
    }

    DraftStoreError remove(const QUuid &id) override
    {
        return drafts.remove(id) ? DraftStoreError {}
                                 : DraftStoreError { DraftStoreError::NotFound, QStringLiteral("not found") };
    }

    QHash<QUuid, DraftRecord> drafts;
};

Note plainNote(const QString &title = QStringLiteral("Title"), const QString &body = QStringLiteral("Body"))
{
    Note note(new NoteData(nullptr));
    note.setTitle(title);
    note.setText(body, Note::PlainText);
    return note;
}
} // namespace

class NoteEditorTest : public QObject {
    Q_OBJECT

private slots:
    void noteCopiesDetachBeforeMutation()
    {
        const auto original = plainNote();
        auto       edited   = original;

        edited.setTitle(QStringLiteral("Changed"));
        edited.setText(QStringLiteral("New body"), Note::PlainText);
        edited.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        edited.setBackendValue(QStringLiteral("revision"), 2);

        QCOMPARE(original.title(), QStringLiteral("Title"));
        QCOMPARE(original.text(), QStringLiteral("Body"));
        QVERIFY(!original.lastChangeUTC().isValid());
        QVERIFY(!original.backendValue(QStringLiteral("revision")).isValid());
        QCOMPARE(edited.title(), QStringLiteral("Changed"));
        QCOMPARE(edited.text(), QStringLiteral("New body"));
    }

    void unchangedEditorDoesNotCreateDraft()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        auto        *data  = store.get();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(), drafts);

        QCOMPARE(editor.text(), QStringLiteral("Title\nBody"));
        QVERIFY(!editor.isDirty());
        QVERIFY(editor.close());
        QVERIFY(data->drafts.isEmpty());
    }

    void checkpointThenCloseTransitionsDraft()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        auto        *data  = store.get();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(), drafts);

        editor.setText(QStringLiteral("Changed\nNew body"));
        QVERIFY(editor.isDirty());
        QVERIFY(editor.save());
        QVERIFY(!editor.isDirty());

        const auto checkpoint = data->drafts.value(editor.draftId());
        QCOMPARE(checkpoint.state, DraftRecord::Editing);
        QCOMPARE(checkpoint.title, QStringLiteral("Changed"));
        QCOMPARE(checkpoint.body, QStringLiteral("New body"));
        QCOMPARE(checkpoint.revision, quint64(1));

        QVERIFY(editor.close());
        QCOMPARE(data->drafts.value(editor.draftId()).state, DraftRecord::NeedsRouting);
    }

    void modelEditIsCheckpointed()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        auto        *data  = store.get();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(), drafts);

        editor.model()->setContents(QStringLiteral("Model title\nModel body"));

        QCOMPARE(editor.text(), QStringLiteral("Model title\nModel body"));
        QVERIFY(editor.isDirty());
        QVERIFY(editor.save());
        const auto checkpoint = data->drafts.value(editor.draftId());
        QCOMPARE(checkpoint.title, QStringLiteral("Model title"));
        QCOMPARE(checkpoint.body, QStringLiteral("Model body"));
    }

    void ownsDocumentHistoryWithoutAWidget()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(), drafts);

        editor.model()->setBlockText(0, QStringLiteral("Changed\nBody"));
        QVERIFY(editor.canUndo());
        QVERIFY(editor.undo());
        QCOMPARE(editor.text(), QStringLiteral("Title\nBody"));
        QVERIFY(editor.canRedo());
        QVERIFY(editor.redo());
        QCOMPARE(editor.text(), QStringLiteral("Changed\nBody"));
    }

    void sharedEditorsPublishOnlyAfterLastClose()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        auto        *data  = store.get();
        DraftManager drafts(std::move(store));
        const auto   note = plainNote();
        NoteEditor   first(note, drafts);
        NoteEditor   second(note, drafts, first.draftId());

        first.setText(QStringLiteral("Shared\nRevision"));
        QVERIFY(first.save());
        QVERIFY(second.reloadNewerDraft());
        QCOMPARE(second.text(), QStringLiteral("Shared\nRevision"));

        QVERIFY(first.close());
        QCOMPARE(data->drafts.value(first.draftId()).state, DraftRecord::Editing);
        QVERIFY(second.close());
        QCOMPARE(data->drafts.value(first.draftId()).state, DraftRecord::NeedsRouting);
    }
};

QTEST_GUILESS_MAIN(NoteEditorTest)

#include "noteeditor_test.moc"
