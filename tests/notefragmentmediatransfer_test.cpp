#include "localmediastore.h"
#include "notefragmentmediatransfer.h"
#include "secureenvelope.h"

#include <QTemporaryDir>
#include <QTest>

using namespace QtNote;

class NoteFragmentMediaTransferTest : public QObject {
    Q_OBJECT

private slots:
    void clonesAttachmentAndRewritesImageUri();
    void clonesAttachmentAndRewritesAudioUri();
    void clonesGenericAttachmentAndRewritesUri();
    void usesEmbeddedDataWithoutSourceStore();
    void rejectsImageWithoutMedia();
};

static NoteFragment imageFragment(const MediaReference &reference, const QByteArray &data = {})
{
    NoteFragment      fragment;
    NoteFragmentBlock image;
    image.type            = NoteFragmentBlockType::Image;
    image.image.sourceUri = reference.uri();
    image.image.alt       = QStringLiteral("diagram");
    fragment.blocks.append(image);
    NoteFragmentMedia media;
    media.sourceUri = reference.uri();
    media.reference = reference;
    media.data      = data;
    fragment.media.append(media);
    return fragment;
}

static NoteFragment audioFragment(const MediaReference &reference)
{
    NoteFragment      fragment;
    NoteFragmentBlock audio;
    audio.type             = NoteFragmentBlockType::Audio;
    audio.audio.sourceUri  = reference.uri();
    audio.audio.title      = QStringLiteral("voice note");
    audio.audio.durationMs = 12345;
    fragment.blocks.append(audio);
    NoteFragmentMedia media;
    media.sourceUri = reference.uri();
    media.reference = reference;
    fragment.media.append(media);
    return fragment;
}

static NoteFragment attachmentFragment(const MediaReference &reference)
{
    NoteFragment      fragment;
    NoteFragmentBlock attachment;
    attachment.type                 = NoteFragmentBlockType::Attachment;
    attachment.attachment.sourceUri = reference.uri();
    attachment.attachment.fileName  = QStringLiteral("specification.pdf");
    attachment.attachment.mediaType = QStringLiteral("application/pdf");
    attachment.attachment.size      = reference.size;
    fragment.blocks.append(attachment);
    NoteFragmentMedia media;
    media.sourceUri = reference.uri();
    media.reference = reference;
    fragment.media.append(media);
    return fragment;
}

void NoteFragmentMediaTransferTest::clonesAttachmentAndRewritesImageUri()
{
    QTemporaryDir sourceDirectory;
    QTemporaryDir destinationDirectory;
    QVERIFY(sourceDirectory.isValid());
    QVERIFY(destinationDirectory.isValid());
    LocalMediaStore  source(sourceDirectory.path(), SecureEnvelope::generateMasterKey());
    LocalMediaStore  destination(destinationDirectory.path(), SecureEnvelope::generateMasterKey());
    const QByteArray bytes("image bytes");
    const auto       original = source.importData(bytes, QStringLiteral("diagram.png"), QStringLiteral("image/png"));
    QVERIFY2(original, qPrintable(original.error));

    NoteFragment fragment = imageFragment(original.value);
    fragment.media.first().reference.remoteData.insert(QStringLiteral("revision"), 7);
    const auto cloned = NoteFragmentMediaTransfer::cloneForDestination(fragment, destination, &source);
    QVERIFY2(cloned, qPrintable(cloned.error));
    QCOMPARE(cloned.importedMedia.size(), 1);
    QVERIFY(cloned.importedMedia.first().id != original.value.id);
    QVERIFY(cloned.importedMedia.first().remoteData.isEmpty());
    QCOMPARE(cloned.fragment.blocks.first().image.sourceUri, cloned.importedMedia.first().uri());
    QVERIFY(cloned.fragment.blocks.first().image.sourceUri != original.value.uri());
    const auto restored = destination.data(cloned.importedMedia.first().blobId);
    QVERIFY2(restored, qPrintable(restored.error));
    QCOMPARE(restored.value, bytes);
}

void NoteFragmentMediaTransferTest::clonesAttachmentAndRewritesAudioUri()
{
    QTemporaryDir sourceDirectory;
    QTemporaryDir destinationDirectory;
    QVERIFY(sourceDirectory.isValid());
    QVERIFY(destinationDirectory.isValid());
    LocalMediaStore  source(sourceDirectory.path(), SecureEnvelope::generateMasterKey());
    LocalMediaStore  destination(destinationDirectory.path(), SecureEnvelope::generateMasterKey());
    const QByteArray bytes("encoded m4a bytes");
    const auto       original = source.importData(bytes, QStringLiteral("recording.m4a"), QStringLiteral("audio/mp4"));
    QVERIFY2(original, qPrintable(original.error));

    const auto cloned
        = NoteFragmentMediaTransfer::cloneForDestination(audioFragment(original.value), destination, &source);
    QVERIFY2(cloned, qPrintable(cloned.error));
    QCOMPARE(cloned.importedMedia.size(), 1);
    QCOMPARE(cloned.fragment.blocks.first().audio.sourceUri, cloned.importedMedia.first().uri());
    QVERIFY(cloned.fragment.blocks.first().audio.sourceUri != original.value.uri());
    QCOMPARE(cloned.fragment.blocks.first().audio.durationMs, qint64(12345));
    const auto restored = destination.data(cloned.importedMedia.first().blobId);
    QVERIFY2(restored, qPrintable(restored.error));
    QCOMPARE(restored.value, bytes);
}

void NoteFragmentMediaTransferTest::clonesGenericAttachmentAndRewritesUri()
{
    QTemporaryDir sourceDirectory;
    QTemporaryDir destinationDirectory;
    QVERIFY(sourceDirectory.isValid());
    QVERIFY(destinationDirectory.isValid());
    LocalMediaStore  source(sourceDirectory.path(), SecureEnvelope::generateMasterKey());
    LocalMediaStore  destination(destinationDirectory.path(), SecureEnvelope::generateMasterKey());
    const QByteArray bytes("pdf bytes");
    const auto       original
        = source.importData(bytes, QStringLiteral("specification.pdf"), QStringLiteral("application/pdf"));
    QVERIFY2(original, qPrintable(original.error));

    const auto cloned
        = NoteFragmentMediaTransfer::cloneForDestination(attachmentFragment(original.value), destination, &source);
    QVERIFY2(cloned, qPrintable(cloned.error));
    QCOMPARE(cloned.importedMedia.size(), 1);
    QCOMPARE(cloned.fragment.blocks.first().attachment.sourceUri, cloned.importedMedia.first().uri());
    QVERIFY(cloned.fragment.blocks.first().attachment.sourceUri != original.value.uri());
    QCOMPARE(cloned.fragment.blocks.first().attachment.fileName, QStringLiteral("specification.pdf"));
    QCOMPARE(cloned.fragment.blocks.first().attachment.mediaType, QStringLiteral("application/pdf"));
    const auto restored = destination.data(cloned.importedMedia.first().blobId);
    QVERIFY2(restored, qPrintable(restored.error));
    QCOMPARE(restored.value, bytes);
}

void NoteFragmentMediaTransferTest::usesEmbeddedDataWithoutSourceStore()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore destination(directory.path(), SecureEnvelope::generateMasterKey());
    MediaReference  source;
    source.id           = QUuid::createUuid();
    source.blobId       = QByteArray::fromHex("cafe");
    source.originalName = QStringLiteral("portable.png");
    source.portableName = QStringLiteral("portable.png");
    source.mediaType    = QStringLiteral("image/png");
    source.size         = 4;

    const QByteArray data("data");
    const auto       cloned = NoteFragmentMediaTransfer::cloneForDestination(imageFragment(source, data), destination);
    QVERIFY2(cloned, qPrintable(cloned.error));
    QCOMPARE(cloned.importedMedia.size(), 1);
    const auto restored = destination.data(cloned.importedMedia.first().blobId);
    QVERIFY2(restored, qPrintable(restored.error));
    QCOMPARE(restored.value, data);
}

void NoteFragmentMediaTransferTest::rejectsImageWithoutMedia()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    LocalMediaStore   destination(directory.path(), SecureEnvelope::generateMasterKey());
    NoteFragment      fragment;
    NoteFragmentBlock image;
    image.type            = NoteFragmentBlockType::Image;
    image.image.sourceUri = QStringLiteral("qtnote-media:/missing/image.png");
    fragment.blocks.append(image);

    const auto cloned = NoteFragmentMediaTransfer::cloneForDestination(fragment, destination);
    QVERIFY(!cloned);
    QCOMPARE(cloned.error, QStringLiteral("media attachment has no matching transfer reference"));
}

QTEST_GUILESS_MAIN(NoteFragmentMediaTransferTest)

#include "notefragmentmediatransfer_test.moc"
