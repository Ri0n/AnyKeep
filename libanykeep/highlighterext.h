#ifndef HIGHLIGHTEREXT_H
#define HIGHLIGHTEREXT_H

#include <QObject>
#include <QSharedPointer>
#include <QStringList>
#include <QTextFormat>
#include <memory>

#include "anykeep_export.h"

class QString;

namespace AnyKeep {

inline constexpr int SpellCheckFormatProperty = QTextFormat::UserProperty + 0x100;

class NoteHighlighter;

class ANYKEEP_EXPORT HighlighterExtension : public std::enable_shared_from_this<HighlighterExtension> {
public:
    virtual void highlight(NoteHighlighter *, const QString &text) = 0;
    virtual void reset() = 0; // resets any state to initial. e.g. because of replacing note contents
};

class ANYKEEP_EXPORT SpellCheckExtension : public HighlighterExtension {
public:
    virtual QStringList suggestions(const QString &word) const = 0;
    virtual void        addToDictionary(const QString &word)   = 0;
};

} // namespace AnyKeep

#endif // HIGHLIGHTEREXT_H
