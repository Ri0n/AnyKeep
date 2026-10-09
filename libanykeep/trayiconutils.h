#ifndef TRAYICONUTILS_H
#define TRAYICONUTILS_H

#include <QString>

#include "anykeep_export.h"

class QIcon;
class QAction;
class QSystemTrayIcon;

namespace AnyKeep {

class ANYKEEP_EXPORT TrayIconUtils {
public:
    static QString themedTrayIconName();
    static QIcon   themedTrayIcon();
    // Empty when there are no remote/network storages.
    static QString connectionSummary();
    static void    setupSystemTrayIcon(QSystemTrayIcon *trayIcon, QAction *statusAction = nullptr);
};

} // namespace AnyKeep

#endif // TRAYICONUTILS_H
