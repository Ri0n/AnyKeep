#ifndef ENGINEINTERFACE_H
#define ENGINEINTERFACE_H

#include <QLocale>

namespace AnyKeep {

class DictionaryDownloader;

class SpellEngineInterface : public QObject {
    Q_OBJECT
public:
    struct DictInfo {
        QLocale::Language language;
        QLocale::Country  country;
        QString           filename;
        bool              isWritable = false; // can be deleted
        bool              isLoaded   = false;

        QLocale toLocale() const { return QLocale(language, country); }
    };

    enum Error {
        NoError,
        NotInstalledError,
        InitError // failed to load/init dict
    };

    virtual QList<DictInfo> supportedLanguages() const = 0;

    virtual Error           addLanguage(const QLocale &locale)    = 0;
    virtual void            removeLanguage(const QLocale &locale) = 0;
    virtual QList<DictInfo> loadedDicts() const                   = 0;

    virtual bool                  canDownload(const QLocale &locale) const = 0; // can we ever try to download?
    virtual DictionaryDownloader *download(const QLocale &locale)          = 0; // from internet?
    virtual void                  removeDictionary(const QLocale &locale)  = 0;

    // true - good word, false - bad word
    virtual bool           spell(const QString &word) const       = 0;
    virtual void           addToDictionary(const QString &word)   = 0;
    virtual QList<QString> suggestions(const QString &word) const = 0;

    virtual QStringList diagnostics() const = 0;

signals:
    void availableDictsUpdated();
};

} // namespace AnyKeep

#endif // ENGINEINTERFACE_H
