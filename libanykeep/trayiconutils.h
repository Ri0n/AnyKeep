#ifndef TRAYICONUTILS_H
#define TRAYICONUTILS_H

#include <QString>

#include "anykeep_export.h"

class QIcon;
class QSystemTrayIcon;

namespace AnyKeep {

class ANYKEEP_EXPORT TrayIconUtils {
public:
    static QString themedTrayIconName();
    static QIcon   themedTrayIcon();
    static void    setupSystemTrayIcon(QSystemTrayIcon *trayIcon);
};

} // namespace AnyKeep

#endif // TRAYICONUTILS_H
