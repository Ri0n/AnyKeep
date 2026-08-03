#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "editorcursorcontroller.h"
#include "localmediaimageprovider.h"
#include "mobileapplication.h"
#include "pluginiconimageprovider.h"
#include "storageiconimageprovider.h"
#include "themediconimageprovider.h"

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("R-Soft"));
    QCoreApplication::setApplicationName(QStringLiteral("AnyKeep"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("AnyKeep"));

    AnyKeep::MobileApplication mobileApplication;
    QQmlApplicationEngine     engine;
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
