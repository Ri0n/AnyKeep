#ifndef STICKYNOTESINTEGRATIONINTERFACE_H
#define STICKYNOTESINTEGRATIONINTERFACE_H

#include <QRect>
#include <QUuid>

class QObject;

namespace AnyKeep {

class StickyNotesServiceInterface {
public:
    virtual ~StickyNotesServiceInterface() = default;

    virtual QString  noteJson(const QUuid &stickyId) const = 0;
    virtual bool     open(const QUuid &stickyId)           = 0;
    virtual bool     unpin(const QUuid &stickyId)          = 0;
    virtual QObject *notifier()                            = 0;
};

class StickyNotesIntegrationInterface {
public:
    virtual ~StickyNotesIntegrationInterface() = default;

    virtual bool  isStickyNotesAvailable() const                                           = 0;
    virtual bool  presentStickyNote(const QUuid &stickyId, const QRect &preferredGeometry) = 0;
    virtual bool  dismissStickyNote(const QUuid &stickyId)                                 = 0;
    virtual QUuid stickyNoteIdForPresentation(const QString &presentationId) const { return {}; }
};

class StickyNotesHostInterface {
public:
    virtual ~StickyNotesHostInterface()                                      = default;
    virtual void initializeStickyNotes(StickyNotesServiceInterface *service) = 0;
    virtual bool stickyNotesRequireApplicationAutostart() const              = 0;
};

} // namespace AnyKeep

Q_DECLARE_INTERFACE(AnyKeep::StickyNotesIntegrationInterface,
                    "com.rion-soft.AnyKeep.StickyNotesIntegrationInterface/1.0")
Q_DECLARE_INTERFACE(AnyKeep::StickyNotesHostInterface, "com.rion-soft.AnyKeep.StickyNotesHostInterface/1.0")

#endif // STICKYNOTESINTEGRATIONINTERFACE_H
