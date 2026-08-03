#ifndef XMPPERROR_H
#define XMPPERROR_H

#include "xmppdto.h"

struct QXmppError;

namespace AnyKeep {

/**
 * @brief Maps a QXmpp protocol error to AnyKeep's retry/error-state policy.
 * @param error Error returned by a QXmpp asynchronous operation.
 */
XmppErrorKind classifyXmppError(const QXmppError &error);

} // namespace AnyKeep

#endif // XMPPERROR_H
