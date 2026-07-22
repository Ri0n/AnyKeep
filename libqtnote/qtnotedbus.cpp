/*
    SPDX-License-Identifier: GPL-3.0-only
*/

#include "qtnotedbus.h"

#include <QByteArray>
#include <QDBusConnection>
#include <QDBusError>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QtGlobal>

#include "notemanager.h"
#include "qtnote.h"
#include "shortcutsmanager.h"
#include "stickynotesmanager.h"

namespace QtNote {

namespace {
    constexpr auto ServiceName          = "com.github.ri0n.QtNote";
    constexpr auto ObjectPath           = "/QtNote";
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
}

QtNoteDBus::QtNoteDBus(Main *qtnote, QObject *parent) : QObject(parent), m_qtnote(qtnote)
{
    auto *manager = NoteManager::instance();
    connect(manager, &NoteManager::storageAdded, this, &QtNoteDBus::notesChanged);
    connect(manager, &NoteManager::storageRemoved, this, &QtNoteDBus::notesChanged);
    connect(manager, &NoteManager::storageChanged, this, &QtNoteDBus::notesChanged);
    connect(qtnote, &Main::settingsUpdated, this, &QtNoteDBus::notesChanged);
    connect(qtnote, &Main::settingsUpdated, this, &QtNoteDBus::globalShortcutsChanged);
    connect(qtnote->stickyNotesManager(), &StickyNotesManager::notesChanged, this, &QtNoteDBus::stickyNotesChanged);

    auto bus = QDBusConnection::sessionBus();
    if (!bus.registerObject(QLatin1String(ObjectPath), this,
                            QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals)) {
        qWarning("Failed to register the QtNote D-Bus object: %s", qPrintable(bus.lastError().message()));
        return;
    }

    m_registeredService = bus.registerService(QLatin1String(ServiceName));
    if (!m_registeredService) {
        qWarning("Failed to register the QtNote D-Bus service: %s", qPrintable(bus.lastError().message()));
        bus.unregisterObject(QLatin1String(ObjectPath));
    }
}

QtNoteDBus::~QtNoteDBus()
{
    if (!m_registeredService)
        return;

    auto bus = QDBusConnection::sessionBus();
    bus.unregisterObject(QLatin1String(ObjectPath));
    bus.unregisterService(QLatin1String(ServiceName));
}

QString QtNoteDBus::notesJson(int offset, int limit, const QString &query) const
{
    offset = qMax(0, offset);
    limit  = limit > 0 ? qMin(limit, MaxNotesPageSize) : DefaultNotesPageSize;

    const auto notes = NoteManager::instance()->noteList(offset, limit + 1, query);

    QJsonArray result;
    for (int i = 0, count = qMin(limit, notes.size()); i < count; ++i) {
        const auto &note = notes.at(i);
        result.append(QJsonObject {
            { "id", note.id() },
            { "storageId", note.storageId() },
            { "title", note.displayTitle() },
            { "tags", QJsonArray::fromStringList(note.tags()) },
            { "modified", note.lastChangeUTC().toUTC().toString(Qt::ISODateWithMs) },
        });
    }
    return QString::fromUtf8(QJsonDocument(QJsonObject {
                                               { "notes", result },
                                               { "hasMore", notes.size() > limit },
                                           })
                                 .toJson(QJsonDocument::Compact));
}

QString QtNoteDBus::globalShortcutsJson() const
{
    QJsonArray result;
    const auto ids = m_qtnote->shortcutsManager()->globalShortcutIds();
    for (const auto &id : ids) {
        result.append(QJsonObject {
            { "id", id },
            { "accelerator", acceleratorFromKeySequence(m_qtnote->shortcutsManager()->key(id)) },
        });
    }
    return QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
}

QString QtNoteDBus::stickyNotesJson() const { return m_qtnote->stickyNotesManager()->notesJson(); }

QString QtNoteDBus::stickyNoteJson(const QString &stickyId) const
{
    return m_qtnote->stickyNotesManager()->noteJson(QUuid(stickyId));
}

QString QtNoteDBus::stickyNoteForPresentationJson(const QString &presentationId) const
{
    return m_qtnote->stickyNotesManager()->noteForPresentationJson(presentationId);
}

QString QtNoteDBus::claimWindowGeometry()
{
    const QString key = m_qtnote->takePendingWindowGeometryKey();
    if (key.isEmpty())
        return {};

    const QRect rect  = QSettings().value(key).toRect();
    const bool  valid = rect.isValid();
    // qInfo() << "Window geometry claimed:" << key << "stored:" << valid << rect;
    return QString::fromUtf8(QJsonDocument(QJsonObject {
                                               { QStringLiteral("key"), key },
                                               { QStringLiteral("valid"), valid },
                                               { QStringLiteral("x"), rect.x() },
                                               { QStringLiteral("y"), rect.y() },
                                               { QStringLiteral("width"), rect.width() },
                                               { QStringLiteral("height"), rect.height() },
                                           })
                                 .toJson(QJsonDocument::Compact));
}

void QtNoteDBus::storeWindowGeometry(const QString &key, int x, int y, int width, int height)
{
    if (!key.startsWith(QLatin1String("geometry.")) || width <= 0 || height <= 0) {
        qWarning() << "Window geometry rejected:" << key << QRect(x, y, width, height);
        return;
    }
    // qDebug() << "Window geometry stored:" << key << QRect(x, y, width, height);
    QSettings().setValue(key, QRect(x, y, width, height));
}

void QtNoteDBus::windowGeometryScriptReady()
{
    m_qtnote->windowGeometryBridgeReady();
    qInfo("QtNote compositor geometry bridge is ready");
}

void QtNoteDBus::openNote(const QString &storageId, const QString &noteId)
{
    if (!storageId.isEmpty() && !noteId.isEmpty())
        emit openNoteRequested(storageId, noteId);
}

void QtNoteDBus::setXdgActivationToken(const QString &token)
{
    if (!token.isEmpty())
        qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
}

void QtNoteDBus::createNote() { emit createNoteRequested(); }
void QtNoteDBus::openStickyNote(const QString &stickyId) { m_qtnote->stickyNotesManager()->open(QUuid(stickyId)); }
void QtNoteDBus::unpinStickyNote(const QString &stickyId) { m_qtnote->stickyNotesManager()->unpin(QUuid(stickyId)); }
void QtNoteDBus::activateGlobalShortcut(const QString &id)
{
    if (!id.isEmpty())
        emit globalShortcutActivated(id);
}
void QtNoteDBus::showNoteManager() { emit noteManagerRequested(); }
void QtNoteDBus::showOptions() { emit optionsRequested(); }
void QtNoteDBus::showAbout() { emit aboutRequested(); }
void QtNoteDBus::quit() { emit quitRequested(); }

} // namespace QtNote
