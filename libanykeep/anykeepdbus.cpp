/*
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "anykeepdbus.h"

#include <QByteArray>
#include <QDBusConnection>
#include <QDBusError>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QtGlobal>

#include <algorithm>

#include "anykeep.h"
#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "notemanager.h"
#include "notepresentationorder.h"
#include "notetitleresolver.h"
#include "shortcutsmanager.h"
#include "stickynotesmanager.h"
#include "windowgeometryutils.h"

namespace AnyKeep {

namespace {
    constexpr auto ServiceName          = "com.github.ri0n.AnyKeep";
    constexpr auto ObjectPath           = "/AnyKeep";
    constexpr int  DefaultNotesPageSize = 50;
    constexpr int  MaxNotesPageSize     = 200;

    QString acceleratorFromKeySequence(const QKeySequence &key)
    {
        if (key.isEmpty())
            return {};

        const auto parts  = key.toString(QKeySequence::PortableText).split(QLatin1Char(','));
        const auto tokens = parts.constFirst().split(QLatin1Char('+'), Qt::SkipEmptyParts);
        if (tokens.isEmpty())
            return {};

        QString accelerator;
        for (int i = 0; i < tokens.size() - 1; ++i) {
            const auto token = tokens.at(i);
            if (token == QLatin1String("Ctrl"))
                accelerator += QLatin1String("<Ctrl>");
            else if (token == QLatin1String("Alt"))
                accelerator += QLatin1String("<Alt>");
            else if (token == QLatin1String("Shift"))
                accelerator += QLatin1String("<Shift>");
            else if (token == QLatin1String("Meta"))
                accelerator += QLatin1String("<Super>");
            else
                accelerator += QLatin1Char('<') + token + QLatin1Char('>');
        }
        accelerator += tokens.constLast();
        return accelerator;
    }

    bool matchesMenuQuery(const QString &title, const QStringList &tags, const QString &query)
    {
        if (query.isEmpty() || title.contains(query, Qt::CaseInsensitive))
            return true;
        QString tagQuery = query;
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

    const FolderRecord *menuFolder(const Note &note, const FolderCatalog &catalog)
    {
        const auto *assignment = catalog.assignment(note.storageId(), note.id());
        if (assignment)
            return assignment->tombstone ? nullptr : catalog.folder(assignment->folderId);
        return catalog.folder(note.folderId());
    }
}

AnyKeepDBus::AnyKeepDBus(Main *anykeep, QObject *parent) : QObject(parent), m_anykeep(anykeep)
{
    auto *manager = NoteManager::instance();
    connect(manager, &NoteManager::storageAdded, this, &AnyKeepDBus::notesChanged);
    connect(manager, &NoteManager::storageRemoved, this, &AnyKeepDBus::notesChanged);
    connect(manager, &NoteManager::storageChanged, this, &AnyKeepDBus::notesChanged);
    // A publication is the point at which a checkpointed edit becomes visible
    // to storage-backed consumers. Refresh the tray/plasmoid after the storage
    // cache has accepted the returned note, even if a plugin omits or delays its
    // noteModified signal.
    connect(DraftManager::instance(), &DraftManager::draftPublished, this, &AnyKeepDBus::notesChanged);
    connect(DraftManager::instance(), &DraftManager::draftsChanged, this, &AnyKeepDBus::notesChanged);
    // Folder flags determine which entries are exposed by menu consumers.
    // They live in the catalog rather than NoteManager, so notify D-Bus
    // clients explicitly when a folder is favorited, archived, moved, etc.
    connect(FolderCatalogManager::instance(), &FolderCatalogManager::catalogChanged, this, &AnyKeepDBus::notesChanged);
    connect(anykeep, &Main::settingsUpdated, this, &AnyKeepDBus::notesChanged);
    connect(anykeep, &Main::settingsUpdated, this, &AnyKeepDBus::globalShortcutsChanged);
    connect(anykeep->stickyNotesManager(), &StickyNotesManager::notesChanged, this, &AnyKeepDBus::stickyNotesChanged);

    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QLatin1String(ObjectPath), this,
                            QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals)) {
        qWarning("Failed to register the AnyKeep D-Bus object: %s", qPrintable(bus.lastError().message()));
        return;
    }

    m_registeredService = bus.registerService(QLatin1String(ServiceName));
    if (!m_registeredService) {
        qWarning("Failed to register the AnyKeep D-Bus service: %s", qPrintable(bus.lastError().message()));
        bus.unregisterObject(QLatin1String(ObjectPath));
    }
}

AnyKeepDBus::~AnyKeepDBus()
{
    if (!m_registeredService)
        return;

    auto bus = QDBusConnection::sessionBus();
    bus.unregisterObject(QLatin1String(ObjectPath));
    bus.unregisterService(QLatin1String(ServiceName));
}

QString AnyKeepDBus::notesJson(int offset, int limit, const QString &query) const
{
    offset = qMax(0, offset);
    limit  = limit > 0 ? qMin(limit, MaxNotesPageSize) : DefaultNotesPageSize;

    struct MenuNote {
        QString     storageId;
        QString     noteId;
        QString     title;
        QStringList tags;
        QDateTime   modified;
        bool        favorite { false };
        bool        pendingDraft { false };
    };

    const QString        filter         = query.trimmed();
    const auto          *catalogManager = FolderCatalogManager::instance();
    const FolderCatalog *catalog        = catalogManager->isAvailable() ? &catalogManager->catalog() : nullptr;
    const auto           draftRecords   = DraftManager::instance()->pendingDrafts();

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

    QList<MenuNote> notes;
    QSet<QString>   presented;
    const auto      allNotes = NoteManager::instance()->noteList(-1);
    notes.reserve(allNotes.size() + draftRecords.size());
    for (const auto &note : allNotes) {
        const auto *folder = catalog ? menuFolder(note, *catalog) : nullptr;
        // The menu is a quick route to active notes. Archived folders are
        // deliberately hidden; Recycle Bin entries are only reachable from
        // the manager, where restore and permanent-delete actions exist.
        if (folder && catalog->isInArchivedBranch(folder->id))
            continue;

        const QString key = note.storageId() + QChar(0x1f) + note.id();
        const auto    it  = pendingByNote.constFind(key);
        MenuNote      entry;
        entry.storageId = note.storageId();
        entry.noteId    = note.id();
        entry.favorite  = note.isFavorite() || (folder && catalog->isEffectivelyFavorite(folder->id));
        if (it != pendingByNote.cend()) {
            entry.title            = draftDisplayTitle(*it);
            entry.tags             = it->tags;
            entry.modified         = it->updatedAt;
            const auto favoriteKey = QString::fromLatin1(FavoriteBackendKey);
            if (it->backendData.contains(favoriteKey))
                entry.favorite = it->backendData.value(favoriteKey).toBool()
                    || (folder && catalog->isEffectivelyFavorite(folder->id));
            entry.pendingDraft = true;
        } else {
            entry.title    = note.displayTitle();
            entry.tags     = note.tags();
            entry.modified = note.lastChangeUTC();
        }
        presented.insert(key);
        if (matchesMenuQuery(entry.title, entry.tags, filter))
            notes.append(std::move(entry));
    }

    for (auto it = pendingByNote.cbegin(); it != pendingByNote.cend(); ++it) {
        if (presented.contains(it.key()))
            continue;
        const auto &draft = it.value();
        const auto  title = draftDisplayTitle(draft);
        if (matchesMenuQuery(title, draft.tags, filter))
            notes.append({ draft.storageId, draft.remoteNoteId, title, draft.tags, draft.updatedAt,
                           draft.backendData.value(QString::fromLatin1(FavoriteBackendKey)).toBool(), true });
    }
    for (const auto &draft : pendingTransfers) {
        const auto title = draftDisplayTitle(draft);
        if (matchesMenuQuery(title, draft.tags, filter)) {
            notes.append({ draft.storageId, draft.id.toString(QUuid::WithoutBraces), title, draft.tags, draft.updatedAt,
                           draft.backendData.value(QString::fromLatin1(FavoriteBackendKey)).toBool(), true });
        }
    }
    for (const auto &draft : localDrafts) {
        const auto title = draftDisplayTitle(draft);
        if (matchesMenuQuery(title, draft.tags, filter)) {
            notes.append({ DraftManager::draftsStorageId(), draft.id.toString(QUuid::WithoutBraces), title, draft.tags,
                           draft.updatedAt, draft.backendData.value(QString::fromLatin1(FavoriteBackendKey)).toBool(),
                           true });
        }
    }

    std::stable_sort(notes.begin(), notes.end(), [](const MenuNote &left, const MenuNote &right) {
        return notePresentationComesBefore(left.pendingDraft, left.favorite, left.modified, left.title,
                                           right.pendingDraft, right.favorite, right.modified, right.title);
    });

    const qsizetype first = qMin<qsizetype>(offset, notes.size());
    const qsizetype last  = qMin<qsizetype>(first + limit, notes.size());

    QJsonArray result;
    for (qsizetype i = first; i < last; ++i) {
        const auto &note = notes.at(i);
        result.append(QJsonObject {
            { "id", note.noteId },
            { "storageId", note.storageId },
            { "title", note.title },
            { "tags", QJsonArray::fromStringList(note.tags) },
            { "modified", note.modified.toUTC().toString(Qt::ISODateWithMs) },
            { "favorite", note.favorite },
            { "pendingDraft", note.pendingDraft },
        });
    }
    return QString::fromUtf8(QJsonDocument(QJsonObject {
                                               { "notes", result },
                                               { "hasMore", last < notes.size() },
                                           })
                                 .toJson(QJsonDocument::Compact));
}

QString AnyKeepDBus::globalShortcutsJson() const
{
    QJsonArray result;
    const auto ids = m_anykeep->shortcutsManager()->globalShortcutIds();
    for (const auto &id : ids) {
        result.append(QJsonObject {
            { "id", id },
            { "accelerator", acceleratorFromKeySequence(m_anykeep->shortcutsManager()->key(id)) },
        });
    }
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

QString AnyKeepDBus::stickyNotesJson() const { return m_anykeep->stickyNotesManager()->notesJson(); }

QString AnyKeepDBus::stickyNoteJson(const QString &stickyId) const
{
    return m_anykeep->stickyNotesManager()->noteJson(QUuid(stickyId));
}

QString AnyKeepDBus::stickyNoteForPresentationJson(const QString &presentationId) const
{
    return m_anykeep->stickyNotesManager()->noteForPresentationJson(presentationId);
}

QString AnyKeepDBus::claimWindowGeometry()
{
    const QString key = m_anykeep->takePendingWindowGeometryKey();
    if (key.isEmpty())
        return {};

    const QRect stored    = QSettings().value(key).toRect();
    const QRect rect      = WindowGeometryUtils::constrainToCurrentScreens(stored, QSize(320, 240));
    const bool  valid     = rect.isValid();
    const bool  keepAbove = QSettings().value(key + QStringLiteral(".always-on-top"), false).toBool();
    if (valid && rect != stored)
        QSettings().setValue(key, rect);
    // qInfo() << "Window geometry claimed:" << key << "stored:" << valid << rect;
    return QString::fromUtf8(QJsonDocument(QJsonObject {
                                               { QStringLiteral("key"), key },
                                               { QStringLiteral("valid"), valid },
                                               { QStringLiteral("x"), rect.x() },
                                               { QStringLiteral("y"), rect.y() },
                                               { QStringLiteral("width"), rect.width() },
                                               { QStringLiteral("height"), rect.height() },
                                               { QStringLiteral("keepAbove"), keepAbove },
                                           })
                                 .toJson(QJsonDocument::Compact));
}

void AnyKeepDBus::storeWindowGeometry(const QString &key, int x, int y, int width, int height)
{
    if (!key.startsWith(QLatin1String("geometry.")) || width <= 0 || height <= 0) {
        qWarning() << "Window geometry rejected:" << key << QRect(x, y, width, height);
        return;
    }
    // qDebug() << "Window geometry stored:" << key << QRect(x, y, width, height);
    QSettings().setValue(key, QRect(x, y, width, height));
}

void AnyKeepDBus::windowGeometryScriptReady()
{
    m_anykeep->windowGeometryBridgeReady();
    qInfo("AnyKeep compositor geometry bridge is ready");
}

void AnyKeepDBus::openNote(const QString &storageId, const QString &noteId)
{
    if (!storageId.isEmpty() && !noteId.isEmpty())
        emit openNoteRequested(storageId, noteId);
}

void AnyKeepDBus::setXdgActivationToken(const QString &token)
{
    if (!token.isEmpty())
        qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
}

void AnyKeepDBus::createNote() { emit createNoteRequested(); }
void AnyKeepDBus::openStickyNote(const QString &stickyId) { m_anykeep->stickyNotesManager()->open(QUuid(stickyId)); }
void AnyKeepDBus::unpinStickyNote(const QString &stickyId) { m_anykeep->stickyNotesManager()->unpin(QUuid(stickyId)); }
void AnyKeepDBus::unpinStickyNoteForPresentation(const QString &presentationId)
{
    m_anykeep->stickyNotesManager()->unpinPresentation(presentationId);
}
void AnyKeepDBus::activateGlobalShortcut(const QString &id)
{
    if (!id.isEmpty())
        emit globalShortcutActivated(id);
}
void AnyKeepDBus::showNoteManager() { emit noteManagerRequested(); }
void AnyKeepDBus::showOptions() { emit optionsRequested(); }
void AnyKeepDBus::showAbout() { emit aboutRequested(); }
void AnyKeepDBus::quit() { emit quitRequested(); }

} // namespace AnyKeep
