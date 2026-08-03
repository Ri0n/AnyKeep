#include <QSet>

#include "notehighlighter.h"

namespace AnyKeep {

NoteHighlighter::NoteHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) { }

void NoteHighlighter::highlightBlock(const QString &text)
{
    formats.clear();
    QMutableListIterator<ExtItem> it(extensions);
    while (it.hasNext()) {
        ExtItem item = it.next();
        if (!item.active) {
            continue;
        }
        auto ext = item.ext.lock();
        if (ext) {
            ext->highlight(this, text);
        } else {
            it.remove();
        }
    }

    QSet<int> bps;

    for (int i = 0; i < formats.count(); i++) {
        bps.insert(formats[i].start);
        bps.insert(formats[i].start + formats[i].count);
    }

    auto bpl = QList<int>(bps.constBegin(), bps.constEnd());
    std::sort(bpl.begin(), bpl.end());
    for (int i = 0; i < bpl.count() - 1; i++) {
        int             count = bpl[i + 1] - bpl[i];
        QTextCharFormat format;
        bool            merged = false;
        foreach (auto &f, formats) {
            if (f.start <= bpl[i] && (f.start + f.count) >= bpl[i + 1]) {
                format.merge(f.format);
                merged = true;
            }
        }
        if (merged) {
            setFormat(bpl[i], count, format);
        }
    }
}

void NoteHighlighter::addExtension(std::shared_ptr<HighlighterExtension> extension, NoteHighlighter::ExtType type,
                                   Priority prio)
{
    int pos = 0;
    while (pos < extensions.count() && prio > extensions[pos].priority) {
        pos++;
    }
    extensions.insert(pos, ExtItem { true, type, prio, extension });
}

void NoteHighlighter::disableExtension(NoteHighlighter::ExtType type)
{
    for (int i = 0; i < extensions.count(); i++) {
        if (extensions[i].type == type) {
            extensions[i].active = false;
        }
    }
}

void NoteHighlighter::enableExtension(NoteHighlighter::ExtType type)
{
    for (int i = 0; i < extensions.count(); i++) {
        if (extensions[i].type == type) {
            extensions[i].active = true;
        }
    }
}

void NoteHighlighter::addFormat(int start, int count, const QTextCharFormat &format)
{
    formats.append({ start, count, format });
}

} // namespace AnyKeep
