#ifndef PROTECTEDBACKUP_H
#define PROTECTEDBACKUP_H

#include "portablebackup.h"

namespace AnyKeep {

// Optional transport for a normal portable PTF directory. Neither the
// manifest nor any original filename is stored in clear text. Both operations
// run on a worker; callers must supply an application-private plaintext
// staging location (also for the source of protectDirectory).
class ANYKEEP_EXPORT ProtectedBackup final {
public:
    static PortableBackupInspection protectDirectory(
        const QString &privatePlainDirectory, const QString &destination, const QString &password,
        const PortableBackup::Progress &progress = {}, const PortableBackup::Continue &keepGoing = {},
        const PortableBackup::Continue &beforePublish = {});

    // The destination must be a new path inside application-private storage.
    // Authenticate the complete tree before exposing the resulting PTF copy.
    static PortableBackupInspection unlockDirectory(
        const QString &source, const QString &privateDestination, const QString &password,
        const PortableBackup::Progress &progress = {}, const PortableBackup::Continue &keepGoing = {},
        const PortableBackup::Continue &beforePublish = {});
};

} // namespace AnyKeep
#endif // PROTECTEDBACKUP_H
