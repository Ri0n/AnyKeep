#ifndef BUNDLEDPLUGINREGISTRY_H
#define BUNDLEDPLUGINREGISTRY_H

#include "pluginlistsource.h"

#include <QHash>
#include <QPointer>
#include <functional>

namespace QtNote {

class QTNOTE_EXPORT BundledPluginRegistry final : public PluginListSource {
    Q_OBJECT

public:
    using Factory = std::function<QObject *(QObject *parent)>;

    explicit BundledPluginRegistry(QObject *parent = nullptr);
    ~BundledPluginRegistry() override;

    bool registerFactory(const Entry &entry, Factory factory);
    void initializeEnabledPlugins();

    QStringList         pluginIds() const override;
    Entry               pluginEntry(const QString &pluginId) const override;
    bool                setPluginLoadPolicy(const QString &pluginId, LoadPolicy policy) override;
    bool                setPluginOrder(const QStringList &pluginIds) override;
    QUrl                settingsComponent(const QString &pluginId) const override;
    SettingsController *createSettingsController(const QString &pluginId, QObject *parent) override;

    QObject *instance(const QString &pluginId) const;

private:
    struct Item {
        Entry             entry;
        Factory           factory;
        QPointer<QObject> instance;
    };

    bool initialize(Item &item);
    void shutdown(Item &item);
    void loadSettings(Item &item);
    void savePolicy(const Item &item) const;

    QStringList          order_;
    QStringList          configuredOrder_;
    QHash<QString, Item> items_;
};

} // namespace QtNote

#endif // BUNDLEDPLUGINREGISTRY_H
