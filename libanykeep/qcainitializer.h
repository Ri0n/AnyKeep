#ifndef QCAINITIALIZER_H
#define QCAINITIALIZER_H

#include "anykeep_export.h"

#include <memory>

namespace QCA {
class Initializer;
}

namespace AnyKeep {

/**
 * Process-level QCA lifetime owned by an application or test executable.
 *
 * Keep an instance alive while AnyKeep code can use QCA. The implementation
 * stays in libanykeep so static QCA builds do not accidentally create a second
 * QCA runtime in the executable.
 */
class ANYKEEP_EXPORT QcaInitializer {
public:
    QcaInitializer();
    ~QcaInitializer();

    QcaInitializer(const QcaInitializer &)            = delete;
    QcaInitializer &operator=(const QcaInitializer &) = delete;
    QcaInitializer(QcaInitializer &&)                 = delete;
    QcaInitializer &operator=(QcaInitializer &&)      = delete;

private:
    std::unique_ptr<QCA::Initializer> initializer_;
};

} // namespace AnyKeep

#endif // QCAINITIALIZER_H
