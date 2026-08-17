#include <KPluginFactory>
#include <Plasma/Applet>

#include <QDBusConnection>
#include <QDBusMessage>

namespace {
constexpr auto ServiceName   = "com.github.ri0n.AnyKeep";
constexpr auto ObjectPath    = "/AnyKeep";
constexpr auto InterfaceName = "com.github.ri0n.AnyKeep";
}

class AnyKeepStickyPlasmoidPlugin : public Plasma::Applet {
    Q_OBJECT

public:
    AnyKeepStickyPlasmoidPlugin(QObject *parent, const KPluginMetaData &data, const QVariantList &args) :
        Plasma::Applet(parent, data, args)
    {
        // Plasma::Applet::destroy() marks a permanent applet removal before
        // the instance is deleted.  Use the stable Plasma presentation id to
        // let AnyKeep resolve the sticky-note id; the applet configuration may
        // already be unavailable while the removal is being processed.
        connect(this, &Plasma::Applet::destroyedChanged, this, [this](bool destroyed) {
            if (!destroyed)
                return;

            auto message = QDBusMessage::createMethodCall(QLatin1String(ServiceName), QLatin1String(ObjectPath),
                                                          QLatin1String(InterfaceName),
                                                          QStringLiteral("unpinStickyNoteForPresentation"));
            message.setArguments({ QString::number(id()) });

            // Widget removal is an explicit user action.  Keep D-Bus
            // activation enabled so removing a sticky while AnyKeep is not
            // running still updates the persistent pin state.
            QDBusConnection::sessionBus().send(message);
        });
    }
};

K_PLUGIN_CLASS_WITH_JSON(AnyKeepStickyPlasmoidPlugin, "pluginmetadata.json")

#include "appletplugin.moc"
