#ifndef NOTEPRESENTATIONORDER_H
#define NOTEPRESENTATIONORDER_H

#include <QDateTime>
#include <QString>

namespace AnyKeep {

inline bool notePresentationComesBefore(bool leftPendingDraft, bool leftFavorite, const QDateTime &leftModified,
                                        const QString &leftTitle, bool rightPendingDraft, bool rightFavorite,
                                        const QDateTime &rightModified, const QString &rightTitle)
{
    if (leftPendingDraft != rightPendingDraft)
        return leftPendingDraft;
    if (leftFavorite != rightFavorite)
        return leftFavorite;
    if (leftModified != rightModified)
        return leftModified > rightModified;
    return leftTitle.localeAwareCompare(rightTitle) < 0;
}

} // namespace AnyKeep

#endif // NOTEPRESENTATIONORDER_H
