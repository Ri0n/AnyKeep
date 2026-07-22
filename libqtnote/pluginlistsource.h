#ifndef PLUGINLISTSOURCE_H
#define PLUGINLISTSOURCE_H

#include "qtnote_export.h"

#include <QIcon>
#include <QObject>
#include <QStringList>
#include <QUrl>

namespace QtNote {

class SettingsController;

class QTNOTE_EXPORT PluginListSource : public QObject {
    Q_OBJECT

public:
    enum LoadStatus {
        LS_Undefined = 0,
        LS_Loaded,
        LS_Initialized,
        LS_Errors       = 100,
        LS_ErrNotPlugin = LS_Errors + 1,
        LS_ErrVersion,
        LS_ErrAbi,
        LS_ErrMetadata,
        LS_Unloaded
    };
    Q_ENUM(LoadStatus)

    enum LoadPolicy { LP_Auto, LP_Enabled, LP_Disabled };
    Q_ENUM(LoadPolicy)

    struct Entry {
        QString    id;
        QString    name;
        QString    description;
        QString    versionText;
        QString    fileName;
        QString    tooltip;
        QString    iconSource;
        QIcon      icon;
        LoadPolicy loadPolicy { LP_Auto };
        LoadStatus loadStatus { LS_Undefined };
        bool       loaded { false };
        bool       configurable { false };
    };

    using QObject::QObject;
    ~PluginListSource() override = default;

    virtual QStringList         pluginIds() const                                                  = 0;
    virtual Entry               pluginEntry(const QString &pluginId) const                         = 0;
    virtual bool                setPluginLoadPolicy(const QString &pluginId, LoadPolicy policy)    = 0;
    virtual bool                setPluginOrder(const QStringList &pluginIds)                       = 0;
    virtual QUrl                settingsComponent(const QString &pluginId) const                   = 0;
    virtual SettingsController *createSettingsController(const QString &pluginId, QObject *parent) = 0;

signals:
    void pluginsReset();
    void pluginChanged(const QString &pluginId);
};

} // namespace QtNote

#endif // PLUGINLISTSOURCE_H
