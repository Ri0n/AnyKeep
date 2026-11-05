#ifndef NOTEWINDOWGEOMETRYSTORE_H
#define NOTEWINDOWGEOMETRYSTORE_H

#include "anykeep_export.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QUuid>

#include <optional>

namespace AnyKeep {

// Geometry of standalone note windows, keyed by a note window id: the
// storage and note ids of a published note, or the draft id of a new one.
// The client geometry (without window decorations) is kept in a cache file,
// apart from the settings, together with the window's keep-above state and
// the draft that last showed the window, so that publishing a draft after
// its window closed can still carry the geometry over to the new id. Only
// the most recently used windows are remembered.
class ANYKEEP_EXPORT NoteWindowGeometryStore final : public QObject {
    Q_OBJECT
public:
    static constexpr int MaxWindows = 50;

    struct Entry {
        QRect   geometry;
        bool    keepAbove = false;
        QUuid   draftId;
        quint64 lastUse = 0; // order of use; a window used later has a higher value
    };

    explicit NoteWindowGeometryStore(const QString &filePath, QObject *parent = nullptr);
    ~NoteWindowGeometryStore() override;

    // The application's store in the cache directory.
    static NoteWindowGeometryStore *instance();
    static QString                  windowId(const QString &storageId, const QString &noteId, const QUuid &draftId);
    static QString                  tutorialWindowId() { return QStringLiteral("tutorial"); }

    // Showing, moving, resizing or pinning a window counts as a use of it.
    // Beyond MaxWindows, the windows used longest ago are forgotten.
    std::optional<Entry> entry(const QString &id) const;
    void                 setGeometry(const QString &id, const QRect &geometry, const QUuid &draftId = {});
    void                 setKeepAbove(const QString &id, bool keepAbove, const QUuid &draftId = {});
    // A window is shown, by the given draft. The draft is recorded also when
    // a compositor helper reports the window's geometry.
    void markShown(const QString &id, const QUuid &draftId);
    void remove(const QString &id);
    // Moves an entry to a new id, replacing an entry stored there. Until the
    // application exits, later writes to the old id go to the new one: a
    // compositor helper may still report the final geometry of a window
    // closed under its draft id after that draft was published.
    void rename(const QString &from, const QString &to);
    // Moves the entry last shown for the draft to the draft's published id.
    void renameDraft(const QUuid &draftId, const QString &to);

    // Every entry as { id: { x, y, width, height, keepAbove } }.
    QJsonObject toJson() const;
    // Writes pending changes now; changes are otherwise written shortly after.
    void flush();

private:
    void        load();
    void        changed();
    Entry      &use(const QString &id);
    void        forgetLeastRecentlyUsed();
    QString     resolved(const QString &id) const;
    static bool validId(const QString &id);

    QString                 path_;
    QHash<QString, Entry>   entries_;
    QHash<QString, QString> renamed_; // old id -> new id, this session only
    QTimer                  saveTimer_;
    quint64                 lastUse_ = 0;
    bool                    dirty_   = false;
};

} // namespace AnyKeep

#endif // NOTEWINDOWGEOMETRYSTORE_H
