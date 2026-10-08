#include "mediarangeservice.h"

#include <QCoreApplication>
#include <QHash>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <memory>

namespace AnyKeep {
namespace {
    struct Resolver {
        QPointer<QObject>          owner;
        MediaRangeService::Accepts accepts;
        MediaRangeService::Reader  read;
    };
    struct Resource {
        QPointer<QObject>         owner;
        MediaReference            reference;
        MediaRangeService::Reader read;
    };
    class Server : public QObject {
    public:
        Server() : QObject(QCoreApplication::instance())
        {
            connect(&server, &QTcpServer::newConnection, this, [this] {
                while (auto *socket = server.nextPendingConnection()) {
                    if (findChildren<QTcpSocket *>().size() >= 8) {
                        socket->disconnectFromHost();
                        socket->deleteLater();
                        continue;
                    }
                    socket->setReadBufferSize(16385);
                    socket->setParent(this);
                    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                    auto header  = std::make_shared<QByteArray>();
                    auto handled = std::make_shared<bool>(false);
                    connect(socket, &QTcpSocket::readyRead, this, [this, socket, header, handled] {
                        if (*handled)
                            return;
                        *header += socket->readAll();
                        if (header->size() > 16384) {
                            *handled = true;
                            socket->disconnectFromHost();
                            return;
                        }
                        if (!header->contains("\r\n\r\n"))
                            return;
                        *handled = true;
                        serve(socket, *header);
                    });
                    QTimer::singleShot(30000, socket, [socket, handled] {
                        if (!*handled)
                            socket->disconnectFromHost();
                    });
                }
            });
        }
        QList<Resolver>             resolvers;
        QHash<QByteArray, Resource> resources;
        QTcpServer                  server;

        void serve(QTcpSocket *socket, const QByteArray &header)
        {
            auto fail = [socket](int code) {
                socket->write("HTTP/1.1 " + QByteArray::number(code)
                              + " Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();
            };
            const auto lines   = header.split('\n');
            const auto request = lines.value(0).trimmed().split(' ');
            if (request.size() != 3 || (request[0] != "GET" && request[0] != "HEAD")) {
                fail(400);
                return;
            }
            const auto resource = resources.value(request[1]);
            if (!resource.owner || !resource.reference.isValid() || resource.reference.size < 0) {
                fail(404);
                return;
            }
            const qint64 total = resource.reference.size;
            qint64       start = 0, end = total - 1;
            bool         ranged = false;
            for (auto line : lines) {
                line = line.trimmed();
                if (!line.toLower().startsWith("range:"))
                    continue;
                if (ranged) {
                    fail(416);
                    return;
                }
                const auto match = QRegularExpression(QStringLiteral("^bytes=(\\d*)-(\\d*)$"))
                                       .match(QString::fromLatin1(line.mid(6).trimmed()));
                if (!match.hasMatch() || (match.captured(1).isEmpty() && match.captured(2).isEmpty()) || total == 0) {
                    fail(416);
                    return;
                }
                bool ok = false;
                if (match.captured(1).isEmpty()) {
                    const auto suffix = match.captured(2).toLongLong(&ok);
                    if (!ok || suffix <= 0) {
                        fail(416);
                        return;
                    }
                    start = qMax<qint64>(0, total - suffix);
                } else {
                    start = match.captured(1).toLongLong(&ok);
                    if (!ok || start >= total) {
                        fail(416);
                        return;
                    }
                    if (!match.captured(2).isEmpty()) {
                        end = match.captured(2).toLongLong(&ok);
                        if (!ok || end < start) {
                            fail(416);
                            return;
                        }
                        end = qMin(end, total - 1);
                    }
                }
                ranged = true;
            }
            QByteArray response = ranged ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
            response += "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n";
            response += "Content-Length: " + QByteArray::number(end - start + 1) + "\r\n";
            if (ranged)
                response += "Content-Range: bytes " + QByteArray::number(start) + "-" + QByteArray::number(end) + "/"
                    + QByteArray::number(total) + "\r\n";
            response += "\r\n";
            if (request[0] == "HEAD" || total == 0) {
                socket->write(response);
                socket->disconnectFromHost();
                return;
            }
            struct State {
                QPointer<QTcpSocket> socket;
                qint64               pos, end;
                bool                 pending = false;
                QByteArray           header;
                Resource             resource;
            };
            auto state      = std::make_shared<State>();
            state->socket   = socket;
            state->pos      = start;
            state->end      = end;
            state->header   = response;
            state->resource = resource;
            connect(resource.owner, &QObject::destroyed, socket, [socket] { socket->abort(); });
            auto                                       pump     = std::make_shared<std::function<void()>>();
            const std::weak_ptr<std::function<void()>> weakPump = pump;
            *pump                                               = [state, weakPump] {
                const auto pump   = weakPump.lock();
                auto      *socket = state->socket.data();
                if (!pump || !socket || socket->state() != QAbstractSocket::ConnectedState || state->pending
                    || socket->bytesToWrite() > 128 * 1024)
                    return;
                if (!state->resource.owner) {
                    socket->disconnectFromHost();
                    return;
                }
                if (state->pos > state->end) {
                    socket->disconnectFromHost();
                    return;
                }
                state->pending         = true;
                const qint64 requested = qMin<qint64>(64 * 1024, state->end - state->pos + 1);
                state->resource.read(
                    state->resource.reference, state->pos, requested,
                    [state, pump, requested](QByteArray bytes, QString error) {
                        if (!state->socket || state->socket->state() != QAbstractSocket::ConnectedState)
                            return;
                        state->pending = false;
                        if (!error.isEmpty() || bytes.isEmpty() || bytes.size() > requested) {
                            if (!state->header.isEmpty())
                                state->socket->write(
                                    "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                            state->socket->disconnectFromHost();
                            return;
                        }
                        if (!state->header.isEmpty()) {
                            state->socket->write(state->header);
                            state->header.clear();
                        }
                        state->socket->write(bytes);
                        state->pos += bytes.size();
                        QTimer::singleShot(0, state->socket, [pump] { (*pump)(); });
                    });
            };
            connect(socket, &QTcpSocket::bytesWritten, socket, [pump](qint64) { (*pump)(); });
            (*pump)();
        }
    };
    Server *instance(bool create = true)
    {
        static QPointer<Server> server;
        if (!server && create && QCoreApplication::instance())
            server = new Server;
        return server;
    }
}

void MediaRangeService::registerResolver(QObject *owner, Accepts accepts, Reader reader)
{
    auto *application = QCoreApplication::instance();
    if (!owner || !application)
        return;
    if (QThread::currentThread() != application->thread()) {
        QMetaObject::invokeMethod(
            application,
            [guard = QPointer<QObject>(owner), accepts = std::move(accepts), reader = std::move(reader)]() mutable {
                if (guard)
                    registerResolver(guard, std::move(accepts), std::move(reader));
            },
            Qt::QueuedConnection);
        return;
    }
    auto *server = instance();
    for (qsizetype i = server->resolvers.size(); i-- > 0;) {
        if (server->resolvers[i].owner == owner)
            server->resolvers.removeAt(i);
    }
    Reader dispatched
        = [guard = QPointer<QObject>(owner), target = QPointer<Server>(server),
           reader = std::move(reader)](MediaReference reference, qint64 offset, qint64 length, Completion done) {
              if (!guard) {
                  done({}, QStringLiteral("Media provider is unavailable"));
                  return;
              }
              QMetaObject::invokeMethod(
                  guard,
                  [target, reader, reference = std::move(reference), offset, length, done = std::move(done)]() mutable {
                      reader(std::move(reference), offset, length,
                             [target, done = std::move(done)](QByteArray bytes, QString error) mutable {
                                 if (!target)
                                     return;
                                 QMetaObject::invokeMethod(
                                     target,
                                     [done = std::move(done), bytes = std::move(bytes),
                                      error = std::move(error)]() mutable { done(std::move(bytes), std::move(error)); },
                                     Qt::QueuedConnection);
                             });
                  },
                  Qt::QueuedConnection);
          };
    server->resolvers.append({ owner, std::move(accepts), std::move(dispatched) });
    QObject::connect(owner, &QObject::destroyed, server, [server] {
        for (auto it = server->resources.begin(); it != server->resources.end();) {
            if (!it->owner)
                it = server->resources.erase(it);
            else
                ++it;
        }
        for (qsizetype i = server->resolvers.size(); i-- > 0;) {
            if (!server->resolvers[i].owner)
                server->resolvers.removeAt(i);
        }
    });
}
void MediaRangeService::releaseUrl(const QUrl &url)
{
    if (auto *server = instance(false))
        server->resources.remove(url.path().toLatin1());
}
QUrl MediaRangeService::urlFor(const MediaReference &reference)
{
    auto *server = instance();
    for (const auto &resolver : server->resolvers) {
        if (!resolver.owner || !resolver.accepts(reference))
            continue;
        if (!server->server.isListening() && !server->server.listen(QHostAddress::LocalHost, 0))
            return {};
        const QByteArray path = "/" + QUuid::createUuid().toString(QUuid::WithoutBraces).toLatin1();
        server->resources.insert(path, { resolver.owner, reference, resolver.read });
        return QUrl(
            QStringLiteral("http://127.0.0.1:%1%2").arg(server->server.serverPort()).arg(QString::fromLatin1(path)));
    }
    return {};
}
}
