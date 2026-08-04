#pragma once

#include <QQuickItem>
#include <QtTest>

namespace AnyKeep::TestSupport {

inline QQuickItem *quickItemByName(QQuickItem *root, const QString &name)
{
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem *child : root->childItems())
        if (auto *match = quickItemByName(child, name))
            return match;
    return nullptr;
}

inline QQuickItem *quickVisibleItemByName(QQuickItem *root, const QString &name)
{
    if (!root)
        return nullptr;
    if (root->objectName() == name && root->isVisible())
        return root;
    for (QQuickItem *child : root->childItems())
        if (auto *match = quickVisibleItemByName(child, name))
            return match;
    return nullptr;
}

inline QQuickItem *ancestorWithProperty(QQuickItem *item, const char *propertyName)
{
    for (auto *candidate = item; candidate; candidate = candidate->parentItem())
        if (candidate->property(propertyName).isValid())
            return candidate;
    return nullptr;
}

template <typename Window>
void moveMouseAlong(Window *window, const QPointF &from, const QPointF &to, int steps, int delayMs = 15,
                    int settleMs = 0)
{
    for (int step = 1; step <= steps; ++step) {
        QTest::mouseMove(window, (from + (to - from) * (qreal(step) / steps)).toPoint(), delayMs);
        if (settleMs > 0)
            QTest::qWait(settleMs);
    }
}

} // namespace AnyKeep::TestSupport
