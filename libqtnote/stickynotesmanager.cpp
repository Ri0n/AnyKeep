#include "stickynotesmanager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QSettings>

#include "draftmanager.h"
#include "notemanager.h"
#include "qtnote.h"
#include "stickynotesintegrationinterface.h"
#include "utils.h"

namespace QtNote {

namespace {
    constexpr auto SettingsGroup = "stickyNotes";

    QString formatName(Note::Format format)
    {
        switch (format) {
        case Note::Markdown:
            return QStringLiteral("markdown");
        case Note::Html:
            return QStringLiteral("html");
        case Note::PlainText:
            return QStringLiteral("plainText");
        }
        return QStringLiteral("plainText");
    }
} // namespace

StickyNotesManager::StickyNotesManager(Main *main) : QObject(main), main_(main)
{
    load();
    connect(DraftManager::instance(), &DraftManager::draftPublished, this,
            [this](const QUuid &draftId, const Note &note) {
                if (!pendingPins_.contains(draftId))
                    return;
                const auto pending = pendingPins_.take(draftId);
                if (!note.id().isEmpty())
                    pinPublished(note, pending.preferredGeometry);
            });
    connect(NoteManager::instance(), &NoteManager::storageChanged, this, &StickyNotesManager::notesChanged);
    connect(NoteManager::instance(), &NoteManager::storageAdded, this,
            [this](const NoteStorage::Ptr &storage) { watchStorage(storage); });
    for (const auto &storage : NoteManager::instance()->storages(true))
        watchStorage(storage);
}

void StickyNotesManager::setBackend(StickyNotesIntegrationInterface *backend)
{
    const bool wasAvailable = isAvailable();
    backend_                = backend;
    const bool available    = isAvailable();
    if (available) {
        for (const auto &record : std::as_const(records_))
            backend_->presentStickyNote(record.id, {});
    }
    if (wasAvailable != available)
        emit availabilityChanged(available);
}

bool StickyNotesManager::isAvailable() const { return backend_ && backend_->isStickyNotesAvailable(); }

void StickyNotesManager::requestPin(const Note &note, const QUuid &draftId, bool awaitingPublication,
                                    const QRect &preferredGeometry)
{
    if (!isAvailable())
        return;
    if (awaitingPublication) {
        pendingPins_.insert(draftId, PendingPin { preferredGeometry });
        return;
    }
    if (!note.id().isEmpty())
        pinPublished(note, preferredGeometry);
}

QUuid StickyNotesManager::pinPublished(const Note &note, const QRect &preferredGeometry)
{
    for (const auto &record : std::as_const(records_)) {
        if (record.storageId == note.storageId() && record.noteId == note.id()) {
            if (backend_)
                backend_->presentStickyNote(record.id, preferredGeometry);
            return record.id;
        }
    }

    if (requiresApplicationAutostart_ && !autostartPromptShown_ && !Utils::isAutostartEnabled()) {
        autostartPromptShown_ = true;
        const auto answer     = QMessageBox::question(
            nullptr, tr("Start QtNote automatically?"),
            tr("QtNote must be running to restore sticky notes after sign-in. Start it automatically with the system?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (answer == QMessageBox::Yes && !Utils::setAutostartEnabled(true))
            main_->notifyError(tr("Failed to enable automatic startup."));
    }

    Record record { QUuid::createUuid(), note.storageId(), note.id() };
    records_.insert(record.id, record);
    save(record);
    if (backend_ && !backend_->presentStickyNote(record.id, preferredGeometry)) {
        qWarning("Failed to present sticky note %s", qPrintable(record.id.toString(QUuid::WithoutBraces)));
    }
    emit notesChanged();
    return record.id;
}

bool StickyNotesManager::unpin(const QUuid &stickyId)
{
    if (!records_.remove(stickyId))
        return false;
    remove(stickyId);
    if (backend_)
        backend_->dismissStickyNote(stickyId);
    emit notesChanged();
    return true;
}

bool StickyNotesManager::open(const QUuid &stickyId)
{
    const auto *item = record(stickyId);
    if (!item)
        return false;
    main_->openNoteDialog(item->storageId, item->noteId);
    return true;
}

QString StickyNotesManager::notesJson() const
{
    QJsonArray result;
    for (const auto &item : std::as_const(records_))
        result.append(QJsonObject { { "stickyId", item.id.toString(QUuid::WithoutBraces) } });
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

QString StickyNotesManager::noteJson(const QUuid &stickyId) const
{
    const auto *item = record(stickyId);
    if (!item)
        return {};
    auto note = NoteManager::instance()->note(item->storageId, item->noteId);
    if (note.isNull())
        return {};
    if (!note.isLoaded() && !note.load())
        return {};
    return QString::fromUtf8(QJsonDocument(QJsonObject {
                                               { "stickyId", item->id.toString(QUuid::WithoutBraces) },
                                               { "storageId", item->storageId },
                                               { "noteId", item->noteId },
                                               { "title", note.displayTitle() },
                                               { "body", note.text() },
                                               { "format", formatName(note.format()) },
                                           })
                                 .toJson(QJsonDocument::Compact));
}

QString StickyNotesManager::noteForPresentationJson(const QString &presentationId) const
{
    if (!backend_)
        return {};
    return noteJson(backend_->stickyNoteIdForPresentation(presentationId));
}

const StickyNotesManager::Record *StickyNotesManager::record(const QUuid &stickyId) const
{
    const auto it = records_.constFind(stickyId);
    return it == records_.constEnd() ? nullptr : &it.value();
}

void StickyNotesManager::load()
{
    QSettings settings;
    settings.beginGroup(QLatin1String(SettingsGroup));
    for (const auto &group : settings.childGroups()) {
        const QUuid id(group);
        if (id.isNull())
            continue;
        settings.beginGroup(group);
        Record record { id, settings.value(QStringLiteral("storageId")).toString(),
                        settings.value(QStringLiteral("noteId")).toString() };
        settings.endGroup();
        if (!record.storageId.isEmpty() && !record.noteId.isEmpty())
            records_.insert(id, record);
    }
    settings.endGroup();
}

void StickyNotesManager::save(const Record &record)
{
    QSettings settings;
    settings.beginGroup(QLatin1String(SettingsGroup));
    settings.beginGroup(record.id.toString(QUuid::WithoutBraces));
    settings.setValue(QStringLiteral("storageId"), record.storageId);
    settings.setValue(QStringLiteral("noteId"), record.noteId);
}

void StickyNotesManager::remove(const QUuid &stickyId)
{
    QSettings settings;
    settings.beginGroup(QLatin1String(SettingsGroup));
    settings.remove(stickyId.toString(QUuid::WithoutBraces));
}

void StickyNotesManager::watchStorage(NoteStorage *storage)
{
    if (!storage)
        return;
    connect(storage, &NoteStorage::noteIdChanged, this, [this, storage](const Note &note, const QString &oldId) {
        bool changed = false;
        for (auto it = records_.begin(); it != records_.end(); ++it) {
            if (it->storageId == storage->systemName() && it->noteId == oldId) {
                it->noteId = note.id();
                save(*it);
                changed = true;
            }
        }
        if (changed)
            emit notesChanged();
    });
    connect(storage, &NoteStorage::noteRemoved, this, [this](const Note &note) {
        QList<QUuid> removed;
        for (const auto &item : std::as_const(records_)) {
            if (item.storageId == note.storageId() && item.noteId == note.id())
                removed.append(item.id);
        }
        for (const auto &id : std::as_const(removed))
            unpin(id);
    });
}

} // namespace QtNote
