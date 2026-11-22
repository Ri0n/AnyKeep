#ifndef KEYCHAINSERVICE_H
#define KEYCHAINSERVICE_H

#include <QByteArray>
#include <QString>

#include <functional>

class QObject;

namespace AnyKeep {

// The only adapter that knows about QtKeychain. All platform operations are
// asynchronous, and results are delivered on the context object's thread.
// Destroying context cancels its pending callback, never the shared key state.
class KeychainService {
public:
    enum class Status { Ok, NotFound, AccessDenied, Unavailable, Failed };

    struct Result {
        Status     status { Status::Failed };
        QByteArray binary;
        QString    text;
        QString    error;
        explicit operator bool() const { return status == Status::Ok; }
    };

    using Callback = std::function<void(Result)>;

    static void read(QObject *context, QString service, QString key, Callback callback);
    static void writeBinary(QObject *context, QString service, QString key, QByteArray data, Callback callback);
    static void writeText(QObject *context, QString service, QString key, QString data, Callback callback);
    static void remove(QObject *context, QString service, QString key, Callback callback);
};

} // namespace AnyKeep

#endif // KEYCHAINSERVICE_H
