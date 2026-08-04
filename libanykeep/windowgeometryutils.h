#ifndef WINDOWGEOMETRYUTILS_H
#define WINDOWGEOMETRYUTILS_H

#include "anykeep_export.h"

#include <QList>
#include <QRect>
#include <QSize>

namespace AnyKeep::WindowGeometryUtils {

ANYKEEP_EXPORT QRect constrainToAvailableScreens(const QRect &requested, const QList<QRect> &availableScreens,
                                                 const QSize &minimumSize = QSize(1, 1));
ANYKEEP_EXPORT QRect constrainToCurrentScreens(const QRect &requested, const QSize &minimumSize = QSize(1, 1));

} // namespace AnyKeep::WindowGeometryUtils

#endif // WINDOWGEOMETRYUTILS_H
