#ifndef NOTETAGLINE_H
#define NOTETAGLINE_H

#include "qtnote_export.h"

#include <QString>
#include <QStringList>

#include <optional>

namespace QtNote {

struct QTNOTE_EXPORT NoteTagLineMatch {
    int         lineStart { -1 };
    int         lineEnd { -1 };
    int         contentStart { -1 };
    QString     rawLine;
    QStringList tags;
    bool        trailingWhitespace { false };
};

class QTNOTE_EXPORT NoteTagLine {
public:
    static bool        isValidTagName(const QString &name);
    static QStringList parseLine(const QString &line);
    static QString     serialize(const QStringList &tags);

    // A plain-text document stores the tag line physically immediately after
    // the title.  No empty separator line is skipped.
    static std::optional<NoteTagLineMatch> findPlainTextDocumentTagLine(const QString &text);

    // Plain projection of a Markdown QTextDocument.  Empty separator lines
    // are tolerated because Qt versions differ in how paragraph boundaries
    // are exposed through TextEdit::getText().
    static std::optional<NoteTagLineMatch> findEditorDocumentTagLine(const QString &text);

    // In Markdown the title and first body paragraph are separated by at least
    // one empty source line.  The first non-empty body line is the only tag-line
    // candidate.
    static std::optional<NoteTagLineMatch> findMarkdownDocumentTagLine(const QString &text);
    static std::optional<NoteTagLineMatch> firstMarkdownDocumentBodyLine(const QString &text);

    // QTextDocument/QML may expose paragraph and line separators instead of
    // '\n'.  Replacing them one-for-one keeps cursor offsets stable.
    static QString normalizeLineBreaks(QString text);
};

} // namespace QtNote

#endif // NOTETAGLINE_H
