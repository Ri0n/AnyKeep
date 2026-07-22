#ifndef XMPPPLUGIN_H
#define XMPPPLUGIN_H

#include "qtnoteplugininterface.h"

#include <QObject>

namespace QtNote {

class PluginHostInterface;

/** @brief Plugin entry point registering the XMPP PubSub NoteStorage backend. */
class XmppPlugin final : public QObject, public PluginInterface, public RegularPluginInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.rion-soft.QtNote.xmpppubsub")
    Q_INTERFACES(QtNote::PluginInterface QtNote::RegularPluginInterface)

public:
    explicit XmppPlugin(QObject *parent = nullptr);
    ~XmppPlugin() override;

    int            metadataVersion() const override;
    void           setHost(PluginHostInterface *host) override;
    PluginMetadata metadata() override;
    bool           initialize() override;
    void           shutdown() override;

private:
    PluginHostInterface *host_ { nullptr };
};

} // namespace QtNote

#endif // XMPPPLUGIN_H
