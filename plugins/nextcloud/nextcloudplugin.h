#ifndef NEXTCLOUDPLUGIN_H
#define NEXTCLOUDPLUGIN_H

#include "bundledplugininterface.h"
#include "notestorage.h"
#include "qtnoteplugininterface.h"

#include <QObject>

namespace QtNote {

class PluginHostInterface;

class NextcloudPlugin final : public QObject,
                              public PluginInterface,
                              public RegularPluginInterface,
                              public BundledPluginInterface {
    Q_OBJECT
#ifndef QTNOTE_BUNDLED_PLUGIN_BUILD
#include "nextcloud_plugin_metadata.inc"
#endif
    Q_INTERFACES(QtNote::PluginInterface QtNote::RegularPluginInterface QtNote::BundledPluginInterface)

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

} // namespace QtNote

#endif // NEXTCLOUDPLUGIN_H
