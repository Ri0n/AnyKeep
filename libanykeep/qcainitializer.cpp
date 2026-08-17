#include "qcainitializer.h"

#include <QtCrypto>

namespace AnyKeep {

QcaInitializer::QcaInitializer() : initializer_(std::make_unique<QCA::Initializer>()) {}

QcaInitializer::~QcaInitializer() = default;

} // namespace AnyKeep
