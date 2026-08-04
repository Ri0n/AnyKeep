#ifndef NEXTCLOUDPLUGIN_H
#define NEXTCLOUDPLUGIN_H

#include "anykeepplugininterface.h"
#include "bundledplugininterface.h"
#include "notestorage.h"

#include <QObject>

namespace AnyKeep {

class PluginHostInterface;

class NextcloudPlugin final : public QObject,
                              public PluginInterface,
                              public RegularPluginInterface,
                              public BundledPluginInterface {
    Q_OBJECT
#ifndef ANYKEEP_BUNDLED_PLUGIN_BUILD
#include "nextcloud_plugin_metadata.inc"
#endif
    Q_INTERFACES(AnyKeep::PluginInterface AnyKeep::RegularPluginInterface AnyKeep::BundledPluginInterface)

public:
    explicit NextcloudPlugin(QObject *parent = nullptr);
    ~NextcloudPlugin() override;
    void setHost(PluginHostInterface *host) override;
    bool initialize() override;
    void shutdown() override;

private:
    PluginHostInterface *host_ { nullptr };
    NoteStorage::Ptr     storage_;
};

} // namespace AnyKeep

#endif // NEXTCLOUDPLUGIN_H
