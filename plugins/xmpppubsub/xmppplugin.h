#ifndef XMPPPLUGIN_H
#define XMPPPLUGIN_H

#include "bundledplugininterface.h"
#include "notestorage.h"
#include "qtnoteplugininterface.h"

#include <QObject>

namespace QtNote {

class PluginHostInterface;

/** @brief Plugin entry point registering the XMPP PubSub NoteStorage backend. */
class XmppPlugin final : public QObject,
                         public PluginInterface,
                         public RegularPluginInterface,
                         public BundledPluginInterface {
    Q_OBJECT
#ifndef QTNOTE_BUNDLED_PLUGIN_BUILD
    Q_PLUGIN_METADATA(IID "com.rion-soft.QtNote.xmpppubsub")
#endif
    Q_INTERFACES(QtNote::PluginInterface QtNote::RegularPluginInterface QtNote::BundledPluginInterface)

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
    NoteStorage::Ptr     storage_;
};

} // namespace QtNote

#endif // XMPPPLUGIN_H
