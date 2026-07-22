#include "corestorageregistry.h"

#include "notemanager.h"
#include "ptfstorage.h"

#include <memory>

namespace QtNote {

void registerCoreStorages()
{
    auto *manager = NoteManager::instance();
    if (!manager->storage(QStringLiteral("ptf")))
        manager->registerStorage(std::make_unique<PTFStorage>());
}

} // namespace QtNote
