#ifndef ANYKEEP_EDITOR_PRIVATE_H
#define ANYKEEP_EDITOR_PRIVATE_H

#include "noteblockmodel.h"

#include <QList>
#include <QString>
#include <QVariantList>

class QTextDocument;

namespace AnyKeep::EditorOperationsPrivate {

int                            documentEnd(const QTextDocument *document);
QString                        markdownRange(QTextDocument *document, int start, int end);
QList<NoteBlockSelectionRange> decodeSelectionRanges(const QVariantList &encodedRanges);

} // namespace AnyKeep::EditorOperationsPrivate

#endif // ANYKEEP_EDITOR_PRIVATE_H
