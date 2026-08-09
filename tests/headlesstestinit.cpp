#include <QByteArray>
#include <QSettings>
#include <QTemporaryDir>
#include <QtGlobal>

namespace {

class HeadlessTestEnvironment {
public:
    HeadlessTestEnvironment()
    {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory_.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsDirectory_.path());

        setDefault("QT_QPA_PLATFORM", "offscreen");
        setDefault("QSG_RHI_BACKEND", "software");
        setDefault("QT_QUICK_CONTROLS_STYLE", "Basic");
#ifdef Q_OS_WIN
        const QByteArray windowsDirectory = qgetenv("WINDIR");
        if (!windowsDirectory.isEmpty())
            setDefault("QT_QPA_FONTDIR", windowsDirectory + QByteArrayLiteral("\\Fonts"));
#endif
    }

private:
    QTemporaryDir settingsDirectory_;

    static void setDefault(const char *name, const char *value)
    {
        if (!qEnvironmentVariableIsSet(name))
            qputenv(name, QByteArray(value));
    }

    static void setDefault(const char *name, const QByteArray &value)
    {
        if (!qEnvironmentVariableIsSet(name))
            qputenv(name, value);
    }
};

// QTEST_MAIN constructs QApplication before the test object exists. A static
// initializer is therefore required for direct IDE/executable launches; CTest
// environment properties alone are too late outside the CTest runner.
const HeadlessTestEnvironment headlessTestEnvironment;

} // namespace
