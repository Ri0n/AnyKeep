#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "localmediaimageprovider.h"
#include "mobileapplication.h"
#include "storageiconimageprovider.h"
#include "themediconimageprovider.h"

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("R-Soft"));
    QCoreApplication::setApplicationName(QStringLiteral("QtNote"));

    QtNote::MobileApplication mobileApplication;
    QQmlApplicationEngine     engine;
    QtNote::installLocalMediaImageProvider(&engine);
    QtNote::installStorageIconImageProvider(&engine);
    QtNote::installThemedIconImageProvider(&engine);
    engine.rootContext()->setContextProperty(QStringLiteral("mobileApp"), &mobileApplication);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/QtNote/Mobile/Main.qml")));

    if (engine.rootObjects().isEmpty())
        return 1;
    return application.exec();
}
