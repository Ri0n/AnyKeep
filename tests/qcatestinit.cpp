#include "qcainitializer.h"
#include <QCoreApplication>

namespace {

// Initialize QCA's core before QTEST_*_MAIN, but discover providers only once
// QCoreApplication has established the executable's library paths.
AnyKeep::QcaInitializer qcaInitializer;

void configureQcaProviders() { qcaInitializer.configureProviders(); }
Q_COREAPP_STARTUP_FUNCTION(configureQcaProviders)

} // namespace
