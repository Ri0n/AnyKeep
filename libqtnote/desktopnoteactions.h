#ifndef QTNOTE_DESKTOPNOTEACTIONS_H
#define QTNOTE_DESKTOPNOTEACTIONS_H

#include "qtnote_export.h"

#include <QObject>
#include <QPointer>

namespace QtNote {

class NoteEditor;

class QTNOTE_EXPORT DesktopNoteActions final : public QObject {
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

} // namespace QtNote

#endif // QTNOTE_DESKTOPNOTEACTIONS_H
