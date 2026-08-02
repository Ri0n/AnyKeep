#pragma once

#include <QQuickWidget>
#include <QStandardItemModel>

#include <functional>

class QQuickItem;

class ManagerInteractionFixture {
public:
    ManagerInteractionFixture();

    bool    isReady() const { return root && page && tree && preview; }
    QString errorString() const { return errorString_; }

    void        appendItem(const QString &storageId, const QString &noteId, int itemType, const QString &title);
    QQuickItem *delegate(int row) const;
    QQuickItem *delegateForNote(const QString &noteId) const;
    QPointF     center(QQuickItem *item) const;
    QPointF     contentOrigin(QQuickItem *item) const;
    bool        waitFor(const std::function<bool()> &condition, int timeout = 1500) const;
    bool        drag(QQuickItem *source, QQuickItem *destination, int previewItems);
    int         modelRowForNote(const QString &noteId) const;
    bool        applyRecordedMove();

    QStandardItemModel notesModel;
    QQuickWidget       quick;
    QObject           *root { nullptr };
    QQuickItem        *page { nullptr };
    QQuickItem        *tree { nullptr };
    QQuickItem        *preview { nullptr };

private:
    QString errorString_;
};
