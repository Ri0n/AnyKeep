#ifndef WINDOWGEOMETRYUTILS_H
#define WINDOWGEOMETRYUTILS_H

#include "qtnote_export.h"

#include <QList>
#include <QRect>
#include <QSize>

namespace QtNote::WindowGeometryUtils {

QTNOTE_EXPORT QRect constrainToAvailableScreens(const QRect &requested, const QList<QRect> &availableScreens,
                                                const QSize &minimumSize = QSize(1, 1));
QTNOTE_EXPORT QRect constrainToCurrentScreens(const QRect &requested, const QSize &minimumSize = QSize(1, 1));

} // namespace QtNote::WindowGeometryUtils

#endif // WINDOWGEOMETRYUTILS_H
