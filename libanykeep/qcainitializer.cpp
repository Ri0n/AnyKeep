#include "qcainitializer.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QtCrypto>

namespace AnyKeep {

QcaInitializer::QcaInitializer()
{
#ifdef ANYKEEP_QCA_PLUGIN_PATH
    // A bundled QCA is deliberately private to AnyKeep. Preserve an explicit
    // user override, otherwise let QCA discover its OpenSSL provider there.
    if (qEnvironmentVariableIsEmpty("QCA_PLUGIN_PATH"))
        qputenv("QCA_PLUGIN_PATH", QByteArray(ANYKEEP_QCA_PLUGIN_PATH));
#elif defined(Q_OS_MACOS)
    // Release bundles keep the prebuilt QCA providers under the conventional
    // Contents/PlugIns tree. QCA expects QCA_PLUGIN_PATH to name the directory
    // containing its crypto/ subdirectory.
    if (qEnvironmentVariableIsEmpty("QCA_PLUGIN_PATH")) {
        const QDir    executableDir(QCoreApplication::applicationDirPath());
        const QString pluginRoot = QDir::cleanPath(executableDir.filePath(QStringLiteral("../PlugIns/qca3-qt6")));
        if (QDir(pluginRoot + QStringLiteral("/crypto")).exists())
            qputenv("QCA_PLUGIN_PATH", pluginRoot.toUtf8());
    }
#endif
    initializer_ = std::make_unique<QCA::Initializer>();
}

QcaInitializer::~QcaInitializer() = default;

} // namespace AnyKeep
