#include "qcainitializer.h"

namespace {

// Keep test process lifetime identical to the application: QCA is initialized
// before QTEST_*_MAIN creates its Q(Core|Gui)Application.
const AnyKeep::QcaInitializer qcaInitializer;

} // namespace
