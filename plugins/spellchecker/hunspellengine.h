#ifndef HUNSPELLENGINE_H
#define HUNSPELLENGINE_H

#include <QSet>
#include <QString>

#include "engineinterface.h"

class Hunspell;
class QTextCodec;
class QStringEncoder;
class QStringDecoder;
class QNetworkAccessManager;

namespace QtNote {

class PluginHostInterface;

class HunspellEngine : public SpellEngineInterface {
    Q_OBJECT
public:
    struct LangItem {
        DictInfo  info;
        Hunspell *hunspell;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        QTextCodec *codec;
#else
        QStringEncoder *encoder = nullptr;
        QStringDecoder *decoder = nullptr;
#endif
        // TODO write destructor + move constructor
    };

    HunspellEngine(PluginHostInterface *host);
    ~HunspellEngine();

    QList<DictInfo> supportedLanguages() const override;
    Error           addLanguage(const QLocale &locale) override;
    void            removeLanguage(const QLocale &locale) override;

    bool                  canDownload(const QLocale &locale) const override;
    DictionaryDownloader *download(const QLocale &locale) override;
    void                  removeDictionary(const QLocale &locale) override;

    bool            spell(const QString &word) const override;
    void            addToDictionary(const QString &word) override;
    QList<QString>  suggestions(const QString &word) const override;
    QList<DictInfo> loadedDicts() const override;

    QStringList diagnostics() const override;

private:
    int findLangItem(const QLocale &locale);

private:
    QList<LangItem>        languages;
    QSet<QString>          runtimeDict;
    QStringList            dictPaths;
    QString                customDictionaryPath_;
    QNetworkAccessManager *qnam = nullptr;
    QString                lastError_;
};

} // namespace QtNote

#endif // HUNSPELLENGINE_H
