#include "managerinteractionfixture.h"

#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QtTest>

#include "themediconimageprovider.h"

using namespace AnyKeep;

ManagerInteractionFixture::ManagerInteractionFixture()
{
    quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick.resize(620, 480);
    notesModel.setItemRoleNames({
        { Qt::UserRole + 1, "storageId" },
        { Qt::UserRole + 2, "noteId" },
        { Qt::UserRole + 3, "itemType" },
        { Qt::UserRole + 4, "title" },
        { Qt::UserRole + 5, "preview" },
        { Qt::UserRole + 6, "loading" },
        { Qt::UserRole + 7, "errorString" },
        { Qt::UserRole + 8, "hasMore" },
        { Qt::UserRole + 9, "noteCount" },
        { Qt::UserRole + 10, "iconSource" },
        { Qt::UserRole + 11, "tags" },
        { Qt::UserRole + 12, "modifiedTime" },
        { Qt::UserRole + 13, "storageName" },
        { Qt::UserRole + 14, "accessible" },
    });
    appendItem(QStringLiteral("storage-a"), {}, 0, QStringLiteral("Storage A"));
    appendItem(QStringLiteral("storage-a"), QStringLiteral("note-a"), 1, QStringLiteral("Note A"));
    appendItem(QStringLiteral("storage-a"), QStringLiteral("note-a2"), 1, QStringLiteral("Note A2"));
    appendItem(QStringLiteral("storage-b"), {}, 0, QStringLiteral("Storage B"));
    appendItem(QStringLiteral("storage-b"), QStringLiteral("note-b"), 1, QStringLiteral("Note B"));

    installThemedIconImageProvider(quick.engine());
    quick.rootContext()->setContextProperty(QStringLiteral("testNotesModel"), &notesModel);
    const QUrl    harnessUrl(QStringLiteral("qrc:/qml/ManagerInteractionHarness.qml"));
    QQmlComponent component(quick.engine(), harnessUrl);
    if (!component.isReady()) {
        errorString_ = component.errorString();
        return;
    }
    root = component.create();
    if (!root) {
        errorString_ = component.errorString();
        return;
    }
    quick.setContent(harnessUrl, &component, root);
    quick.show();
    QTest::qWait(50);
    page    = root->findChild<QQuickItem *>(QStringLiteral("managerPage"));
    tree    = root->findChild<QQuickItem *>(QStringLiteral("notesTree"));
    preview = root->findChild<QQuickItem *>(QStringLiteral("managerDragPreview"));
}

void ManagerInteractionFixture::appendItem(const QString &storageId, const QString &noteId, int itemType,
                                           const QString &title)
{
    auto *item = new QStandardItem;
    item->setData(storageId, Qt::UserRole + 1);
    item->setData(noteId, Qt::UserRole + 2);
    item->setData(itemType, Qt::UserRole + 3);
    item->setData(title, Qt::UserRole + 4);
    item->setData(QString(), Qt::UserRole + 5);
    item->setData(false, Qt::UserRole + 6);
    item->setData(QString(), Qt::UserRole + 7);
    item->setData(false, Qt::UserRole + 8);
    item->setData(itemType == 0 && storageId == QStringLiteral("storage-a") ? 1 : 0, Qt::UserRole + 9);
    item->setData(QString(), Qt::UserRole + 10);
    item->setData(QStringList(), Qt::UserRole + 11);
    item->setData(QDateTime(), Qt::UserRole + 12);
    item->setData(itemType == 0 ? title
                                : (storageId == QStringLiteral("storage-a") ? QStringLiteral("Storage A")
                                                                            : QStringLiteral("Storage B")),
                  Qt::UserRole + 13);
    item->setData(true, Qt::UserRole + 14);
    notesModel.appendRow(item);
}

QQuickItem *ManagerInteractionFixture::delegate(int row) const
{
    QVariant value;
    if (!page
        || !QMetaObject::invokeMethod(page, "groupedItemAtRow", Q_RETURN_ARG(QVariant, value), Q_ARG(QVariant, row))) {
        return nullptr;
    }
    return qobject_cast<QQuickItem *>(value.value<QObject *>());
}

QQuickItem *ManagerInteractionFixture::delegateForNote(const QString &noteId) const
{
    if (!tree)
        return nullptr;
    for (int row = 0; row < tree->property("rows").toInt(); ++row) {
        if (auto *item = delegate(row);
            item && item->property("noteId").toString() == noteId && item->property("itemType").toInt() == 1) {
            return item;
        }
    }
    return nullptr;
}

QPointF ManagerInteractionFixture::center(QQuickItem *item) const
{
    return item ? item->mapToItem(qobject_cast<QQuickItem *>(root), QPointF(item->width() / 2, item->height() / 2))
                : QPointF();
}

QPointF ManagerInteractionFixture::contentOrigin(QQuickItem *item) const
{
    auto *content = item ? qobject_cast<QQuickItem *>(item->property("contentItem").value<QObject *>()) : nullptr;
    return content ? content->mapToItem(qobject_cast<QQuickItem *>(root), QPointF()) : QPointF();
}

bool ManagerInteractionFixture::waitFor(const std::function<bool()> &condition, int timeout) const
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout)
        QTest::qWait(10);
    return condition();
}

bool ManagerInteractionFixture::drag(QQuickItem *source, QQuickItem *destination, int previewItems)
{
    if (!source || !destination)
        return false;
    if (!waitFor(
            [&]() {
                return preview->property("previewCount").toInt() == 0
                    && !page->property("dragSelectionSuppressed").toBool()
                    && qAbs(source->property("reorderOffset").toReal()) < 0.5
                    && qAbs(destination->property("reorderOffset").toReal()) < 0.5 && source->width() > 0
                    && source->height() > 0 && destination->width() > 0 && destination->height() > 0;
            },
            2500)) {
        return false;
    }
    const QPointF from = center(source);
    QPointF       to   = center(destination);
    to.ry() += from.y() < to.y() ? destination->height() / 2 - 2 : -destination->height() / 2 + 2;
    QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, from.toPoint());
    for (int step = 1; step <= 8; ++step)
        QTest::mouseMove(&quick, (from + (to - from) * (qreal(step) / 8)).toPoint(), 15);
    if (!waitFor([&]() { return preview->property("previewCount").toInt() == previewItems; }))
        return false;
    const auto animatedSource = [&]() {
        for (int row = 0; row < tree->property("rows").toInt(); ++row) {
            if (auto *item = delegate(row);
                item && item->property("partOfActiveDrag").toBool() && item->property("collapseSpace").toReal() > 0) {
                return true;
            }
        }
        return false;
    };
    if (!waitFor(animatedSource))
        return false;
    root->setProperty("lastDraggedNoteId", page->property("activeDraggedNoteId"));
    QObject *dropTarget = nullptr;
    if (!waitFor([&]() {
            dropTarget = page->property("dropTargetDelegate").value<QObject *>();
            return dropTarget && dropTarget->property("dragHovered").toBool()
                && (dropTarget->property("dropSpace").toReal() > 0
                    || dropTarget->property("dropAfterSpace").toReal() > 0);
        })) {
        return false;
    }
    const auto displacedNeighbor = [&]() {
        for (int row = 0; row < tree->property("rows").toInt(); ++row) {
            if (auto *item = delegate(row); item && !item->property("partOfActiveDrag").toBool()
                && qAbs(item->property("reorderOffset").toReal()) > 1) {
                return true;
            }
        }
        return false;
    };
    if (!waitFor(displacedNeighbor))
        return false;
    QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, to.toPoint());
    return waitFor([&]() {
        if (preview->property("previewCount").toInt() != 0)
            return false;
        for (int row = 0; row < tree->property("rows").toInt(); ++row)
            if (auto *item = delegate(row); item && qAbs(item->property("reorderOffset").toReal()) > 0.5)
                return false;
        return true;
    });
}

int ManagerInteractionFixture::modelRowForNote(const QString &noteId) const
{
    for (int row = 0; row < notesModel.rowCount(); ++row)
        if (notesModel.index(row, 0).data(Qt::UserRole + 2).toString() == noteId)
            return row;
    return -1;
}

bool ManagerInteractionFixture::applyRecordedMove()
{
    const QString movedId  = root->property("lastDraggedNoteId").toString();
    const QString anchorId = root->property("noteAnchor").toString();
    const int     movedRow = modelRowForNote(movedId);
    if (movedRow < 0)
        return false;
    auto      movedItems = notesModel.takeRow(movedRow);
    const int anchorRow  = modelRowForNote(anchorId);
    if (anchorRow < 0)
        return false;
    notesModel.insertRow(anchorRow + (root->property("noteInsertAfter").toBool() ? 1 : 0), movedItems);
    return true;
}
