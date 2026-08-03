#ifndef MACOSXTRAY_H
#define MACOSXTRAY_H

#include "trayimpl.h"

class QAction;
class QSystemTrayIcon;
class QTimer;
class QMenu;

namespace AnyKeep {

class Main;
class PluginHostInterface;

class MacOSXTray : public TrayImpl {
    Q_OBJECT
public:
    explicit MacOSXTray(Main *anykeep, PluginHostInterface *host, QObject *parent);
    ~MacOSXTray();

signals:

public slots:

private slots:
    void rebuildMenu();
    void noteSelected();

private:
    friend class MacOSXPlugin;
    Main                *anykeep;
    PluginHostInterface *host;
    QSystemTrayIcon     *sti;
    QAction             *actQuit, *actNew, *actAbout, *actOptions, *actManager;
    QTimer              *menuUpdateTimer;
    QMenu               *contextMenu;
    QMenu               *advancedMenu;
    uint                 menuUpdateHash;
};

}

#endif // MACOSXTRAY_H
