#include <QtTest>

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QTextBlock>
#include <QTextDocument>

#include "draftmanager.h"
#include "draftstore.h"
#include "noteblockmodel.h"
#include "notedata.h"
#include "noteeditor.h"
#include "notehighlighter.h"
#include "noterule.h"
#include "spellcheckprovider.h"
#include "notetransfercontroller.h"

using namespace AnyKeep;

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

class RejectAllSpellCheckProvider final : public SpellCheckProvider {
public:
    QString id() const override { return QStringLiteral("test"); }
    QString displayName() const override { return QStringLiteral("Test"); }
    bool isValid() const override { return true; }
    bool isCorrect(const QString &) const override { return false; }
    QStringList suggestions(const QString &) const override { return {}; }
    void addToDictionary(const QString &) override { }

protected:
    void onDisabled(DisableMode) override { }
};
} // namespace

class NoteEditorTest : public QObject {
    Q_OBJECT

private slots:
    void spellCheckSkipsInlineCode()
    {
        QTextDocument document;
        document.setMarkdown(QStringLiteral("misspelled `inlinecode` misspelled"));
        NoteHighlighter highlighter(&document);
        const auto extension = makeSpellCheckExtension(std::make_shared<RejectAllSpellCheckProvider>());
        highlighter.addExtension(extension, NoteHighlighter::SpellCheck);
        highlighter.rehighlight();

        const QString plain       = document.toPlainText();
        const int     inlineStart = plain.indexOf(QStringLiteral("inlinecode"));
        int           spellingRanges = 0;
        for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
            if (!block.layout())
                continue;
            for (const auto &range : block.layout()->formats()) {
                if (!range.format.property(SpellCheckFormatProperty).toBool())
                    continue;
                ++spellingRanges;
                const int start = block.position() + range.start;
                QVERIFY(start < inlineStart || start >= inlineStart + 10);
            }
        }
        QCOMPARE(spellingRanges, 2);
    }

    void noteCopiesDetachBeforeMutation()
    {
        const auto original = plainNote();
        auto       edited   = original;

        edited.setTitle(QStringLiteral("Changed"));
        edited.setText(QStringLiteral("New body"), Note::PlainText);
        edited.setFolderId(QUuid::createUuid());
        edited.setLastChangeUTC(QDateTime::currentDateTimeUtc());
        edited.setBackendValue(QStringLiteral("revision"), 2);

        QCOMPARE(original.title(), QStringLiteral("Title"));
        QCOMPARE(original.text(), QStringLiteral("Body"));
        QVERIFY(original.folderId().isNull());
        QVERIFY(!original.lastChangeUTC().isValid());
        QVERIFY(!original.backendValue(QStringLiteral("revision")).isValid());
        QCOMPARE(edited.title(), QStringLiteral("Changed"));
        QCOMPARE(edited.text(), QStringLiteral("New body"));
        QVERIFY(!edited.folderId().isNull());
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

    void markdownTagLineIsCheckpointedAsTags()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QString(), Note::Markdown);

        auto         store = std::make_unique<MemoryDraftStore>();
        auto        *data  = store.get();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(note, drafts);

        editor.setText(QStringLiteral("Title\n\n*tb *interview\n\nhello"));
        QVERIFY(editor.save());
        const auto record = data->drafts.value(editor.draftId());
        QCOMPARE(record.body, QStringLiteral("*tb *interview\n\nhello"));
        QCOMPARE(record.tags, QStringList({ QStringLiteral("tb"), QStringLiteral("interview") }));

        NoteRule rule;
        rule.id         = QUuid::createUuid();
        rule.name       = QStringLiteral("Tomboy");
        rule.revision   = 1;
        rule.modifiedAt = QDateTime::currentDateTimeUtc();
        rule.conditions = { { NoteRuleConditionKind::HasTag, QStringLiteral("*tb"), false } };
        NoteRuleAction action;
        action.kind      = NoteRuleActionKind::SelectStorage;
        action.storageId = QStringLiteral("tomboy");
        rule.actions     = { action };

        NoteRuleEvaluationInput input;
        input.storageId       = QStringLiteral("ptf");
        input.noteId          = record.id.toString(QUuid::WithoutBraces);
        input.title           = record.title;
        input.tags            = record.tags;
        input.text            = record.body;
        input.textAvailable   = true;
        const auto evaluation = NoteRuleEvaluator::evaluate({ rule }, input);
        QVERIFY2(evaluation, qPrintable(evaluation.error.message));
        QCOMPARE(evaluation.matchedRuleIds, QList<QUuid> { rule.id });
        QCOMPARE(evaluation.storageId, QStringLiteral("tomboy"));
    }

    void plainTextSecondLineIsCheckpointedAsTags()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        auto        *data  = store.get();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(QStringLiteral("Title"), QString()), drafts);

        editor.setText(QStringLiteral("Title\n*tb *interview\nhello"));
        QVERIFY(editor.save());
        const auto record = data->drafts.value(editor.draftId());
        QCOMPARE(record.body, QStringLiteral("*tb *interview\nhello"));
        QCOMPARE(record.tags, QStringList({ QStringLiteral("tb"), QStringLiteral("interview") }));
    }

    void programmaticReplacementSynchronizesStructuralModel()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QString());
        note.setText(QString(), Note::Markdown);

        auto         store = std::make_unique<MemoryDraftStore>();
        auto        *data  = store.get();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(note, drafts);

        editor.setText(QStringLiteral("Selected title\n\nSelected body"));
        QCOMPARE(editor.model()->contents(), QStringLiteral("Selected title\n\nSelected body"));
        QVERIFY(editor.isDirty());
        QVERIFY(editor.save());
        const auto record = data->drafts.value(editor.draftId());
        QCOMPARE(record.title, QStringLiteral("Selected title"));
        QCOMPARE(record.body, QStringLiteral("Selected body"));
        QCOMPARE(record.format, Note::Markdown);
    }

    void copiesStructuredSelectionAsMarkdownPlainText()
    {
        Note note(new NoteData(nullptr));
        note.setTitle(QStringLiteral("Title"));
        note.setText(QStringLiteral("| **cell** | Other |\n| --- | --- |\n| value | value |"), Note::Markdown);

        auto         store = std::make_unique<MemoryDraftStore>();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(note, drafts);
        QCOMPARE(editor.model()->blockTypeAt(1), int(NoteBlockModel::Table));

        const QVariantList ranges { QVariantMap { { QStringLiteral("blockIndex"), 1 },
                                                  { QStringLiteral("listItemIndex"), -1 },
                                                  { QStringLiteral("tableCellIndex"), 0 },
                                                  { QStringLiteral("markdown"), QStringLiteral("**cell**") },
                                                  { QStringLiteral("wholeEditor"), true },
                                                  { QStringLiteral("before"), QString() },
                                                  { QStringLiteral("after"), QString() } } };

        QVERIFY(editor.copySelectionToClipboard(ranges));
        QCOMPARE(QGuiApplication::clipboard()->mimeData()->text(), QStringLiteral("cell"));

        QVERIFY(editor.copySelectionAsMarkdownToClipboard(ranges));
        const QMimeData *mimeData = QGuiApplication::clipboard()->mimeData();
        QVERIFY(mimeData);
        const QString markdown
            = QString::fromUtf8(mimeData->data(QString::fromLatin1(NoteTransferController::MarkdownMimeType)));
        QCOMPARE(mimeData->text(), markdown);
        QVERIFY2(markdown.startsWith(QStringLiteral("| cell |\n| --- |")), qPrintable(markdown));
        QVERIFY(mimeData->hasFormat(QString::fromLatin1(NoteTransferController::FragmentMimeType)));

        editor.copyMarkdownAsPlainTextToClipboard(QStringLiteral("**bold**"));
        mimeData = QGuiApplication::clipboard()->mimeData();
        QVERIFY(mimeData);
        QCOMPARE(mimeData->text(), QStringLiteral("**bold**"));
        QCOMPARE(QString::fromUtf8(mimeData->data(QString::fromLatin1(NoteTransferController::MarkdownMimeType))),
                 QStringLiteral("**bold**"));
    }

    void discardAndCloseRemovesPersistedUnpublishedDraft()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        auto        *data  = store.get();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(), drafts);

        QVERIFY(editor.noteId().isEmpty());
        editor.setText(QStringLiteral("Temporary\nSelection"));
        QVERIFY(editor.save());
        QVERIFY(data->drafts.contains(editor.draftId()));
        QVERIFY(editor.discardAndClose());
        QVERIFY(!data->drafts.contains(editor.draftId()));
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

    void folderOnlyChangeIsCheckpointed()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        auto        *data  = store.get();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(), drafts);
        const auto   folder = QUuid::createUuid();

        editor.setFolderId(folder);
        QVERIFY(editor.isDirty());
        QVERIFY(editor.save());
        QVERIFY(!editor.isDirty());
        QCOMPARE(data->drafts.value(editor.draftId()).folderId, folder);

        editor.setFolderId({});
        QVERIFY(editor.isDirty());
        QVERIFY(editor.save());
        QVERIFY(data->drafts.value(editor.draftId()).folderId.isNull());
    }

    void explicitFolderChoiceIsCheckpointed()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        auto        *data  = store.get();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(), drafts);

        editor.setFolderId(QUuid::createUuid());
        editor.setFolderUserOverride();
        QVERIFY(editor.save());
        QVERIFY(data->drafts.value(editor.draftId()).folderUserOverride);
    }

    void metadataOnlyFolderPersistenceLeavesContentDirtyStateIntact()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(), drafts);
        const auto   folder = QUuid::createUuid();
        QSignalSpy   folderChanged(&editor, &NoteEditor::folderIdChanged);

        editor.setFolderId(folder);
        QCOMPARE(folderChanged.count(), 1);
        QVERIFY(editor.isDirty());
        editor.markFolderPersisted(folder);
        QVERIFY(!editor.isDirty());

        editor.setText(QStringLiteral("Changed\nBody"));
        QVERIFY(editor.isDirty());
        editor.markFolderPersisted(folder);
        QVERIFY(editor.isDirty());
    }

    void editingDraftRestoresFolderMetadata()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        auto        *data  = store.get();
        DraftManager drafts(std::move(store));
        DraftRecord  draft;
        draft.id       = QUuid::createUuid();
        draft.title    = QStringLiteral("Draft title");
        draft.body     = QStringLiteral("Draft body");
        draft.format   = Note::PlainText;
        draft.folderId = QUuid::createUuid();
        draft.revision = 1;
        data->drafts.insert(draft.id, draft);

        NoteEditor editor(plainNote(), drafts, draft.id);
        QCOMPARE(editor.folderId(), draft.folderId);
        QVERIFY(!editor.isDirty());
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

    void codeLanguageChangesParticipateInUndo()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(), drafts);

        editor.setMarkdown(true);
        editor.model()->insertCodeBlock(1, QStringLiteral("python"));
        editor.resetHistory();

        editor.model()->setCodeLanguage(1, QStringLiteral("cpp"));
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::LanguageRole).toString(),
                 QStringLiteral("cpp"));
        QVERIFY(editor.canUndo());
        QVERIFY(editor.undo());
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::LanguageRole).toString(),
                 QStringLiteral("python"));
        QVERIFY(editor.redo());
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::LanguageRole).toString(),
                 QStringLiteral("cpp"));
    }

    void audioTranscriptChangesParticipateInUndo()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(), drafts);

        editor.setMarkdown(true);
        const QString source = QStringLiteral("anykeep-media:/00000000-0000-0000-0000-000000000001/audio.m4a");
        editor.model()->insertAudio(1, source, QStringLiteral("Voice memo"), 2500);
        editor.model()->insertAudio(2, source, QStringLiteral("Duplicate voice memo"), 2500);
        editor.resetHistory();

        QVERIFY(editor.setAudioTranscript(2, QStringLiteral("Recognized text")));
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::AudioTranscriptRole).toString(),
                 QString());
        QCOMPARE(editor.model()->data(editor.model()->index(2), NoteBlockModel::AudioTranscriptRole).toString(),
                 QStringLiteral("Recognized text"));
        QVERIFY(editor.canUndo());
        QVERIFY(editor.undo());
        QCOMPARE(editor.model()->data(editor.model()->index(2), NoteBlockModel::AudioTranscriptRole).toString(),
                 QString());
        QVERIFY(editor.redo());
        QCOMPARE(editor.model()->data(editor.model()->index(2), NoteBlockModel::AudioTranscriptRole).toString(),
                 QStringLiteral("Recognized text"));
    }

    void imagePresentationChangesParticipateInUndo()
    {
        auto         store = std::make_unique<MemoryDraftStore>();
        DraftManager drafts(std::move(store));
        NoteEditor   editor(plainNote(), drafts);

        editor.setMarkdown(true);
        editor.model()->insertImage(1, QStringLiteral("media://image"), QStringLiteral("Image"));
        editor.resetHistory();

        editor.beginHistoryTransaction(QStringLiteral("image-presentation"), {});
        editor.model()->setImageWidth(1, 280);
        editor.model()->setImageAlignment(1, QStringLiteral("right"));
        editor.endHistoryTransaction({});
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::ImageWidthRole).toInt(), 280);
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::ImageAlignmentRole).toString(),
                 QStringLiteral("right"));

        QVERIFY(editor.undo());
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::ImageWidthRole).toInt(), 0);
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::ImageAlignmentRole).toString(),
                 QStringLiteral("center"));
        QVERIFY(editor.redo());
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::ImageWidthRole).toInt(), 280);
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::ImageAlignmentRole).toString(),
                 QStringLiteral("right"));
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

QTEST_MAIN(NoteEditorTest)

#include "noteeditor_test.moc"
