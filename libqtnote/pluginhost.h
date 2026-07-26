#ifndef QTNOTE_PLUGINHOST_H
#define QTNOTE_PLUGINHOST_H

#include "pluginhostinterface.h"
#include "qtnote_export.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>

namespace QtNote {

class EditorPlatformBackend;

class QTNOTE_EXPORT PluginHost : public QObject, public PluginHostInterface {
    Q_OBJECT
public:
    explicit PluginHost(QObject *parent = nullptr);
    QString      utilsCuttedDots(const QString &str, int n) override;
    NoteManager *noteManager() override;
    QString      qtnoteDataDir() override;
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

} // namespace QtNote

#endif // QTNOTE_PLUGINHOST_H
