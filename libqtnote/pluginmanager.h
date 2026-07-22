#ifndef PLUGINMANAGER_H
#define PLUGINMANAGER_H

#include <QDateTime>
#include <QLibrary>
#include <QList>
#include <QObject>
#include <QSharedPointer>

#include "../plugins/qtnoteplugininterface.h"
#include "pluginlistsource.h"
#include "settingsproviderinterface.h"
#include "speechrecognitionprovider.h"

namespace QtNote {

class Main;
class DesktopEditorPlatformBackend;
class PluginHost;

class PluginManager : public PluginListSource {
    Q_OBJECT
public:
    enum PluginFeature {
        FirstFeature    = 0x1,
        RegularPlugin   = FirstFeature,
        DEIntegration   = 0x2,
        TrayIcon        = 0x4,
        GlobalShortcuts = 0x8,
        Notifications   = 010,
        StickyNotes     = 0x10,
        LastFeature     = 0x20
    };
    Q_DECLARE_FLAGS(PluginFeatures, PluginFeature)

    class PluginData {
    public:
        typedef QSharedPointer<PluginData> Ptr;

        PluginData() : instance(0), loadPolicy(LP_Auto), loadPolicyExplicit(false), loadStatus(LS_Undefined) { }
        QObject                      *instance;
        LoadPolicy                    loadPolicy;
        bool                          loadPolicyExplicit;
        LoadStatus                    loadStatus;
        QString                       fileName;
        QDateTime                     modifyTime;
        PluginManager::PluginFeatures features;
        PluginMetadata                metadata;
    };

    explicit PluginManager(Main *parent);
    ~PluginManager() override;

    QStringList pluginIds() const override;
    Entry       pluginEntry(const QString &pluginId) const override;
    bool        setPluginLoadPolicy(const QString &pluginId, LoadPolicy policy) override;
    bool        setPluginOrder(const QStringList &pluginIds) override;

    template <class T> T *castInterface(const PluginData::Ptr &pd) const
    {
        return hasLiveInstance(pd) ? qobject_cast<T *>(pd->instance) : nullptr;
    }

    void                  loadPlugins();
    LoadPolicy            loadPolicy(const QString &pluginId) const { return plugins[pluginId]->loadPolicy; }
    LoadStatus            loadStatus(const QString &pluginId) const { return plugins[pluginId]->loadStatus; }
    bool                  isLoaded(const QString &pluginId) const { return hasLiveInstance(plugins.value(pluginId)); }
    void                  setLoadPolicy(const QString &pluginId, LoadPolicy lp);
    int                   pluginsCount() const { return plugins.size(); }
    QStringList           pluginsIds() const;
    const PluginMetadata &metadata(const QString &pluginId) const { return plugins[pluginId]->metadata; }
    QString               filename(const QString &pluginId) const { return plugins[pluginId]->fileName; }
    QString               tooltip(const QString &pluginId) const;
    bool                  canConfigure(const QString &pluginId) const;
    QUrl                  settingsComponent(const QString &pluginId) const override;
    SettingsController   *createSettingsController(const QString &pluginId, QObject *parent) override;
    QList<SpeechRecognitionProviderInterface *> speechRecognitionProviders() const;
    SpeechRecognitionProviderInterface         *speechRecognitionProvider() const;
    void                                        attachEditorPlatformBackend(DesktopEditorPlatformBackend *backend);

signals:

public slots:

private:
    Main                           *qtnote;
    PluginHost                     *pluginHost = nullptr;
    QHash<QString, PluginData::Ptr> plugins;

    static bool hasLiveInstance(const PluginData::Ptr &plugin);

    LoadStatus loadPlugin(const QString &fileName, PluginData::Ptr &cache,
                          QLibrary::LoadHints loadHints = QLibrary::LoadHints());
    void       updateMetadata();
    bool       ensureLoaded(PluginData::Ptr pd);
    bool       initRegularPlugin(const PluginData::Ptr &pd);
    void       deinitRegularPlugin(const PluginData::Ptr &pd);
    QString    iconsCacheDir() const;
};

}

#endif // PLUGINMANAGER_H
