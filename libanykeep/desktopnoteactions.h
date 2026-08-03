#ifndef ANYKEEP_DESKTOPNOTEACTIONS_H
#define ANYKEEP_DESKTOPNOTEACTIONS_H

#include "anykeep_export.h"

#include <QObject>
#include <QPointer>

namespace AnyKeep {

class NoteEditor;

class ANYKEEP_EXPORT DesktopNoteActions final : public QObject {
    Q_OBJECT

public:
    explicit DesktopNoteActions(QObject *parent = nullptr);

    void setEditor(NoteEditor *editor);

    Q_INVOKABLE bool printNote();
    Q_INVOKABLE bool exportNote();

signals:
    void operationFailed(const QString &message);
    void operationCompleted(const QString &message);

private:
    QPointer<NoteEditor> editor_;
    QString              exportFileName_;
    QString              exportFilter_;
};

} // namespace AnyKeep

#endif // ANYKEEP_DESKTOPNOTEACTIONS_H
