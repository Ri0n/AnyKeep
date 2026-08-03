#ifndef SONNETSPELLCHECKPROVIDER_H
#define SONNETSPELLCHECKPROVIDER_H

#include "spellcheckprovider.h"

#ifdef ANYKEEP_SONNET_AVAILABLE
#include <Sonnet/Speller>

#include <vector>
#endif

namespace AnyKeep {

class SonnetSpellCheckProvider final : public SpellCheckProvider {
public:
    SonnetSpellCheckProvider();

    QString     id() const override;
    QString     displayName() const override;
    bool        isValid() const override;
    bool        isCorrect(const QString &word) const override;
    QStringList suggestions(const QString &word) const override;
    void        addToDictionary(const QString &word) override;

    QStringList languages() const;

private:
    void onDisabled(DisableMode mode) override;

#ifdef ANYKEEP_SONNET_AVAILABLE
    std::vector<Sonnet::Speller> spellers_;
#endif
    QStringList languages_;
    bool        enabled_ = true;
};

} // namespace AnyKeep

#endif // SONNETSPELLCHECKPROVIDER_H
