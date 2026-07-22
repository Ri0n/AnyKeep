#ifndef QTNOTE_DIALOGSERVICE_H
#define QTNOTE_DIALOGSERVICE_H

#include "qtnote_export.h"

#include <QObject>
#include <QQueue>

namespace QtNote {

class QTNOTE_EXPORT DialogService final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(quint64 requestId READ requestId NOTIFY activeChanged)
    Q_PROPERTY(QString title READ title NOTIFY activeChanged)
    Q_PROPERTY(QString message READ message NOTIFY activeChanged)
    Q_PROPERTY(QString acceptText READ acceptText NOTIFY activeChanged)
    Q_PROPERTY(QString rejectText READ rejectText NOTIFY activeChanged)
    Q_PROPERTY(bool destructive READ destructive NOTIFY activeChanged)

public:
    explicit DialogService(QObject *parent = nullptr);

    bool    active() const { return active_; }
    quint64 requestId() const;
    QString title() const;
    QString message() const;
    QString acceptText() const;
    QString rejectText() const;
    bool    destructive() const;

    Q_INVOKABLE quint64 confirm(const QString &title, const QString &message, const QString &acceptText = {},
                                const QString &rejectText = {}, bool destructive = false);
    Q_INVOKABLE quint64 inform(const QString &title, const QString &message, const QString &acceptText = {});
    Q_INVOKABLE void    accept();
    Q_INVOKABLE void    reject();

signals:
    void activeChanged();
    void completed(quint64 requestId, bool accepted);

private:
    struct Request {
        quint64 id = 0;
        QString title;
        QString message;
        QString acceptText;
        QString rejectText;
        bool    destructive = false;
    };

    quint64 enqueue(Request request);
    void    finish(bool accepted);
    void    showNext();

    QQueue<Request> pending_;
    Request         current_;
    quint64         nextId_ { 1 };
    bool            active_ { false };
};

} // namespace QtNote

#endif // QTNOTE_DIALOGSERVICE_H
