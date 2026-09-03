#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "editorcursorcontroller.h"
#include "localmediaimageprovider.h"
#include "mobileapplication.h"
#include "pluginiconimageprovider.h"
#include "qcainitializer.h"
#include "storageiconimageprovider.h"
#include "themediconimageprovider.h"

int main(int argc, char *argv[])
{
#ifdef ANYKEEP_ANDROID_FORCE_XMPP_XML_LOG
    // Android cannot conveniently inject an environment variable into an app
    // spawned by Zygote. Keep the existing sanitizer and enable this only for
    // an explicitly configured debug APK.
    qputenv("ANYKEEP_XMPP_XML_LOG", "1");
#endif
    QGuiApplication         application(argc, argv);
    AnyKeep::QcaInitializer qca;
    QCoreApplication::setOrganizationName(QStringLiteral("R-Soft"));
    QCoreApplication::setApplicationName(QStringLiteral("AnyKeep"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("AnyKeep"));

    AnyKeep::MobileApplication mobileApplication;
    QQmlApplicationEngine      engine;
    AnyKeep::installLocalMediaImageProvider(&engine);
    AnyKeep::installPluginIconImageProvider(&engine);
    AnyKeep::installStorageIconImageProvider(&engine);
    AnyKeep::installThemedIconImageProvider(&engine);
    AnyKeep::installEditorCursorController(engine.rootContext());
    engine.rootContext()->setContextProperty(QStringLiteral("mobileApp"), &mobileApplication);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/AnyKeep/Mobile/Main.qml")));

    if (engine.rootObjects().isEmpty())
        return 1;
    return application.exec();
}
