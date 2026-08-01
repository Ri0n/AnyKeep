#include "windowgeometryutils.h"

#include <QGuiApplication>
#include <QScreen>

#include <limits>

namespace QtNote::WindowGeometryUtils {
namespace {

    qint64 area(const QRect &rect) { return rect.isValid() ? qint64(rect.width()) * qint64(rect.height()) : 0; }

    qint64 squaredDistanceToRect(const QPoint &point, const QRect &rect)
    {
        const qint64 dx = point.x() < rect.left() ? qint64(rect.left()) - point.x()
            : point.x() > rect.right()            ? qint64(point.x()) - rect.right()
                                                  : 0;
        const qint64 dy = point.y() < rect.top() ? qint64(rect.top()) - point.y()
            : point.y() > rect.bottom()          ? qint64(point.y()) - rect.bottom()
                                                 : 0;
        return dx * dx + dy * dy;
    }

    const QRect *bestScreen(const QRect &requested, const QList<QRect> &screens)
    {
        const QRect *best             = nullptr;
        qint64       bestIntersection = 0;
        for (const auto &screen : screens) {
            const qint64 intersection = area(requested.intersected(screen));
            if (intersection > bestIntersection) {
                bestIntersection = intersection;
                best             = &screen;
            }
        }
        if (best)
            return best;

        qint64 nearestDistance = std::numeric_limits<qint64>::max();
        for (const auto &screen : screens) {
            const qint64 distance = squaredDistanceToRect(requested.center(), screen);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                best            = &screen;
            }
        }
        return best;
    }

} // namespace

QRect constrainToAvailableScreens(const QRect &requested, const QList<QRect> &availableScreens,
                                  const QSize &minimumSize)
{
    if (!requested.isValid() || availableScreens.isEmpty())
        return requested;

    const QRect *screen = bestScreen(requested, availableScreens);
    if (!screen || !screen->isValid())
        return requested;

    QSize size = requested.size().expandedTo(minimumSize);
    size.setWidth(qMin(size.width(), screen->width()));
    size.setHeight(qMin(size.height(), screen->height()));

    const int maxX = screen->right() - size.width() + 1;
    const int maxY = screen->bottom() - size.height() + 1;
    const int x    = qBound(screen->left(), requested.x(), maxX);
    const int y    = qBound(screen->top(), requested.y(), maxY);
    return QRect(QPoint(x, y), size);
}

QRect constrainToCurrentScreens(const QRect &requested, const QSize &minimumSize)
{
    QList<QRect> screens;
    const auto   currentScreens = QGuiApplication::screens();
    screens.reserve(currentScreens.size());
    for (const auto *screen : currentScreens) {
        if (screen)
            screens.append(screen->availableGeometry());
    }
    return constrainToAvailableScreens(requested, screens, minimumSize);
}

} // namespace QtNote::WindowGeometryUtils
