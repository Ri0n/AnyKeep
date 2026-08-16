#ifndef TRAYIMPL_H
#define TRAYIMPL_H

#include <QObject>

#include "notemanager.h"

namespace AnyKeep {

class TrayImpl : public QObject {
    Q_OBJECT

public:
    TrayImpl(QObject *parent = 0) : QObject(parent) {}

signals:
    void exitTriggered();
    void newNoteTriggered();
    void noteManagerTriggered();
    void optionsTriggered();
    void aboutTriggered();
    void showNoteTriggered(const QString &storageId, const QString &noteId);
};

} // namespace AnyKeep

#endif // TRAYIMPL_H
