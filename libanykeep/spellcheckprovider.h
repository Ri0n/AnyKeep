#ifndef SPELLCHECKPROVIDER_H
#define SPELLCHECKPROVIDER_H

#include <QString>
#include <QStringList>

#include <memory>

#include "anykeep_export.h"
#include "highlighterext.h"

namespace AnyKeep {

class ANYKEEP_EXPORT SpellCheckProvider {
public:
    enum class DisableMode { Session, Persistent };

    virtual ~SpellCheckProvider() = default;

    virtual QString     id() const                             = 0;
    virtual QString     displayName() const                    = 0;
    virtual bool        isValid() const                        = 0;
    virtual bool        isCorrect(const QString &word) const   = 0;
    virtual QStringList suggestions(const QString &word) const = 0;
    virtual void        addToDictionary(const QString &word)   = 0;

    void disable(DisableMode mode, const QString &reason)
    {
        disabledReason_ = reason;
        onDisabled(mode);
    }
    QString disabledReason() const { return disabledReason_; }

protected:
    virtual void onDisabled(DisableMode mode) = 0;

private:
    QString disabledReason_;
};

ANYKEEP_EXPORT std::shared_ptr<SpellCheckExtension>
               makeSpellCheckExtension(const std::shared_ptr<SpellCheckProvider> &provider);

} // namespace AnyKeep

#endif // SPELLCHECKPROVIDER_H
