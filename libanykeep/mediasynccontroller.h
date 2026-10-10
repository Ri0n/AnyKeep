#ifndef ANYKEEP_MEDIASYNCCONTROLLER_H
#define ANYKEEP_MEDIASYNCCONTROLLER_H

#include "anykeep_export.h"
#include "mediareference.h"
#include "mediasyncservice.h"
#include <QHash>
#include <QObject>
#include <QVariantMap>

namespace AnyKeep {
class NoteEditor;

// Observes only the currently referenced media of one live editor. No
// synchronization state is written to the document, history or draft store.
class ANYKEEP_EXPORT MediaSyncController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(int state READ state NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    explicit MediaSyncController(NoteEditor *editor, QObject *parent = nullptr);
    ~MediaSyncController() override;

    bool hasMedia() const { return !media_.isEmpty(); }
    double progress() const;
    int state() const;
    int revision() const { return revision_; }
    Q_INVOKABLE QVariantMap statusForUri(const QString &uri) const;

signals:
    void changed();

private:
    MediaSyncSnapshot status(const MediaReference &reference) const;
    void refresh();
    void notifyChange();

    NoteEditor *editor_;
    QList<MediaReference> media_;
    QHash<QString, MediaReference> byUri_;
    int revision_ = 0;
};
} // namespace AnyKeep
#endif
