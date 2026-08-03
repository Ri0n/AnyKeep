#include "notefragmentmediatransfer.h"

#include "localmediastore.h"

#include <QHash>

#include <utility>

namespace AnyKeep {

NoteFragmentMediaTransfer::Result NoteFragmentMediaTransfer::cloneForDestination(const NoteFragment    &fragment,
                                                                                 LocalMediaStore       &destination,
                                                                                 const LocalMediaStore *sourceStore)
{
    Result result;
    result.fragment = fragment;

    QHash<QString, NoteFragmentMedia> sourceMedia;
    for (const NoteFragmentMedia &media : fragment.media) {
        if (media.sourceUri.isEmpty() || !media.reference.isValid()) {
            result.error = QStringLiteral("fragment contains an invalid media reference");
            return result;
        }
        if (sourceMedia.contains(media.sourceUri)) {
            result.error = QStringLiteral("fragment contains duplicate media URIs");
            return result;
        }
        sourceMedia.insert(media.sourceUri, media);
    }

    const auto blockSourceUri = [](const NoteFragmentBlock &block) {
        if (block.type == NoteFragmentBlockType::Image)
            return block.image.sourceUri;
        if (block.type == NoteFragmentBlockType::Audio)
            return block.audio.sourceUri;
        if (block.type == NoteFragmentBlockType::Attachment)
            return block.attachment.sourceUri;
        return QString();
    };

    QHash<QString, QString> rewrittenUris;
    for (const NoteFragmentBlock &block : fragment.blocks) {
        const QString sourceUri = blockSourceUri(block);
        if (sourceUri.isEmpty() || rewrittenUris.contains(sourceUri))
            continue;
        const auto source = sourceMedia.constFind(sourceUri);
        if (source == sourceMedia.cend()) {
            result.error = QStringLiteral("media attachment has no matching transfer reference");
            return result;
        }

        QByteArray data = source->data;
        if (data.isEmpty()) {
            if (!sourceStore) {
                result.error = QStringLiteral("media bytes are unavailable");
                return result;
            }
            const auto loaded = sourceStore->data(source->reference.blobId);
            if (!loaded) {
                result.error = QStringLiteral("could not read source media: %1").arg(loaded.error);
                return result;
            }
            data = loaded.value;
        }

        const auto imported = destination.importData(data, source->reference.originalName, source->reference.mediaType,
                                                     QUuid::createUuid());
        if (!imported) {
            result.error = QStringLiteral("could not import media: %1").arg(imported.error);
            return result;
        }
        MediaReference reference = imported.value;
        reference.remoteData.clear();
        rewrittenUris.insert(sourceUri, reference.uri());
        result.importedMedia.append(reference);
    }

    for (NoteFragmentBlock &block : result.fragment.blocks) {
        if (block.type == NoteFragmentBlockType::Image)
            block.image.sourceUri = rewrittenUris.value(block.image.sourceUri, block.image.sourceUri);
        else if (block.type == NoteFragmentBlockType::Audio)
            block.audio.sourceUri = rewrittenUris.value(block.audio.sourceUri, block.audio.sourceUri);
        else if (block.type == NoteFragmentBlockType::Attachment)
            block.attachment.sourceUri = rewrittenUris.value(block.attachment.sourceUri, block.attachment.sourceUri);
    }
    result.fragment.media.clear();
    for (const MediaReference &reference : std::as_const(result.importedMedia)) {
        NoteFragmentMedia media;
        media.sourceUri = reference.uri();
        media.reference = reference;
        result.fragment.media.append(media);
    }
    return result;
}

} // namespace AnyKeep
