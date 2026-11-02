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
    // user override, otherwise let QCA discover its providers there.
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
#ifdef Q_OS_WIN
    // Prebuilt SDKs may also contain qca-ossl. Prefer the native Windows
    // provider independently of filesystem/plugin discovery order.
    QCA::setProviderPriority(QStringLiteral("qca-wincrypto"), 0);
    QCA::setProviderPriority(QStringLiteral("qca-wingss"), 1);
    QCA::setProviderPriority(QStringLiteral("qca-cyrus-sasl"), 2);
    QCA::setProviderPriority(QStringLiteral("qca-ossl"), 100);
#endif
}

QcaInitializer::~QcaInitializer() = default;

} // namespace AnyKeep
