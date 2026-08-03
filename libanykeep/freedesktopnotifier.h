/*
    SPDX-License-Identifier: GPL-3.0-only
*/

#ifndef FREEDESKTOPNOTIFIER_H
#define FREEDESKTOPNOTIFIER_H

#include "anykeep_export.h"

#include <QString>

namespace AnyKeep {

class ANYKEEP_EXPORT FreedesktopNotifier {
public:
    static bool notifyError(const QString &summary, const QString &body);
};

} // namespace AnyKeep

#endif // FREEDESKTOPNOTIFIER_H
