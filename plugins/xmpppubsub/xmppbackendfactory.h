#ifndef ANYKEEP_XMPPBACKENDFACTORY_H
#define ANYKEEP_XMPPBACKENDFACTORY_H

class QObject;

namespace AnyKeep {
class XmppBackend;

/** Creates the XMPP backend selected at CMake configure time. */
XmppBackend *createXmppBackend(QObject *parent = nullptr);

} // namespace AnyKeep

#endif // ANYKEEP_XMPPBACKENDFACTORY_H
