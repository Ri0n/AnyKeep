#ifndef PORTABLEBACKUPARCHIVE_H
#define PORTABLEBACKUPARCHIVE_H

#include "portablebackup.h"

namespace AnyKeep {

// ZIP is a transport for the ordinary PTF tree, not another note format.
// Worker-only API. Destinations must be new absolute paths; extraction
// destinations must be inside application-private storage.
class ANYKEEP_EXPORT PortableBackupArchive final {
public:
    static PortableBackupInspection create(const QString &source, const QString &destination,
                                           const PortableBackup::Continue &keepGoing = {},
                                           const PortableBackup::Continue &beforePublish = {});
    static PortableBackupInspection extract(const QString &source, const QString &privateDestination,
                                            const PortableBackup::Continue &keepGoing = {},
                                            const PortableBackup::Continue &beforePublish = {});
};

} // namespace AnyKeep
#endif // PORTABLEBACKUPARCHIVE_H
