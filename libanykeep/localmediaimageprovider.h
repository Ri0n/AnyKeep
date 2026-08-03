#ifndef LOCALMEDIAIMAGEPROVIDER_H
#define LOCALMEDIAIMAGEPROVIDER_H

#include "anykeep_export.h"

#include <QQuickImageProvider>

class QQmlEngine;

namespace AnyKeep {

class ANYKEEP_EXPORT LocalMediaImageProvider final : public QQuickImageProvider {
public:
    LocalMediaImageProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

ANYKEEP_EXPORT void installLocalMediaImageProvider(QQmlEngine *engine);

} // namespace AnyKeep

#endif // LOCALMEDIAIMAGEPROVIDER_H
