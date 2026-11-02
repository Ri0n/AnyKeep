#include "qcainitializer.h"

namespace {

// Most tests use explicit provider paths and keep QCA alive across QTEST_*_MAIN.
// Application-relative provider discovery is checked separately by qcastartup_test,
// which constructs its initializer only after QCoreApplication exists.
const AnyKeep::QcaInitializer qcaInitializer;

} // namespace
