#include "xmppbackendfactory.h"

#include "xmppbackend.h"

#if defined(ANYKEEP_XMPP_BACKEND_IRIS)
#include "iris/irisxmppbackend.h"
#else
#include "xmppworker.h"
#endif

namespace AnyKeep {

XmppBackend *createXmppBackend(QObject *parent)
{
#if defined(ANYKEEP_XMPP_BACKEND_IRIS)
    return new IrisXmppBackend(parent);
#else
    return new XmppWorker(parent);
#endif
}

} // namespace AnyKeep
