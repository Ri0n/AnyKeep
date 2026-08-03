#ifndef SPELLCHECKPROVIDERINTERFACE_H
#define SPELLCHECKPROVIDERINTERFACE_H

#include <QtPlugin>
#include <memory>

namespace AnyKeep {
class SpellCheckProvider;

class SpellCheckProviderInterface {
public:
    virtual ~SpellCheckProviderInterface()                           = default;
    virtual std::shared_ptr<SpellCheckProvider> spellCheckProvider() = 0;
};
}

Q_DECLARE_INTERFACE(AnyKeep::SpellCheckProviderInterface, "com.rion-soft.AnyKeep.SpellCheckProviderInterface/1.0")

#endif // SPELLCHECKPROVIDERINTERFACE_H
