#ifndef NOTETRANSFERCONTROLLER_H
#define NOTETRANSFERCONTROLLER_H

#include "anykeep_export.h"
#include "note.h"
#include "notefragment.h"

#include <QImage>
#include <QString>
#include <memory>

class QMimeData;

namespace AnyKeep {

// Format conversion boundary for clipboard and drag-and-drop.  It deliberately
// does not mutate a note or a media store; those operations require a target
// note context and are handled by the caller after import succeeds.
class ANYKEEP_EXPORT NoteTransferController {
public:
    static constexpr const char *FragmentMimeType       = "application/vnd.anykeep.fragment+cbor";
    static constexpr const char *MarkdownMimeType       = "text/markdown";
    static constexpr const char *TsvMimeType            = "text/tab-separated-values";
    static constexpr qint64      PortableImageDataLimit = 8 * 1024 * 1024;

    struct ExportResult {
        std::unique_ptr<QMimeData> mimeData;
        QString                    error;

        explicit operator bool() const { return mimeData != nullptr && error.isEmpty(); }
    };

    struct ImportResult {
        NoteFragment fragment;
        QImage       image;
        QString      sourceMimeType;
        QString      error;

        explicit operator bool() const { return error.isEmpty(); }
        bool     hasImage() const { return !image.isNull(); }
    };

    ExportResult createMimeData(const NoteFragment &fragment) const;
    ImportResult importMimeData(const QMimeData *mimeData) const;

    static QString markdownForFragment(const NoteFragment &fragment, QString *error = nullptr);
    static QString plainTextForFragment(const NoteFragment &fragment, QString *error = nullptr);
    static QString htmlForFragment(const NoteFragment &fragment, QString *error = nullptr);
    static QString tsvForFragment(const NoteFragment &fragment);
    static QString convertTextFormat(const QString &text, Note::Format sourceFormat, Note::Format targetFormat);
};

} // namespace AnyKeep

#endif // NOTETRANSFERCONTROLLER_H
