#ifndef NOTEFRAGMENT_H
#define NOTEFRAGMENT_H

#include "mediareference.h"
#include "anykeep_export.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace AnyKeep {

// A versioned, editor-independent representation of selected note content.
// It is deliberately not tied to QML items or QTextDocument so it can be used
// for clipboard, drag-and-drop, and (later) cross-process note transfer.
enum class NoteFragmentKind {
    Inline,
    BlockSequence,
    TableCells,
};

enum class NoteFragmentSourceFormat {
    PlainText,
    Markdown,
};

enum class NoteFragmentBlockType {
    Text,
    Heading,
    List,
    Table,
    Image,
    BlockQuote,
    CodeBlock,
    TagLine,
    Audio,
    Attachment,
};

enum class NoteFragmentListKind {
    Bullet,
    Check,
    Numbered,
};

struct ANYKEEP_EXPORT NoteFragmentListItem {
    QString              markdown;
    int                  indent { 0 };
    NoteFragmentListKind kind { NoteFragmentListKind::Bullet };
    bool                 checked { false };
};

struct ANYKEEP_EXPORT NoteFragmentTable {
    int         rows { 0 };
    int         columns { 0 };
    int         headerRows { 0 };
    QStringList markdownCells;
};

struct ANYKEEP_EXPORT NoteFragmentImage {
    QString sourceUri;
    QString alt;
    int     width { 0 };
    QString alignment { QStringLiteral("center") };
};

struct ANYKEEP_EXPORT NoteFragmentAudio {
    QString sourceUri;
    QString title;
    qint64  durationMs { 0 };
    QString transcript;
};

struct ANYKEEP_EXPORT NoteFragmentAttachment {
    QString sourceUri;
    QString fileName;
    QString mediaType;
    qint64  size { 0 };
};

struct ANYKEEP_EXPORT NoteFragmentBlock {
    NoteFragmentBlockType       type { NoteFragmentBlockType::Text };
    QString                     markdown;
    int                         headingLevel { 0 };
    QList<NoteFragmentListItem> listItems;
    NoteFragmentTable           table;
    NoteFragmentImage           image;
    NoteFragmentAudio           audio;
    NoteFragmentAttachment      attachment;
    QString                     language;
    QStringList                 tags;
};

// "data" is optional. It is only populated when a clipboard or drag payload
// must remain usable without access to the source LocalMediaStore.
struct ANYKEEP_EXPORT NoteFragmentMedia {
    QString        sourceUri;
    MediaReference reference;
    QByteArray     data;
};

struct ANYKEEP_EXPORT NoteFragment {
    static constexpr quint32 CurrentVersion = 6;

    quint32                  version { CurrentVersion };
    NoteFragmentKind         kind { NoteFragmentKind::BlockSequence };
    NoteFragmentSourceFormat sourceFormat { NoteFragmentSourceFormat::Markdown };
    QList<NoteFragmentBlock> blocks;
    QList<NoteFragmentMedia> media;
};

struct ANYKEEP_EXPORT NoteFragmentDecodeResult {
    NoteFragment fragment;
    QString      error;

    explicit operator bool() const { return error.isEmpty(); }
};

// The codec is the contract of application/vnd.anykeep.fragment+cbor.
// Decoder validation is intentionally strict because paste/drop data is an
// external input even when the MIME type belongs to AnyKeep.
ANYKEEP_EXPORT QByteArray               encodeNoteFragment(const NoteFragment &fragment);
ANYKEEP_EXPORT NoteFragmentDecodeResult decodeNoteFragment(const QByteArray &data);

} // namespace AnyKeep

#endif // NOTEFRAGMENT_H
