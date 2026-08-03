#ifndef XMPPPLUGIN_H
#define XMPPPLUGIN_H

#include "bundledplugininterface.h"
#include "notestorage.h"
#include "anykeepplugininterface.h"

#include <QObject>

namespace AnyKeep {

class PluginHostInterface;

/** @brief Plugin entry point registering the XMPP PubSub NoteStorage backend. */
class XmppPlugin final : public QObject,
                         public PluginInterface,
                         public RegularPluginInterface,
                         public BundledPluginInterface {
    Q_OBJECT
#ifndef ANYKEEP_BUNDLED_PLUGIN_BUILD
#include "xmpppubsub_plugin_metadata.inc"
#endif
    Q_INTERFACES(AnyKeep::PluginInterface AnyKeep::RegularPluginInterface AnyKeep::BundledPluginInterface)

public:
    explicit XmppPlugin(QObject *parent = nullptr);
    ~XmppPlugin() override;
    void setHost(PluginHostInterface *host) override;
    bool initialize() override;
    void shutdown() override;

private:
    PluginHostInterface *host_ { nullptr };
    NoteStorage::Ptr     storage_;
};

} // namespace AnyKeep

#endif // XMPPPLUGIN_H
