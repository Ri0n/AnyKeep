#ifndef LOCALDATAKEYSTORE_H
#define LOCALDATAKEYSTORE_H

#include "anykeep_export.h"

#include <QByteArray>
#include <QString>

namespace AnyKeep {

class ANYKEEP_EXPORT LocalDataKeyStore {
public:
    static constexpr int MasterKeySize = 32;

    static QByteArray loadOrCreateMasterKey(QString *error = nullptr);
};

} // namespace AnyKeep

#endif // LOCALDATAKEYSTORE_H
