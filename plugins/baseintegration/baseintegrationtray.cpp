#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QFrame>
#include <QHash>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <limits>
#include <memory>
#include <utility>

#include "anykeep.h"
#include "baseintegrationtray.h"
#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "notetitleresolver.h"
#include "pluginhostinterface.h"
#include "trayiconutils.h"
#include "utils.h"

namespace AnyKeep {

namespace {
    constexpr int NoteTitleLimit = 48;

    struct TrayNote {
        QString     storageId;
        QString     noteId;
        QString     title;
        QStringList tags;
        QIcon       icon;
        bool        pendingDraft { false };
    };

    QString draftDisplayTitle(const DraftRecord &draft)
    {
        auto title = NoteTitleResolver::displayTitle(draft.title, draft.body, draft.format);
        return title.isEmpty() ? QObject::tr("Untitled Note") : title;
    }

    bool isUnpublishedDraft(const DraftRecord &draft)
    {
        return draft.remoteNoteId.isEmpty() && draft.removeSourceStorageId.isEmpty()
            && draft.removeSourceNoteId.isEmpty();
    }

    bool matchesFilter(const QString &title, const QStringList &tags, const QString &filter)
    {
        if (filter.isEmpty() || title.contains(filter, Qt::CaseInsensitive))
            return true;
        QString tagQuery = filter;
        if (tagQuery.startsWith(QLatin1Char('#')))
            tagQuery.remove(0, 1);
        if (tagQuery.isEmpty())
            return false;
        for (const auto &tag : tags) {
            if (tag.contains(tagQuery, Qt::CaseInsensitive))
                return true;
        }
        return false;
    }

    QList<TrayNote> activeNotes(PluginHostInterface *host, int limit, const QString &filter)
    {
        // Build the menu from the storage snapshot plus the persisted draft
        // overlay. New unpublished notes use a virtual Drafts storage id;
        // edits of existing notes keep their real storage/note identity.
        const auto                  draftRecords = DraftManager::instance()->pendingDrafts();
        QHash<QString, DraftRecord> pendingByNote;
        QList<DraftRecord>          localDrafts;
        QList<DraftRecord>          pendingTransfers;
        for (const auto &draft : draftRecords) {
            if (isUnpublishedDraft(draft)) {
                localDrafts.append(draft);
                continue;
            }
            if (draft.remoteNoteId.isEmpty()) {
                pendingTransfers.append(draft);
                continue;
            }
            const QString key = draft.storageId + QChar(0x1f) + draft.remoteNoteId;
            const auto    it  = pendingByNote.constFind(key);
            if (it == pendingByNote.cend() || draft.updatedAt > it->updatedAt)
                pendingByNote.insert(key, draft);
        }

        QList<TrayNote> notes;
        QSet<QString>   presented;
        const auto      storageNotes   = host->noteManager()->noteList();
        const auto     *catalogManager = FolderCatalogManager::instance();
        for (const auto &note : storageNotes) {
            if (catalogManager->isAvailable() && catalogManager->catalog().isRecycled(note.storageId(), note.id())) {
                continue;
            }
            const QString key     = note.storageId() + QChar(0x1f) + note.id();
            const auto    pending = pendingByNote.constFind(key);
            TrayNote      entry;
            entry.storageId = note.storageId();
            entry.noteId    = note.id();
            entry.icon      = note.storage() ? note.storage()->noteIcon() : QIcon();
            if (pending != pendingByNote.cend()) {
                entry.title        = draftDisplayTitle(*pending);
                entry.tags         = pending->tags;
                entry.pendingDraft = true;
            } else {
                entry.title = note.displayTitle();
                entry.tags  = note.tags();
            }
            presented.insert(key);
            if (matchesFilter(entry.title, entry.tags, filter))
                notes.append(std::move(entry));
        }

        for (auto it = pendingByNote.cbegin(); it != pendingByNote.cend(); ++it) {
            if (presented.contains(it.key()))
                continue;
            const auto storage = host->noteManager()->storage(it->storageId);
            TrayNote   entry { it->storageId,
                               it->remoteNoteId,
                               draftDisplayTitle(*it),
                               it->tags,
                               storage ? storage->noteIcon() : QIcon(),
                               true };
            if (matchesFilter(entry.title, entry.tags, filter))
                notes.append(std::move(entry));
        }
        for (const auto &draft : pendingTransfers) {
            const auto storage = host->noteManager()->storage(draft.storageId);
            TrayNote   entry { draft.storageId, draft.id.toString(QUuid::WithoutBraces), draftDisplayTitle(draft),
                               draft.tags,      storage ? storage->noteIcon() : QIcon(), true };
            if (matchesFilter(entry.title, entry.tags, filter))
                notes.append(std::move(entry));
        }
        for (const auto &draft : localDrafts) {
            TrayNote entry { DraftManager::draftsStorageId(),
                             draft.id.toString(QUuid::WithoutBraces),
                             draftDisplayTitle(draft),
                             draft.tags,
                             QIcon::fromTheme(QStringLiteral("document-edit"),
                                              QIcon(QStringLiteral(":/icons/manager"))),
                             true };
            if (matchesFilter(entry.title, entry.tags, filter))
                notes.append(std::move(entry));
        }

        std::stable_partition(notes.begin(), notes.end(), [](const TrayNote &note) { return note.pendingDraft; });
        if (notes.size() > limit)
            notes.resize(limit);
        return notes;
    }

    void fillNotesList(QListWidget *list, const QList<TrayNote> &notes, PluginHostInterface *host,
                       const QString &emptyText)
    {
        list->clear();
        if (notes.isEmpty()) {
            auto *item = new QListWidgetItem(emptyText, list);
            item->setFlags(Qt::NoItemFlags);
            return;
        }

        for (int i = 0; i < notes.count(); ++i) {
            const auto &note  = notes.at(i);
            auto        title = note.title.trimmed();
            if (title.isEmpty())
                title = QObject::tr("Untitled Note");
            auto *item = new QListWidgetItem(note.icon, host->utilsCuttedDots(title, NoteTitleLimit), list);
            if (note.pendingDraft)
                item->setForeground(list->palette().color(QPalette::PlaceholderText));
            item->setData(Qt::UserRole, i);
        }
        list->setCurrentRow(0);
    }
}

BaseIntegrationTray::BaseIntegrationTray(Main *anykeep, PluginHostInterface *host, QObject *parent) :
    TrayImpl(parent), anykeep(anykeep), host(host)
{
    actQuit    = new QAction(QIcon(":/icons/exit"), tr("&Quit"), this);
    actNew     = new QAction(QIcon(":/icons/new"), tr("&New"), this);
    actAbout   = new QAction(QIcon(":/icons/trayicon"), tr("&About"), this);
    actOptions = new QAction(QIcon(":/icons/options"), tr("&Options"), this);
    actManager = new QAction(QIcon(":/icons/manager"), tr("&Note Manager"), this);

    contextMenu = new QMenu;
    contextMenu->addAction(actNew);
    contextMenu->addSeparator();
    contextMenu->addAction(actManager);
    contextMenu->addAction(actOptions);
    contextMenu->addAction(actAbout);
    contextMenu->addSeparator();
    contextMenu->addAction(actQuit);

    tray = new QSystemTrayIcon(this);
    TrayIconUtils::setupSystemTrayIcon(tray);
    tray->show();
    tray->setContextMenu(contextMenu);

    connect(tray, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            SLOT(showNoteList(QSystemTrayIcon::ActivationReason)));
    connect(tray, &QSystemTrayIcon::messageClicked, this, [this]() {
        auto action        = std::move(notificationAction);
        notificationAction = {};
        if (action)
            action();
    });

    connect(actQuit, SIGNAL(triggered()), SIGNAL(exitTriggered()));
    connect(actNew, SIGNAL(triggered()), SIGNAL(newNoteTriggered()));
    connect(actManager, SIGNAL(triggered()), SIGNAL(noteManagerTriggered()));
    connect(actOptions, SIGNAL(triggered()), SIGNAL(optionsTriggered()));
    connect(actAbout, SIGNAL(triggered()), SIGNAL(aboutTriggered()));
}

BaseIntegrationTray::~BaseIntegrationTray()
{
    // ensure proper order of delition and don't forget to delete contextMenu
    delete tray;
    delete contextMenu;
}

void BaseIntegrationTray::showNotification(const QString &title, const QString &message, const QString &actionText,
                                           std::function<void()> action, bool error)
{
    Q_UNUSED(actionText)
    notificationAction = std::move(action);
    tray->showMessage(title, message, error ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information, 10000);
}

void BaseIntegrationTray::showNoteList(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::MiddleClick || reason == QSystemTrayIcon::DoubleClick) {
        emit newNoteTriggered();
        return;
    }
    if (reason != QSystemTrayIcon::Trigger) {
        return;
    }
    if (currentPopup) {
        currentPopup->close();
        return;
    }

    QSettings s;
    const int maxNotes = s.value("ui.menu-notes-amount", 15).toInt();

    auto *popup = new QFrame(nullptr, Qt::Tool | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setFrameShape(QFrame::StyledPanel);
    popup->setWindowTitle(tr("Notes"));

    auto *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto *filter = new QLineEdit(popup);
    filter->setPlaceholderText(tr("Search notes"));
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);

    auto *list = new QListWidget(popup);
    list->setFrameShape(QFrame::NoFrame);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setUniformItemSizes(true);
    list->setMinimumWidth(280);
    list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    layout->addWidget(list);

    auto *newButton = new QPushButton(QIcon(":/icons/new"), tr("&New"), popup);
    layout->addWidget(newButton);

    auto notes       = std::make_shared<QList<TrayNote>>();
    auto reloadNotes = [=, this]() {
        *notes = activeNotes(host, maxNotes, filter->text());
        fillNotesList(list, *notes, host,
                      filter->text().trimmed().isEmpty() ? tr("No notes yet") : tr("No notes match the search"));
    };
    reloadNotes();

    // The setting controls the number of rows visible in the tray popup, not
    // merely the number of rows fetched. Keep the popup within the available
    // screen area; QListWidget then exposes the remaining configured rows via
    // its normal scrollbar.
    const int   rowHeight   = qMax(list->sizeHintForRow(0), list->fontMetrics().height() + 10);
    const int   visibleRows = qMax(1, qMin(maxNotes, list->count()));
    const auto *screen      = QGuiApplication::screenAt(QCursor::pos());
    const QRect availableGeometry
        = screen ? screen->availableGeometry() : QGuiApplication::primaryScreen()->availableGeometry();
    const QMargins margins     = layout->contentsMargins();
    const int      popupChrome = margins.top() + margins.bottom() + filter->sizeHint().height()
        + newButton->sizeHint().height() + layout->spacing() * 2 + popup->frameWidth() * 2;
    const int maximumListHeight = qMax(rowHeight, availableGeometry.height() - popupChrome - 16);
    const int desiredListHeight = visibleRows * rowHeight + list->frameWidth() * 2;
    list->setFixedHeight(qMin(desiredListHeight, maximumListHeight));

    auto openItem = [=, this](QListWidgetItem *item) {
        if (!item) {
            return;
        }
        const int noteIndex = item->data(Qt::UserRole).toInt();
        if (noteIndex < 0 || noteIndex >= notes->size()) {
            return;
        }
        const auto note = notes->at(noteIndex);
        popup->close();
        emit showNoteTriggered(note.storageId, note.noteId);
    };

    connect(filter, &QLineEdit::textChanged, popup, reloadNotes);
    connect(filter, &QLineEdit::returnPressed, popup, [=]() { openItem(list->currentItem()); });
    connect(list, &QListWidget::itemClicked, popup, openItem);
    connect(list, &QListWidget::itemActivated, popup, openItem);
    connect(newButton, &QPushButton::clicked, popup, [this, popup]() {
        popup->close();
        emit newNoteTriggered();
    });
    connect(qApp, &QApplication::focusChanged, popup, [popup](QWidget *, QWidget *now) {
        if (now && (now == popup || popup->isAncestorOf(now))) {
            return;
        }
        popup->close();
    });

    currentPopup = popup;
    connect(popup, &QObject::destroyed, this, [this]() { currentPopup.clear(); });
    QRect dr = QGuiApplication::screenAt(QCursor::pos())->geometry();
    QRect ir = tray->geometry();
    if (ir.isEmpty()) { // O_O but with kde this happens...
        ir = QRect(QCursor::pos() - QPoint(8, 8), QSize(16, 16));
    }
    QRect mr;
    mr.setSize(popup->sizeHint());
    if (ir.left() < dr.width() / 2) {
        if (ir.top() < dr.height() / 2) {
            mr.moveTopLeft(ir.bottomLeft());
        } else {
            mr.moveBottomLeft(ir.topLeft());
        }
    } else {
        if (ir.top() < dr.height() / 2) {
            mr.moveTopRight(ir.bottomRight());
        } else {
            mr.moveBottomRight(ir.topRight());
        }
    }
    // and now align to available desktop geometry
    if (mr.right() > dr.right()) {
        mr.moveRight(dr.right());
    }
    if (mr.bottom() > dr.bottom()) {
        mr.moveBottom(dr.bottom());
    }
    if (mr.left() < dr.left()) {
        mr.moveLeft(dr.left());
    }
    if (mr.top() < dr.top()) {
        mr.moveTop(dr.top());
    }
    popup->setGeometry(mr);
    popup->show();
    anykeep->activateWidget(popup);
    QTimer::singleShot(0, filter, [filter]() { filter->setFocus(Qt::PopupFocusReason); });
}

} // namespace AnyKeep
