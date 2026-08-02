#include <QByteArray>
#include <QtGlobal>

namespace {

class HeadlessTestEnvironment {
public:
    HeadlessTestEnvironment()
    {
        setDefault("QT_QPA_PLATFORM", "offscreen");
        setDefault("QSG_RHI_BACKEND", "software");
    }

private:
    static void setDefault(const char *name, const char *value)
    {
        if (!qEnvironmentVariableIsSet(name))
            qputenv(name, QByteArray(value));
    }
};

// QTEST_MAIN constructs QApplication before the test object exists. A static
// initializer is therefore required for direct IDE/executable launches; CTest
// environment properties alone are too late outside the CTest runner.
const HeadlessTestEnvironment headlessTestEnvironment;

} // namespace
