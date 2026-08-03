#ifndef KDEINTEGRATIONTRAY_H
#define KDEINTEGRATIONTRAY_H

#include "trayimpl.h"

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
class QAction;
class QMenu;
class KStatusNotifierItem;
#endif

namespace AnyKeep {

class Main;

class KDEIntegrationTray : public TrayImpl {
    Q_OBJECT

public:
    explicit KDEIntegrationTray(Main *anykeep, QObject *parent);
    ~KDEIntegrationTray() override;

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
private:
    void ensurePlasmoidInSystemTray();
    void addPlasmoidToPanel();
    void announcePlasmoidPackageInstalled() const;
    bool isPlasmoidRegisteredInSystemTray() const;
    bool isPlasmoidInstalled() const;
#ifdef ANYKEEP_DEVEL
    bool ensureDevelopmentPlasmoidLinks();
    void cleanupDevelopmentPlasmoidLinks();
    void clearDevelopmentPlasmaCache();
    void resetDevelopmentPlasmoidOnPanel();
    void removeDevelopmentPlasmoidFromPanel();
    bool updateDevelopmentPackage(const QString &sourcePath, const QString &packagePath, const QString &qmlSourcePath);
    bool updateDevelopmentFile(const QString &sourcePath, const QString &filePath);
    bool updateDevelopmentMainQml(const QString &sourcePath, const QString &filePath);
    bool updateDevelopmentQmlBackend(const QString &sourcePath, const QString &backendPath);
    bool updateDevelopmentFileContent(const QString &filePath, const QByteArray &content);
    bool updateDevelopmentLink(const QString &sourcePath, const QString &linkPath);
#endif
#else
private slots:
    void showNotes(bool active, const QPoint &pos);

private:
    Main                *anykeep;
    KStatusNotifierItem *sni;
    QAction             *actNew;
    QMenu               *currentMenu = nullptr;
#endif
};

}

#endif // KDEINTEGRATIONTRAY_H
