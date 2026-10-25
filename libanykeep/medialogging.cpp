#include "medialogging.h"

#include <QString>
#include <QtCore/qlogging.h>

#include <atomic>
#include <mutex>

namespace AnyKeep {
namespace {
    QtMessageHandler previousHandler    = nullptr;
    std::atomic_flag missingPtsReported = ATOMIC_FLAG_INIT;

    void mediaMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
    {
        // Qt 6.10's demuxer emits this uncategorized warning for every packet
        // without a PTS, so category/debug rules cannot silence just this noise.
        // Retain one diagnostic; keep all other messages and severities intact.
        if (type == QtWarningMsg
            && message == QLatin1String("QFFmpeg::Demuxer received AVPacket with pts == AV_NOPTS_VALUE")) {
            if (missingPtsReported.test_and_set(std::memory_order_relaxed))
                return;
            previousHandler(type, context, message + QStringLiteral(" (further identical warnings suppressed)"));
            return;
        }
        previousHandler(type, context, message);
    }
} // namespace

void installMediaLogFilter()
{
    static std::once_flag installed;
    std::call_once(installed, [] { previousHandler = qInstallMessageHandler(mediaMessageHandler); });
}

} // namespace AnyKeep
