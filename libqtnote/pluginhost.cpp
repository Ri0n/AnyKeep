#include "pluginhost.h"
#include "desktopeditorplatformbackend.h"
#include "notehighlighter.h"
#include "notemanager.h"
#include "notewidget.h"
#include "spellcheckprovider.h"
#include "utils.h"

namespace QtNote {

PluginHost::PluginHost(QObject *parent) : QObject(parent) { }

QString PluginHost::utilsCuttedDots(const QString &str, int n) { return Utils::cuttedDots(str, n); }

NoteManager *PluginHost::noteManager() { return NoteManager::instance(); }

QString PluginHost::qtnoteDataDir() { return Utils::qtnoteDataDir(); }

void PluginHost::rehighlight() { emit rehightlight_requested(); }

bool PluginHost::offerSpellCheckProvider(std::shared_ptr<SpellCheckProvider> candidate)
{
    if (!candidate || !candidate->isValid())
        return false;
    if (provider_) {
        if (provider_->id() != candidate->id()) {
            qWarning() << "Spell check provider ignored:" << candidate->id() << "active provider:" << provider_->id();
            candidate->disable(
                SpellCheckProvider::DisableMode::Session,
                tr("Not initialized because %1 is the active spell checker.").arg(provider_->displayName()));
            const QString signature = provider_->id() + QLatin1Char('|') + candidate->id();
            if (!notifiedSpellCheckConflicts_.contains(signature)) {
                notifiedSpellCheckConflicts_.insert(signature);
                emit spellCheckProviderConflict(provider_->displayName(), candidate->displayName());
            }
        }
        return provider_->id() == candidate->id();
    }

    auto extension = makeSpellCheckExtension(candidate);
    if (!extension)
        return false;
    provider_            = std::move(candidate);
    spellCheckExtension_ = std::move(extension);
    qInfo() << "Spell check provider selected:" << provider_->id();
    return true;
}

void PluginHost::attachSpellCheck(NoteWidget *widget)
{
    if (widget && spellCheckExtension_)
        widget->addHighlightExtension(spellCheckExtension_, int(NoteHighlighter::SpellCheck));
}

void PluginHost::attachSpellCheck(DesktopEditorPlatformBackend *backend)
{
    if (backend && spellCheckExtension_)
        backend->addHighlightExtension(spellCheckExtension_, int(NoteHighlighter::SpellCheck));
}

QString PluginHost::activeSpellCheckProviderId() const { return provider_ ? provider_->id() : QString(); }

} // namespace QtNote
