#include "sonnetspellcheckprovider.h"

#include <QLocale>
#include <QSet>
#include <QSettings>

#ifdef ANYKEEP_SONNET_AVAILABLE
#include <Sonnet/Settings>
#endif

namespace AnyKeep {
namespace {
#ifdef ANYKEEP_SONNET_AVAILABLE
    QString resolveAvailableLanguage(const QString &candidate, const QStringList &available)
    {
        const QString normalized = QString(candidate).replace(QLatin1Char('-'), QLatin1Char('_'));
        for (const auto &language : available) {
            if (QString(language).replace(QLatin1Char('-'), QLatin1Char('_')).compare(normalized, Qt::CaseInsensitive)
                == 0)
                return language;
        }

        const QString prefix = normalized.section(QLatin1Char('_'), 0, 0);
        for (const auto &language : available) {
            const QString current = QString(language).replace(QLatin1Char('-'), QLatin1Char('_'));
            if (current.section(QLatin1Char('_'), 0, 0).compare(prefix, Qt::CaseInsensitive) == 0)
                return language;
        }
        return {};
    }

    QString baseLanguage(const QString &language)
    {
        return QString(language).replace(QLatin1Char('-'), QLatin1Char('_')).section(QLatin1Char('_'), 0, 0).toLower();
    }

    void appendCandidate(QStringList &result, const QString &candidate, const QStringList &available,
                         const QSet<QString> &preferred)
    {
        if (candidate.isEmpty() || result.size() >= 3)
            return;
        auto resolved = resolveAvailableLanguage(candidate, available);
        if (resolved.isEmpty())
            return;
        if (!preferred.isEmpty()) {
            QString preferredVariant;
            for (const auto &item : preferred) {
                if (baseLanguage(item) == baseLanguage(resolved)) {
                    preferredVariant = resolveAvailableLanguage(item, available);
                    break;
                }
            }
            if (preferredVariant.isEmpty())
                return;
            resolved = preferredVariant;
        }
        for (const auto &existing : result) {
            if (baseLanguage(existing) == baseLanguage(resolved))
                return;
        }
        result.append(resolved);
    }
#endif
} // namespace

SonnetSpellCheckProvider::SonnetSpellCheckProvider()
{
#ifdef ANYKEEP_SONNET_AVAILABLE
    Sonnet::Settings settings;
    Sonnet::Speller  probe;
    const auto       available = probe.availableLanguages();
    QSet<QString>    preferred;
    for (const auto &language : settings.preferredLanguages())
        preferred.insert(language);

    appendCandidate(languages_, settings.defaultLanguage(), available, preferred);
    for (const auto &language : QLocale::system().uiLanguages())
        appendCandidate(languages_, language, available, preferred);

    if (QLocale::system().territory() == QLocale::Belarus) {
        appendCandidate(languages_, QStringLiteral("ru"), available, preferred);
        appendCandidate(languages_, QStringLiteral("be"), available, preferred);
        appendCandidate(languages_, QStringLiteral("en"), available, preferred);
    } else if (QLocale::system().territory() == QLocale::Ukraine) {
        appendCandidate(languages_, QStringLiteral("uk"), available, preferred);
        appendCandidate(languages_, QStringLiteral("ru"), available, preferred);
        appendCandidate(languages_, QStringLiteral("en"), available, preferred);
    }

    QStringList remaining(preferred.begin(), preferred.end());
    remaining.sort(Qt::CaseInsensitive);
    for (const auto &language : remaining)
        appendCandidate(languages_, language, available, preferred);

    for (const auto &language : languages_) {
        Sonnet::Speller speller(language);
        if (speller.isValid())
            spellers_.push_back(std::move(speller));
    }
#endif
}

QString SonnetSpellCheckProvider::id() const { return QStringLiteral("sonnet"); }
QString SonnetSpellCheckProvider::displayName() const { return QStringLiteral("Sonnet"); }

bool SonnetSpellCheckProvider::isValid() const
{
#ifdef ANYKEEP_SONNET_AVAILABLE
    return enabled_ && !spellers_.empty();
#else
    return false;
#endif
}

bool SonnetSpellCheckProvider::isCorrect(const QString &word) const
{
#ifdef ANYKEEP_SONNET_AVAILABLE
    if (!enabled_)
        return true;
    for (const auto &speller : spellers_) {
        if (speller.isCorrect(word))
            return true;
    }
#endif
    return false;
}

QStringList SonnetSpellCheckProvider::suggestions(const QString &word) const
{
    QStringList result;
#ifdef ANYKEEP_SONNET_AVAILABLE
    for (const auto &speller : spellers_) {
        for (const auto &suggestion : speller.suggest(word)) {
            if (!result.contains(suggestion))
                result.append(suggestion);
            if (result.size() >= 12)
                return result;
        }
    }
#endif
    return result;
}

void SonnetSpellCheckProvider::addToDictionary(const QString &word)
{
#ifdef ANYKEEP_SONNET_AVAILABLE
    if (!spellers_.empty())
        spellers_.front().addToPersonal(word);
#else
    Q_UNUSED(word)
#endif
}

QStringList SonnetSpellCheckProvider::languages() const { return languages_; }

void SonnetSpellCheckProvider::onDisabled(DisableMode mode)
{
    enabled_ = false;
    if (mode == DisableMode::Persistent) {
        QSettings settings;
        settings.setValue(QStringLiteral("kdeintegration/useSonnet"), false);
    }
}

} // namespace AnyKeep
