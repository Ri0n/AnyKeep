#ifndef LOCALDATAKEYSTORE_H
#define LOCALDATAKEYSTORE_H

#include "anykeep_export.h"

#include <QByteArray>
#include <QString>

#include <functional>

class QObject;

namespace AnyKeep {

class ANYKEEP_EXPORT LocalDataKeyStore {
public:
    static constexpr int MasterKeySize = 32;

    using AsyncKeyCallback = std::function<void(QByteArray, QString)>;

    static QByteArray loadOrCreateMasterKey(QString *error = nullptr);
    // Android only: after explicit user confirmation, preserve any local
    // ciphertext encrypted with an inaccessible key in a separate backup
    // and replace the unusable key. Plain PTF notes are not affected.
    static bool recoverUnusableAndroidKey(QString *error = nullptr, QString *backupPath = nullptr);
    // Asynchronous, coalesced keychain lookup. The callback is delivered on
    // context's event loop and discarded if the context is destroyed.
    static void loadOrCreateMasterKeyAsync(QObject *context, AsyncKeyCallback callback);
};

} // namespace AnyKeep

#endif // LOCALDATAKEYSTORE_H
