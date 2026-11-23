#include "keychainservice.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <qt6keychain/keychain.h>

#include <utility>

namespace AnyKeep {
namespace {
    enum class Operation { ReadBinary, ReadText, WriteBinary, WriteText, Remove };

    KeychainService::Status statusFor(QKeychain::Error error)
    {
        switch (error) {
        case QKeychain::NoError:
            return KeychainService::Status::Ok;
        case QKeychain::EntryNotFound:
            return KeychainService::Status::NotFound;
        case QKeychain::AccessDenied:
            return KeychainService::Status::AccessDenied;
        default:
            return KeychainService::Status::Failed;
        }
    }

    void deliver(QPointer<QObject> context, KeychainService::Callback callback, KeychainService::Result result)
    {
        if (!context)
            return;
        QMetaObject::invokeMethod(
            context,
            [context, callback = std::move(callback), result = std::move(result)]() mutable {
                if (context)
                    callback(std::move(result));
            },
            Qt::QueuedConnection);
    }

    void run(QObject *context, QString service, QString key, Operation operation, QByteArray binary, QString text,
             KeychainService::Callback callback)
    {
        if (!context || !callback)
            return;

        auto *app = QCoreApplication::instance();
        const QPointer<QObject> guard(context);
        if (!app) {
            deliver(guard, std::move(callback),
                    { KeychainService::Status::Unavailable, {}, {}, QStringLiteral("Application is shutting down") });
            return;
        }
        // QtKeychain jobs must be created and started on the application thread,
        // never directly from a media reader or an SCTP callback on a worker.
        if (QThread::currentThread() != app->thread()) {
            QMetaObject::invokeMethod(
                app,
                [guard, service = std::move(service), key = std::move(key), operation, binary = std::move(binary),
                 text = std::move(text), callback = std::move(callback)]() mutable {
                    if (guard)
                        run(guard, std::move(service), std::move(key), operation, std::move(binary), std::move(text),
                            std::move(callback));
                },
                Qt::QueuedConnection);
            return;
        }
        if (!QKeychain::isAvailable()) {
            deliver(guard, std::move(callback),
                    { KeychainService::Status::Unavailable, {}, {},
                      QStringLiteral("No secure system keychain is available") });
            return;
        }

        QKeychain::Job *job = nullptr;
        switch (operation) {
        case Operation::ReadBinary:
        case Operation::ReadText:
            job = new QKeychain::ReadPasswordJob(service, app);
            break;
        case Operation::WriteBinary: {
            auto *write = new QKeychain::WritePasswordJob(service, app);
            write->setBinaryData(binary);
            job = write;
            break;
        }
        case Operation::WriteText: {
            auto *write = new QKeychain::WritePasswordJob(service, app);
            write->setTextData(text);
            job = write;
            break;
        }
        case Operation::Remove:
            job = new QKeychain::DeletePasswordJob(service, app);
            break;
        }
        job->setAutoDelete(false);
        job->setInsecureFallback(false);
        job->setKey(key);
        QObject::connect(job, &QKeychain::Job::finished, job,
                         [job, operation, guard, callback = std::move(callback)]() mutable {
            KeychainService::Result result;
            result.status = statusFor(job->error());
            if (result.status == KeychainService::Status::Ok
                && (operation == Operation::ReadBinary || operation == Operation::ReadText)) {
                const auto *read = static_cast<QKeychain::ReadPasswordJob *>(job);
                if (operation == Operation::ReadBinary)
                    result.binary = read->binaryData();
                else
                    result.text = read->textData();
            }
            if (result.status != KeychainService::Status::Ok)
                result.error = job->errorString();
            job->deleteLater();
            deliver(guard, std::move(callback), std::move(result));
        });
        job->start();
    }
} // namespace

void KeychainService::readBinary(QObject *context, QString service, QString key, Callback callback)
{
    run(context, std::move(service), std::move(key), Operation::ReadBinary, {}, {}, std::move(callback));
}

void KeychainService::readText(QObject *context, QString service, QString key, Callback callback)
{
    run(context, std::move(service), std::move(key), Operation::ReadText, {}, {}, std::move(callback));
}

void KeychainService::writeBinary(QObject *context, QString service, QString key, QByteArray data, Callback callback)
{
    run(context, std::move(service), std::move(key), Operation::WriteBinary, std::move(data), {}, std::move(callback));
}

void KeychainService::writeText(QObject *context, QString service, QString key, QString data, Callback callback)
{
    run(context, std::move(service), std::move(key), Operation::WriteText, {}, std::move(data), std::move(callback));
}

void KeychainService::remove(QObject *context, QString service, QString key, Callback callback)
{
    run(context, std::move(service), std::move(key), Operation::Remove, {}, {}, std::move(callback));
}

} // namespace AnyKeep
