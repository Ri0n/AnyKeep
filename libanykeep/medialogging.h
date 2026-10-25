#ifndef ANYKEEP_MEDIALOGGING_H
#define ANYKEEP_MEDIALOGGING_H

#include "anykeep_export.h"

namespace AnyKeep {

// Install at process startup, before creating the application or worker threads.
// Keeps the first Qt FFmpeg missing-PTS warning and all other diagnostics.
ANYKEEP_EXPORT void installMediaLogFilter();

} // namespace AnyKeep

#endif
