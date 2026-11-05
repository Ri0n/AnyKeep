#include "notewindowgeometrystore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace AnyKeep {
namespace {
    Q_LOGGING_CATEGORY(logNoteWindowGeometry, "anykeep.windows.geometry")

    constexpr int FileVersion = 1;
    // Geometry follows a window being dragged; write it once it settles.
    constexpr int SaveDelayMs = 500;
} // namespace

NoteWindowGeometryStore::NoteWindowGeometryStore(const QString &filePath, QObject *parent) :
    QObject(parent), path_(filePath)
{
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(SaveDelayMs);
    connect(&saveTimer_, &QTimer::timeout, this, &NoteWindowGeometryStore::flush);
    load();
}

NoteWindowGeometryStore::~NoteWindowGeometryStore() { flush(); }

NoteWindowGeometryStore *NoteWindowGeometryStore::instance()
{
    static NoteWindowGeometryStore *store = [] {
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        auto         *created   = new NoteWindowGeometryStore(
            QDir(directory).filePath(QStringLiteral("note-window-geometry.json")), QCoreApplication::instance());
        if (auto *application = QCoreApplication::instance())
            QObject::connect(application, &QCoreApplication::aboutToQuit, created, &NoteWindowGeometryStore::flush);
        return created;
    }();
    return store;
}

QString NoteWindowGeometryStore::windowId(const QString &storageId, const QString &noteId, const QUuid &draftId)
{
    if (!storageId.isEmpty() && !noteId.isEmpty())
        return storageId + QLatin1Char('/') + noteId;
    if (!draftId.isNull())
        return QStringLiteral("draft/") + draftId.toString(QUuid::WithoutBraces);
    return {};
}

bool NoteWindowGeometryStore::validId(const QString &id) { return !id.isEmpty() && id.size() <= 1024; }

QString NoteWindowGeometryStore::resolved(const QString &id) const
{
    QString current = id;
    // Renames form chains (draft -> note -> note in another storage); a cycle
    // cannot loop forever.
    for (int step = 0; step < 8; ++step) {
        const auto it = renamed_.constFind(current);
        if (it == renamed_.cend())
            break;
        current = *it;
    }
    return current;
}

std::optional<NoteWindowGeometryStore::Entry> NoteWindowGeometryStore::entry(const QString &id) const
{
    const auto it = entries_.constFind(resolved(id));
    if (it == entries_.cend())
        return std::nullopt;
    return *it;
}

NoteWindowGeometryStore::Entry &NoteWindowGeometryStore::use(const QString &id)
{
    Entry &entry  = entries_[resolved(id)];
    entry.lastUse = ++lastUse_;
    return entry;
}

void NoteWindowGeometryStore::forgetLeastRecentlyUsed()
{
    if (entries_.size() <= MaxWindows)
        return;
    QList<std::pair<quint64, QString>> byUse;
    byUse.reserve(entries_.size());
    for (auto it = entries_.cbegin(); it != entries_.cend(); ++it)
        byUse.append({ it.value().lastUse, it.key() });
    std::sort(byUse.begin(), byUse.end());
    const qsizetype excess = entries_.size() - MaxWindows;
    for (qsizetype index = 0; index < excess; ++index)
        entries_.remove(byUse.at(index).second);
}

void NoteWindowGeometryStore::setGeometry(const QString &id, const QRect &geometry, const QUuid &draftId)
{
    if (!validId(id) || !geometry.isValid())
        return;
    Entry &entry   = use(id);
    entry.geometry = geometry;
    if (!draftId.isNull())
        entry.draftId = draftId;
    forgetLeastRecentlyUsed();
    changed();
}

void NoteWindowGeometryStore::setKeepAbove(const QString &id, bool keepAbove, const QUuid &draftId)
{
    if (!validId(id))
        return;
    Entry &entry    = use(id);
    entry.keepAbove = keepAbove;
    if (!draftId.isNull())
        entry.draftId = draftId;
    forgetLeastRecentlyUsed();
    changed();
}

void NoteWindowGeometryStore::markShown(const QString &id, const QUuid &draftId)
{
    if (!validId(id))
        return;
    Entry &entry = use(id);
    if (!draftId.isNull())
        entry.draftId = draftId;
    forgetLeastRecentlyUsed();
    changed();
}

void NoteWindowGeometryStore::remove(const QString &id)
{
    if (entries_.remove(resolved(id)))
        changed();
}

void NoteWindowGeometryStore::rename(const QString &from, const QString &to)
{
    if (from == to || !validId(to))
        return;
    renamed_.insert(from, to);
    renamed_.remove(to);
    const auto it = entries_.find(from);
    if (it == entries_.end())
        return;
    Entry entry = *it;
    entries_.erase(it);
    entry.lastUse = ++lastUse_;
    entries_.insert(to, entry);
    changed();
}

void NoteWindowGeometryStore::renameDraft(const QUuid &draftId, const QString &to)
{
    if (draftId.isNull() || !validId(to))
        return;
    QString from;
    for (auto it = entries_.cbegin(); it != entries_.cend(); ++it) {
        if (it.value().draftId == draftId && it.key() != to) {
            from = it.key();
            break;
        }
    }
    if (!from.isEmpty())
        rename(from, to);
}

QJsonObject NoteWindowGeometryStore::toJson() const
{
    QJsonObject result;
    for (auto it = entries_.cbegin(); it != entries_.cend(); ++it) {
        const Entry &entry = it.value();
        QJsonObject  value { { QStringLiteral("keepAbove"), entry.keepAbove } };
        if (entry.geometry.isValid()) {
            value.insert(QStringLiteral("x"), entry.geometry.x());
            value.insert(QStringLiteral("y"), entry.geometry.y());
            value.insert(QStringLiteral("width"), entry.geometry.width());
            value.insert(QStringLiteral("height"), entry.geometry.height());
        }
        result.insert(it.key(), value);
    }
    return result;
}

void NoteWindowGeometryStore::changed()
{
    dirty_ = true;
    saveTimer_.start();
}

void NoteWindowGeometryStore::load()
{
    QFile file(path_);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(logNoteWindowGeometry) << "Cannot read note window geometry:" << file.errorString();
        return;
    }
    const QJsonObject root    = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject windows = root.value(QStringLiteral("windows")).toObject();
    for (auto it = windows.constBegin(); it != windows.constEnd(); ++it) {
        if (!validId(it.key()))
            continue;
        const QJsonObject value = it.value().toObject();
        Entry             entry;
        entry.geometry
            = QRect(value.value(QStringLiteral("x")).toInt(), value.value(QStringLiteral("y")).toInt(),
                    value.value(QStringLiteral("width")).toInt(), value.value(QStringLiteral("height")).toInt());
        entry.keepAbove = value.value(QStringLiteral("keepAbove")).toBool();
        entry.draftId   = QUuid(value.value(QStringLiteral("draft")).toString());
        // Files without the order of use (version 1 before it was added) rank
        // every window as used before any new use.
        entry.lastUse = quint64(qMax<qint64>(0, value.value(QStringLiteral("used")).toInteger()));
        lastUse_      = qMax(lastUse_, entry.lastUse);
        entries_.insert(it.key(), entry);
    }
    forgetLeastRecentlyUsed();
}

void NoteWindowGeometryStore::flush()
{
    saveTimer_.stop();
    if (!dirty_)
        return;
    QJsonObject windows;
    for (auto it = entries_.cbegin(); it != entries_.cend(); ++it) {
        const Entry &entry = it.value();
        QJsonObject  value {
            { QStringLiteral("x"), entry.geometry.x() },         { QStringLiteral("y"), entry.geometry.y() },
            { QStringLiteral("width"), entry.geometry.width() }, { QStringLiteral("height"), entry.geometry.height() },
            { QStringLiteral("keepAbove"), entry.keepAbove },    { QStringLiteral("used"), qint64(entry.lastUse) },
        };
        if (!entry.draftId.isNull())
            value.insert(QStringLiteral("draft"), entry.draftId.toString(QUuid::WithoutBraces));
        windows.insert(it.key(), value);
    }
    const QJsonObject root { { QStringLiteral("version"), FileVersion }, { QStringLiteral("windows"), windows } };

    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0
        || !file.commit()) {
        qCWarning(logNoteWindowGeometry) << "Cannot write note window geometry:" << file.errorString();
        return;
    }
    dirty_ = false;
}

} // namespace AnyKeep
