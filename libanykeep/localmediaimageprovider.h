#ifndef LOCALMEDIAIMAGEPROVIDER_H
#define LOCALMEDIAIMAGEPROVIDER_H

#include "anykeep_export.h"

#include <QQuickImageProvider>

class QQmlEngine;

namespace AnyKeep {

class LocalMediaStore;
struct MediaReference;

class ANYKEEP_EXPORT LocalMediaImageProvider final : public QQuickImageProvider {
public:
    LocalMediaImageProvider();
    explicit LocalMediaImageProvider(LocalMediaStore *store);
    // Carries attachment identity for lazy external sources, without exposing
    // their local filesystem locator to QML or copying the image into a blob.
    static QString imageUrl(const MediaReference &reference);
    QImage         requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    LocalMediaStore *store_;
};

ANYKEEP_EXPORT void installLocalMediaImageProvider(QQmlEngine *engine);

} // namespace AnyKeep

#endif // LOCALMEDIAIMAGEPROVIDER_H
