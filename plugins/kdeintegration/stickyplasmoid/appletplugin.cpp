#include <KPluginFactory>
#include <Plasma/Applet>

class AnyKeepStickyPlasmoidPlugin : public Plasma::Applet {
    Q_OBJECT

public:
    AnyKeepStickyPlasmoidPlugin(QObject *parent, const KPluginMetaData &data, const QVariantList &args) :
        Plasma::Applet(parent, data, args)
    {
    }
};

K_PLUGIN_CLASS_WITH_JSON(AnyKeepStickyPlasmoidPlugin, "pluginmetadata.json")

#include "appletplugin.moc"
