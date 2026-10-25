#ifndef ANYKEEP_MEDIAPREVIEWCONTROLLER_H
#define ANYKEEP_MEDIAPREVIEWCONTROLLER_H

#include "anykeep_export.h"

#include <QObject>
#include <QUrl>
#include <memory>

namespace AnyKeep {
class NoteEditor;
class LocalMediaStore;

// Derived, session-only posters. Extraction never starts audible playback or
// changes the document, its manifest, undo history, or the shared player.
class ANYKEEP_EXPORT MediaPreviewController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    explicit MediaPreviewController(NoteEditor *editor, QObject *parent = nullptr, LocalMediaStore *store = nullptr);
    ~MediaPreviewController() override;
    int              revision() const;
    Q_INVOKABLE QUrl posterForUri(const QString &uri) const;
    Q_INVOKABLE void requestPreview(const QString &uri, bool retry = false);

signals:
    void changed();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace AnyKeep
#endif
