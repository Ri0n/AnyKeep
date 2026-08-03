#ifndef STICKYNOTESMANAGER_H
#define STICKYNOTESMANAGER_H

#include <QHash>
#include <QObject>
#include <QRect>
#include <QUuid>

#include "note.h"
#include "stickynotesintegrationinterface.h"

namespace AnyKeep {

class Main;
class StickyNotesIntegrationInterface;

class StickyNotesManager : public QObject, public StickyNotesServiceInterface {
    Q_OBJECT
public:
    explicit StickyNotesManager(Main *main);

    void setBackend(StickyNotesIntegrationInterface *backend);
    void setRequiresApplicationAutostart(bool required) { requiresApplicationAutostart_ = required; }
    bool isAvailable() const;

    void requestPin(const Note &note, const QUuid &draftId, bool awaitingPublication, const QRect &preferredGeometry);
    bool unpin(const QUuid &stickyId) override;
    bool open(const QUuid &stickyId) override;

    QString  notesJson() const;
    QString  noteJson(const QUuid &stickyId) const override;
    QString  noteForPresentationJson(const QString &presentationId) const;
    QObject *notifier() override { return this; }

signals:
    void availabilityChanged(bool available);
    void notesChanged();

private:
    struct Record {
        QUuid   id;
        QString storageId;
        QString noteId;
    };
    struct PendingPin {
        QRect preferredGeometry;
    };

    void          load();
    void          save(const Record &record);
    void          remove(const QUuid &stickyId);
    void          watchStorage(NoteStorage *storage);
    QUuid         pinPublished(const Note &note, const QRect &preferredGeometry);
    const Record *record(const QUuid &stickyId) const;

    Main                            *main_    = nullptr;
    StickyNotesIntegrationInterface *backend_ = nullptr;
    QHash<QUuid, Record>             records_;
    QHash<QUuid, PendingPin>         pendingPins_;
    bool                             requiresApplicationAutostart_ = false;
    bool                             autostartPromptShown_         = false;
};

} // namespace AnyKeep

#endif // STICKYNOTESMANAGER_H
