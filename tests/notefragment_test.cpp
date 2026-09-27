#include "notefragment.h"

#include <QTest>

using namespace AnyKeep;

class NoteFragmentTest : public QObject {
    Q_OBJECT

private slots:
    void roundTripsStructuredFragment();
    void roundTripsCodeBlock();
    void roundTripsTagLine();
    void roundTripsTimedMediaBlock();
    void roundTripsAttachmentBlock();
    void rejectsObsoleteFragmentVersions();
    void rejectsInvalidInput();
};

void NoteFragmentTest::roundTripsStructuredFragment()
{
    NoteFragment fragment;
    fragment.kind         = NoteFragmentKind::BlockSequence;
    fragment.sourceFormat = NoteFragmentSourceFormat::Markdown;

    NoteFragmentBlock heading;
    heading.type         = NoteFragmentBlockType::Heading;
    heading.markdown     = QStringLiteral("**Release** notes");
    heading.headingLevel = 2;
    fragment.blocks.append(heading);

    NoteFragmentBlock list;
    list.type      = NoteFragmentBlockType::List;
    list.listItems = {
        { QStringLiteral("first"), 0, NoteFragmentListKind::Check, true },
        { QStringLiteral("*nested*"), 1, NoteFragmentListKind::Numbered, false },
    };
    fragment.blocks.append(list);

    NoteFragmentBlock table;
    table.type             = NoteFragmentBlockType::Table;
    table.table.rows       = 2;
    table.table.columns    = 2;
    table.table.headerRows = 1;
    table.table.markdownCells
        = { QStringLiteral("Name"), QStringLiteral("Status"), QStringLiteral("Alice"), QStringLiteral("**Ready**") };
    fragment.blocks.append(table);

    NoteFragmentBlock visual;
    visual.type               = NoteFragmentBlockType::Media;
    visual.media.sourceUri    = QStringLiteral("anykeep-media:/11111111-1111-1111-1111-111111111111/picture.png");
    visual.media.title        = QStringLiteral("Diagram");
    visual.media.mediaType    = QStringLiteral("image/png");
    visual.media.pixelWidth   = 1280;
    visual.media.pixelHeight  = 720;
    visual.media.displayWidth = 360;
    visual.media.alignment    = QStringLiteral("right");
    fragment.blocks.append(visual);

    NoteFragmentMedia media;
    media.sourceUri              = visual.media.sourceUri;
    media.reference.id           = QUuid(QStringLiteral("{11111111-1111-1111-1111-111111111111}"));
    media.reference.blobId       = QByteArray::fromHex("abcdef");
    media.reference.originalName = QStringLiteral("Picture.png");
    media.reference.portableName = QStringLiteral("picture.png");
    media.reference.mediaType    = QStringLiteral("image/png");
    media.reference.size         = 3;
    media.reference.checksum     = QByteArray::fromHex("012345");
    media.reference.remoteData.insert(QStringLiteral("revision"), 7);
    media.data = QByteArray("PNG");
    fragment.media.append(media);

    const NoteFragmentDecodeResult decoded = decodeNoteFragment(encodeNoteFragment(fragment));
    QVERIFY2(decoded, qPrintable(decoded.error));
    QCOMPARE(decoded.fragment.version, fragment.version);
    QCOMPARE(decoded.fragment.kind, fragment.kind);
    QCOMPARE(decoded.fragment.sourceFormat, fragment.sourceFormat);
    QCOMPARE(decoded.fragment.blocks.size(), 4);
    QCOMPARE(decoded.fragment.blocks.at(0).markdown, heading.markdown);
    QCOMPARE(decoded.fragment.blocks.at(0).headingLevel, 2);
    QCOMPARE(decoded.fragment.blocks.at(1).listItems.size(), 2);
    QCOMPARE(decoded.fragment.blocks.at(1).listItems.at(1).indent, 1);
    QCOMPARE(decoded.fragment.blocks.at(1).listItems.at(1).kind, NoteFragmentListKind::Numbered);
    QCOMPARE(decoded.fragment.blocks.at(2).table.markdownCells, table.table.markdownCells);
    QCOMPARE(decoded.fragment.blocks.at(3).type, NoteFragmentBlockType::Media);
    QCOMPARE(decoded.fragment.blocks.at(3).media.title, visual.media.title);
    QCOMPARE(decoded.fragment.blocks.at(3).media.mediaType, visual.media.mediaType);
    QCOMPARE(decoded.fragment.blocks.at(3).media.pixelWidth, visual.media.pixelWidth);
    QCOMPARE(decoded.fragment.blocks.at(3).media.pixelHeight, visual.media.pixelHeight);
    QCOMPARE(decoded.fragment.blocks.at(3).media.displayWidth, visual.media.displayWidth);
    QCOMPARE(decoded.fragment.blocks.at(3).media.alignment, visual.media.alignment);
    QCOMPARE(decoded.fragment.media.size(), 1);
    QCOMPARE(decoded.fragment.media.at(0).reference.id, media.reference.id);
    QCOMPARE(decoded.fragment.media.at(0).reference.remoteData, media.reference.remoteData);
    QCOMPARE(decoded.fragment.media.at(0).data, media.data);
}

void NoteFragmentTest::roundTripsCodeBlock()
{
    NoteFragment      fragment;
    NoteFragmentBlock code;
    code.type     = NoteFragmentBlockType::CodeBlock;
    code.language = QStringLiteral("cpp");
    code.markdown = QStringLiteral("if (value) {\n    return \"**literal**\";\n}\n");
    fragment.blocks.append(code);

    const auto decoded = decodeNoteFragment(encodeNoteFragment(fragment));
    QVERIFY2(decoded, qPrintable(decoded.error));
    QCOMPARE(decoded.fragment.version, NoteFragment::CurrentVersion);
    QCOMPARE(decoded.fragment.blocks.size(), 1);
    QCOMPARE(decoded.fragment.blocks.constFirst().type, NoteFragmentBlockType::CodeBlock);
    QCOMPARE(decoded.fragment.blocks.constFirst().language, code.language);
    QCOMPARE(decoded.fragment.blocks.constFirst().markdown, code.markdown);
}

void NoteFragmentTest::roundTripsTagLine()
{
    NoteFragment      fragment;
    NoteFragmentBlock tags;
    tags.type = NoteFragmentBlockType::TagLine;
    tags.tags = { QStringLiteral("tb"), QStringLiteral("interview") };
    fragment.blocks.append(tags);

    const auto decoded = decodeNoteFragment(encodeNoteFragment(fragment));
    QVERIFY2(decoded, qPrintable(decoded.error));
    QCOMPARE(decoded.fragment.version, NoteFragment::CurrentVersion);
    QCOMPARE(decoded.fragment.blocks.size(), 1);
    QCOMPARE(decoded.fragment.blocks.constFirst().type, NoteFragmentBlockType::TagLine);
    QCOMPARE(decoded.fragment.blocks.constFirst().tags, tags.tags);
}

void NoteFragmentTest::roundTripsTimedMediaBlock()
{
    NoteFragment      fragment;
    NoteFragmentBlock media;
    media.type               = NoteFragmentBlockType::Media;
    media.media.sourceUri    = QStringLiteral("anykeep-media:/11111111-1111-1111-1111-111111111111/recording.m4a");
    media.media.title        = QStringLiteral("Meeting note");
    media.media.mediaType    = QStringLiteral("audio/mp4");
    media.media.durationMs   = 91234;
    media.media.transcript   = QStringLiteral("First line\nSecond line");
    fragment.blocks.append(media);

    const auto decoded = decodeNoteFragment(encodeNoteFragment(fragment));
    QVERIFY2(decoded, qPrintable(decoded.error));
    QCOMPARE(decoded.fragment.version, NoteFragment::CurrentVersion);
    QCOMPARE(decoded.fragment.blocks.size(), 1);
    QCOMPARE(decoded.fragment.blocks.constFirst().type, NoteFragmentBlockType::Media);
    QCOMPARE(decoded.fragment.blocks.constFirst().media.sourceUri, media.media.sourceUri);
    QCOMPARE(decoded.fragment.blocks.constFirst().media.title, media.media.title);
    QCOMPARE(decoded.fragment.blocks.constFirst().media.mediaType, media.media.mediaType);
    QCOMPARE(decoded.fragment.blocks.constFirst().media.durationMs, media.media.durationMs);
    QCOMPARE(decoded.fragment.blocks.constFirst().media.transcript, media.media.transcript);
}

void NoteFragmentTest::roundTripsAttachmentBlock()
{
    NoteFragment      fragment;
    NoteFragmentBlock attachment;
    attachment.type                 = NoteFragmentBlockType::Attachment;
    attachment.attachment.sourceUri = QStringLiteral("anykeep-media:/22222222-2222-2222-2222-222222222222/spec.pdf");
    attachment.attachment.fileName  = QStringLiteral("Specification.pdf");
    attachment.attachment.mediaType = QStringLiteral("application/pdf");
    attachment.attachment.size      = 123456;
    fragment.blocks.append(attachment);

    const auto decoded = decodeNoteFragment(encodeNoteFragment(fragment));
    QVERIFY2(decoded, qPrintable(decoded.error));
    QCOMPARE(decoded.fragment.version, NoteFragment::CurrentVersion);
    QCOMPARE(decoded.fragment.blocks.size(), 1);
    QCOMPARE(decoded.fragment.blocks.constFirst().type, NoteFragmentBlockType::Attachment);
    QCOMPARE(decoded.fragment.blocks.constFirst().attachment.sourceUri, attachment.attachment.sourceUri);
    QCOMPARE(decoded.fragment.blocks.constFirst().attachment.fileName, attachment.attachment.fileName);
    QCOMPARE(decoded.fragment.blocks.constFirst().attachment.mediaType, attachment.attachment.mediaType);
    QCOMPARE(decoded.fragment.blocks.constFirst().attachment.size, attachment.attachment.size);
}

void NoteFragmentTest::rejectsObsoleteFragmentVersions()
{
    NoteFragment fragment;
    fragment.version = NoteFragment::CurrentVersion - 1;
    NoteFragmentBlock text;
    text.type     = NoteFragmentBlockType::Text;
    text.markdown = QStringLiteral("legacy");
    fragment.blocks.append(text);

    QCOMPARE(decodeNoteFragment(encodeNoteFragment(fragment)).error,
             QStringLiteral("unsupported fragment version"));
}

void NoteFragmentTest::rejectsInvalidInput()
{
    QCOMPARE(decodeNoteFragment(QByteArrayLiteral("not-cbor")).error, QStringLiteral("invalid CBOR fragment"));

    QCborMap wrongSchema;
    wrongSchema.insert(QStringLiteral("schema"), QStringLiteral("other"));
    QCOMPARE(decodeNoteFragment(QCborValue(wrongSchema).toCbor()).error, QStringLiteral("unknown fragment schema"));

    NoteFragment      invalidTable;
    NoteFragmentBlock table;
    table.type                = NoteFragmentBlockType::Table;
    table.table.rows          = 2;
    table.table.columns       = 2;
    table.table.markdownCells = { QStringLiteral("only one") };
    invalidTable.blocks.append(table);
    QVERIFY(!decodeNoteFragment(encodeNoteFragment(invalidTable)));

    NoteFragment      invalidList;
    NoteFragmentBlock list;
    list.type      = NoteFragmentBlockType::List;
    list.listItems = { { QStringLiteral("too deep"), 129, NoteFragmentListKind::Bullet, false } };
    invalidList.blocks.append(list);
    QVERIFY(!decodeNoteFragment(encodeNoteFragment(invalidList)));

    NoteFragment      invalidTags;
    NoteFragmentBlock tags;
    tags.type = NoteFragmentBlockType::TagLine;
    invalidTags.blocks.append(tags);
    QVERIFY(!decodeNoteFragment(encodeNoteFragment(invalidTags)));

    tags.tags          = { QStringLiteral("bad?") };
    invalidTags.blocks = { tags };
    QVERIFY(!decodeNoteFragment(encodeNoteFragment(invalidTags)));

    tags.tags          = { QStringLiteral("tb"), QStringLiteral("tb") };
    invalidTags.blocks = { tags };
    QVERIFY(!decodeNoteFragment(encodeNoteFragment(invalidTags)));

    NoteFragment      invalidPresentation;
    NoteFragmentBlock visual;
    visual.type               = NoteFragmentBlockType::Media;
    visual.media.sourceUri    = QStringLiteral("media://image");
    visual.media.mediaType    = QStringLiteral("image/png");
    visual.media.alignment    = QStringLiteral("diagonal");
    invalidPresentation.blocks.append(visual);
    QVERIFY(!decodeNoteFragment(encodeNoteFragment(invalidPresentation)));

    NoteFragment      invalidDuration;
    NoteFragmentBlock timed;
    timed.type               = NoteFragmentBlockType::Media;
    timed.media.sourceUri    = QStringLiteral("media://audio");
    timed.media.mediaType    = QStringLiteral("audio/ogg");
    timed.media.durationMs   = -1;
    invalidDuration.blocks.append(timed);
    QVERIFY(!decodeNoteFragment(encodeNoteFragment(invalidDuration)));

    NoteFragment      invalidAttachment;
    NoteFragmentBlock attachment;
    attachment.type                 = NoteFragmentBlockType::Attachment;
    attachment.attachment.sourceUri = QStringLiteral("media://attachment");
    attachment.attachment.size      = -1;
    invalidAttachment.blocks.append(attachment);
    QVERIFY(!decodeNoteFragment(encodeNoteFragment(invalidAttachment)));
}

QTEST_GUILESS_MAIN(NoteFragmentTest)

#include "notefragment_test.moc"
