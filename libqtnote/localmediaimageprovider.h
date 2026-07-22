#ifndef LOCALMEDIAIMAGEPROVIDER_H
#define LOCALMEDIAIMAGEPROVIDER_H

#include "qtnote_export.h"

#include <QQuickImageProvider>

class QQmlEngine;

namespace QtNote {

class QTNOTE_EXPORT LocalMediaImageProvider final : public QQuickImageProvider {
public:
    LocalMediaImageProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

QTNOTE_EXPORT void installLocalMediaImageProvider(QQmlEngine *engine);

} // namespace QtNote

#endif // LOCALMEDIAIMAGEPROVIDER_H
