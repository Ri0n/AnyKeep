#include "bundledpluginregistry.h"

#include "bundledplugininterface.h"
#include "settingsproviderinterface.h"

#include <QSet>
#include <QSettings>

#include <utility>

namespace QtNote {

BundledPluginRegistry::BundledPluginRegistry(QObject *parent) : PluginListSource(parent)
{
    configuredOrder_ = QSettings().value(QStringLiteral("bundled-plugins-priority")).toStringList();
}

BundledPluginRegistry::~BundledPluginRegistry()
{
    for (auto it = items_.begin(); it != items_.end(); ++it)
        shutdown(it.value());
}

bool BundledPluginRegistry::registerFactory(const Entry &descriptor, Factory factory)
{
    if (descriptor.id.isEmpty() || descriptor.name.isEmpty() || !factory || items_.contains(descriptor.id))
        return false;

    Item item;
    item.entry   = descriptor;
    item.factory = std::move(factory);
    loadSettings(item);
    items_.insert(item.entry.id, item);
    const int configuredPosition = configuredOrder_.indexOf(item.entry.id);
    int       insertionPosition  = order_.size();
    if (configuredPosition >= 0) {
        insertionPosition = 0;
        while (insertionPosition < order_.size()) {
            const int otherPosition = configuredOrder_.indexOf(order_.at(insertionPosition));
            if (otherPosition < 0 || otherPosition > configuredPosition)
                break;
            ++insertionPosition;
        }
    }
    order_.insert(insertionPosition, item.entry.id);
    emit pluginsReset();
    return true;
}

void BundledPluginRegistry::initializeEnabledPlugins()
{
    for (const auto &pluginId : std::as_const(order_)) {
        auto it = items_.find(pluginId);
        if (it == items_.end() || it->entry.loadPolicy == LP_Disabled)
            continue;
        initialize(it.value());
    }
    emit pluginsReset();
}

QStringList BundledPluginRegistry::pluginIds() const { return order_; }

PluginListSource::Entry BundledPluginRegistry::pluginEntry(const QString &pluginId) const
{
    const auto it = items_.constFind(pluginId);
    return it == items_.cend() ? Entry() : it->entry;
}

bool BundledPluginRegistry::setPluginLoadPolicy(const QString &pluginId, LoadPolicy policy)
{
    auto it = items_.find(pluginId);
    if (it == items_.end() || policy < LP_Auto || policy > LP_Disabled)
        return false;
    if (it->entry.loadPolicy == policy)
        return true;

    it->entry.loadPolicy = policy;
    savePolicy(it.value());
    bool success = true;
    if (policy == LP_Disabled)
        shutdown(it.value());
    else
        success = initialize(it.value());
    emit pluginChanged(pluginId);
    return success;
}

bool BundledPluginRegistry::setPluginOrder(const QStringList &pluginIds)
{
    if (pluginIds.size() != items_.size()
        || QSet<QString>(pluginIds.cbegin(), pluginIds.cend()).size() != items_.size())
        return false;
    for (const auto &pluginId : pluginIds) {
        if (!items_.contains(pluginId))
            return false;
    }
    order_ = pluginIds;
    QSettings().setValue(QStringLiteral("bundled-plugins-priority"), order_);
    emit pluginsReset();
    return true;
}

QUrl BundledPluginRegistry::settingsComponent(const QString &pluginId) const
{
    const auto it = items_.constFind(pluginId);
    if (it == items_.cend() || !it->instance)
        return {};
    auto *provider = qobject_cast<SettingsProviderInterface *>(it->instance.data());
    return provider ? provider->settingsComponent() : QUrl();
}

SettingsController *BundledPluginRegistry::createSettingsController(const QString &pluginId, QObject *parent)
{
    auto it = items_.find(pluginId);
    if (it == items_.end())
        return nullptr;
    if (!it->instance) {
        QObject *created = it->factory(this);
        if (!created)
            return nullptr;
        if (!created->parent())
            created->setParent(this);
        it->instance           = created;
        it->entry.configurable = qobject_cast<SettingsProviderInterface *>(created) != nullptr;
    }
    auto *provider = qobject_cast<SettingsProviderInterface *>(it->instance.data());
    return provider ? provider->createSettingsController(parent) : nullptr;
}

QObject *BundledPluginRegistry::instance(const QString &pluginId) const
{
    const auto it = items_.constFind(pluginId);
    return it == items_.cend() ? nullptr : it->instance.data();
}

bool BundledPluginRegistry::initialize(Item &item)
{
    if (item.instance && item.entry.loaded)
        return true;
    QObject *created = item.instance.data();
    if (!created)
        created = item.factory(this);
    if (!created) {
        item.entry.loaded     = false;
        item.entry.loadStatus = LS_ErrNotPlugin;
        return false;
    }
    if (!created->parent())
        created->setParent(this);

    auto *plugin = qobject_cast<BundledPluginInterface *>(created);
    if (!plugin || !plugin->initialize()) {
        delete created;
        item.instance.clear();
        item.entry.loaded     = false;
        item.entry.loadStatus = LS_ErrNotPlugin;
        return false;
    }

    item.instance           = created;
    item.entry.loaded       = true;
    item.entry.loadStatus   = LS_Initialized;
    item.entry.configurable = qobject_cast<SettingsProviderInterface *>(created) != nullptr;
    return true;
}

void BundledPluginRegistry::shutdown(Item &item)
{
    if (item.instance) {
        if (auto *plugin = qobject_cast<BundledPluginInterface *>(item.instance.data()))
            plugin->shutdown();
        delete item.instance.data();
    }
    item.instance.clear();
    item.entry.loaded     = false;
    item.entry.loadStatus = LS_Unloaded;
}

void BundledPluginRegistry::loadSettings(Item &item)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("bundled-plugins"));
    settings.beginGroup(item.entry.id);
    const int storedPolicy = settings.value(QStringLiteral("loadPolicy"), int(item.entry.loadPolicy)).toInt();
    if (storedPolicy >= LP_Auto && storedPolicy <= LP_Disabled)
        item.entry.loadPolicy = LoadPolicy(storedPolicy);
}

void BundledPluginRegistry::savePolicy(const Item &item) const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("bundled-plugins"));
    settings.beginGroup(item.entry.id);
    settings.setValue(QStringLiteral("loadPolicy"), int(item.entry.loadPolicy));
}

} // namespace QtNote
