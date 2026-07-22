#include "dialogservice.h"

#include <utility>

namespace QtNote {

DialogService::DialogService(QObject *parent) : QObject(parent) { }

quint64 DialogService::requestId() const { return active_ ? current_.id : 0; }
QString DialogService::title() const { return active_ ? current_.title : QString(); }
QString DialogService::message() const { return active_ ? current_.message : QString(); }
QString DialogService::acceptText() const { return active_ ? current_.acceptText : QString(); }
QString DialogService::rejectText() const { return active_ ? current_.rejectText : QString(); }
bool    DialogService::destructive() const { return active_ && current_.destructive; }

quint64 DialogService::confirm(const QString &title, const QString &message, const QString &acceptText,
                               const QString &rejectText, bool destructive)
{
    Request request;
    request.title       = title;
    request.message     = message;
    request.acceptText  = acceptText.isEmpty() ? tr("OK") : acceptText;
    request.rejectText  = rejectText.isEmpty() ? tr("Cancel") : rejectText;
    request.destructive = destructive;
    return enqueue(std::move(request));
}

quint64 DialogService::inform(const QString &title, const QString &message, const QString &acceptText)
{
    Request request;
    request.title      = title;
    request.message    = message;
    request.acceptText = acceptText.isEmpty() ? tr("OK") : acceptText;
    return enqueue(std::move(request));
}

void DialogService::accept() { finish(true); }
void DialogService::reject() { finish(false); }

quint64 DialogService::enqueue(Request request)
{
    request.id = nextId_++;
    if (!request.id)
        request.id = nextId_++;
    const auto id = request.id;
    pending_.enqueue(std::move(request));
    showNext();
    return id;
}

void DialogService::finish(bool accepted)
{
    if (!active_)
        return;
    const auto id = current_.id;
    current_      = {};
    active_       = false;
    emit activeChanged();
    emit completed(id, accepted);
    showNext();
}

void DialogService::showNext()
{
    if (active_ || pending_.isEmpty())
        return;
    current_ = pending_.dequeue();
    active_  = true;
    emit activeChanged();
}

} // namespace QtNote
