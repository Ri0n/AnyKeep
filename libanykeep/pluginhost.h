#ifndef ANYKEEP_PLUGINHOST_H
#define ANYKEEP_PLUGINHOST_H

#include "anykeep_export.h"
#include "pluginhostinterface.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>

namespace AnyKeep {

class EditorPlatformBackend;

class ANYKEEP_EXPORT PluginHost : public QObject, public PluginHostInterface {
    Q_OBJECT
public:
    explicit PluginHost(QObject *parent = nullptr);
    QString      utilsCuttedDots(const QString &str, int n) override;
    NoteManager *noteManager() override;
    QString      anykeepDataDir() override;
    void         rehighlight() override;
    bool         offerSpellCheckProvider(std::shared_ptr<SpellCheckProvider> provider) override;
    void         attachSpellCheck(EditorPlatformBackend *backend);

    QString activeSpellCheckProviderId() const;

signals:
    void rehightlight_requested();
    void spellCheckProviderConflict(const QString &activeName, const QString &ignoredName);

private:
    std::shared_ptr<SpellCheckProvider>    provider_;
    std::shared_ptr<HighlighterExtension>  spellCheckExtension_;
    QSet<QString>                          notifiedSpellCheckConflicts_;
    QList<QPointer<EditorPlatformBackend>> editorBackends_;
};

} // namespace AnyKeep

#endif // ANYKEEP_PLUGINHOST_H
