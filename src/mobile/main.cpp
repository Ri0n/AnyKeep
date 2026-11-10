#include <QGuiApplication>
#include <QLocale>
#include <QTranslator>
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

    // All catalogs live in the APK's Qt resource system. The shared catalog
    // covers C++ and existing desktop strings; the mobile catalog adds the
    // translated mobile QML shell, taking precedence for duplicate entries.
    // Install both before loading Main.qml so qsTr() resolves on first render.
    const QLocale locale = QLocale::system();
    QTranslator sharedTranslator;
    QTranslator mobileTranslator;
    if (sharedTranslator.load(locale, QStringLiteral("anykeep"), QStringLiteral("_"), QStringLiteral(":/i18n")))
        application.installTranslator(&sharedTranslator);
    if (mobileTranslator.load(locale, QStringLiteral("anykeep_mobile"), QStringLiteral("_"),
                              QStringLiteral(":/i18n")))
        application.installTranslator(&mobileTranslator);

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
