/*
QtNote - Simple note-taking application
Copyright (C) 2015 Sergei Ilinykh

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

Contacts:
E-Mail: rion4ik@gmail.com XMPP: rion@jabber.ru
*/

#include <QAbstractListModel>
#include <QDebug>
#include <QLocale>
#include <QRegularExpression>
#include <QSettings>
#include <QtPlugin>
#include <memory>

#include "dictionarydownloader.h"
#include "highlighterext.h"
#include "hunspellengine.h"
#include "qtnote_config.h"
#include "settingscontroller.h"
#include "spellcheckplugin.h"
#include "spellcheckprovider.h"

namespace QtNote {

static const QLatin1String pluginId("spellchecker");

class HunspellProvider final : public SpellCheckProvider {
public:
    explicit HunspellProvider(std::shared_ptr<SpellEngineInterface> engine) : engine_(std::move(engine)) { }

    QString     id() const override { return QStringLiteral("hunspell"); }
    QString     displayName() const override { return QStringLiteral("Hunspell"); }
    bool        isValid() const override { return engine_ && !engine_->loadedDicts().isEmpty(); }
    bool        isCorrect(const QString &word) const override { return enabled_ && engine_->spell(word); }
    QStringList suggestions(const QString &word) const override { return engine_->suggestions(word); }
    void        addToDictionary(const QString &word) override { engine_->addToDictionary(word); }

protected:
    void onDisabled(DisableMode) override { enabled_ = false; }

private:
    std::shared_ptr<SpellEngineInterface> engine_;
    bool                                  enabled_ = true;
};

class SpellcheckDictionaryModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role {
        LocaleCodeRole = Qt::UserRole + 1,
        DisplayNameRole,
        SelectedRole,
        SystemSelectedRole,
        InstalledRole,
        RemovableRole,
        BusyRole,
        ActionTextRole,
        ErrorRole,
    };

    struct Row {
        SpellCheckPlugin::Dict dict;
        bool                   selected { false };
        bool                   busy { false };
        QString                error;
    };

    explicit SpellcheckDictionaryModel(QObject *parent = nullptr) : QAbstractListModel(parent) { }

    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : rows_.size(); }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size())
            return {};
        const auto &row = rows_.at(index.row());
        switch (role) {
        case LocaleCodeRole:
            return row.dict.locale.bcp47Name();
        case DisplayNameRole: {
            const QString language = row.dict.locale.nativeLanguageName();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const QString territory = row.dict.locale.nativeTerritoryName();
#else
            const QString territory = row.dict.locale.nativeCountryName();
#endif
            return territory.isEmpty() ? language : QStringLiteral("%1 (%2)").arg(language, territory);
        }
        case SelectedRole:
            return row.selected;
        case SystemSelectedRole:
            return bool(row.dict.flags & SpellCheckPlugin::DictSystemSelected);
        case InstalledRole:
            return bool(row.dict.flags & SpellCheckPlugin::DictInstalled);
        case RemovableRole:
            return bool(row.dict.flags & SpellCheckPlugin::DictWritable);
        case BusyRole:
            return row.busy;
        case ActionTextRole:
            return row.dict.flags & SpellCheckPlugin::DictInstalled ? tr("Remove") : tr("Download");
        case ErrorRole:
            return row.error;
        default:
            return {};
        }
    }

    QHash<int, QByteArray> roleNames() const override
    {
        return {
            { LocaleCodeRole, "localeCode" },
            { DisplayNameRole, "displayName" },
            { SelectedRole, "selected" },
            { SystemSelectedRole, "systemSelected" },
            { InstalledRole, "installed" },
            { RemovableRole, "removable" },
            { BusyRole, "busy" },
            { ActionTextRole, "actionText" },
            { ErrorRole, "errorString" },
        };
    }

    void setRows(QList<Row> rows)
    {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }

    const Row *row(int index) const { return index >= 0 && index < rows_.size() ? &rows_.at(index) : nullptr; }

    void setSelected(int index, bool selected)
    {
        if (index < 0 || index >= rows_.size() || rows_[index].selected == selected)
            return;
        rows_[index].selected = selected;
        emit dataChanged(this->index(index), this->index(index), { SelectedRole });
    }

    void setBusy(int index, bool busy, const QString &error = {})
    {
        if (index < 0 || index >= rows_.size())
            return;
        rows_[index].busy  = busy;
        rows_[index].error = error;
        emit dataChanged(this->index(index), this->index(index), { BusyRole, ErrorRole, ActionTextRole });
    }

    QList<QLocale> selectedLocales() const
    {
        QList<QLocale> result;
        for (const auto &row : rows_) {
            if (row.selected)
                result.append(row.dict.locale);
        }
        return result;
    }

private:
    QList<Row> rows_;
};

class SpellcheckSettingsController final : public SettingsController {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *dictionariesModel READ dictionariesModel CONSTANT)
    Q_PROPERTY(bool usingSystemLanguages READ usingSystemLanguages NOTIFY usingSystemLanguagesChanged)
public:
    explicit SpellcheckSettingsController(SpellCheckPlugin *plugin, QObject *parent = nullptr) :
        SettingsController(parent), plugin_(plugin), model_(new SpellcheckDictionaryModel(this))
    {
        originalUseSystem_ = plugin_->userLanguagePreferences().isEmpty();
        useSystem_         = originalUseSystem_;
        reloadRows();
        originalSelected_ = selectedCodes();
    }

    QAbstractItemModel *dictionariesModel() const { return model_; }
    bool                usingSystemLanguages() const { return useSystem_; }

    Q_INVOKABLE void setDictionarySelected(int row, bool selected)
    {
        model_->setSelected(row, selected);
        if (useSystem_) {
            useSystem_ = false;
            emit usingSystemLanguagesChanged();
        }
        updateCustomDirty();
    }

    Q_INVOKABLE void useSystemLanguages()
    {
        if (!useSystem_) {
            useSystem_ = true;
            emit usingSystemLanguagesChanged();
        }
        applySystemSelection();
        updateCustomDirty();
    }

    Q_INVOKABLE void toggleDictionary(int row)
    {
        const auto *entry = model_->row(row);
        if (!entry || entry->busy)
            return;
        const auto locale = entry->dict.locale;
        if (entry->dict.flags & SpellCheckPlugin::DictInstalled) {
            if (!(entry->dict.flags & SpellCheckPlugin::DictWritable))
                return;
            plugin_->removeDictionary(locale);
            reloadRows();
            updateCustomDirty();
            return;
        }

        auto *downloader = plugin_->download(locale);
        if (!downloader) {
            setErrorString(tr("The dictionary cannot be downloaded."));
            return;
        }
        model_->setBusy(row, true);
        connect(downloader, &DictionaryDownloader::finished, this, [this, downloader, locale]() {
            if (downloader->hasErrors())
                setErrorString(downloader->lastError());
            else
                setErrorString({});
            reloadRows();
            updateCustomDirty();
        });
    }

signals:
    void usingSystemLanguagesChanged();

protected:
    bool applyValues(const QVariantMap &, QString *) override
    {
        plugin_->applyLanguagePreferences(model_->selectedLocales(), useSystem_);
        originalUseSystem_ = useSystem_;
        originalSelected_  = selectedCodes();
        return true;
    }

    void afterReset() override
    {
        useSystem_ = originalUseSystem_;
        emit usingSystemLanguagesChanged();
        reloadRows();
        for (int row = 0; row < model_->rowCount(); ++row) {
            const auto *entry = model_->row(row);
            model_->setSelected(row, entry && originalSelected_.contains(entry->dict.locale.bcp47Name()));
        }
        setDirty(false);
    }

private:
    QStringList selectedCodes() const
    {
        QStringList result;
        for (const auto &locale : model_->selectedLocales())
            result.append(locale.bcp47Name());
        result.sort();
        return result;
    }

    void applySystemSelection()
    {
        for (int row = 0; row < model_->rowCount(); ++row) {
            const auto *entry = model_->row(row);
            model_->setSelected(row, entry && bool(entry->dict.flags & SpellCheckPlugin::DictSystemSelected));
        }
    }

    void reloadRows()
    {
        const QStringList                     selectedBefore = selectedCodes();
        QList<SpellcheckDictionaryModel::Row> rows;
        for (const auto &dict : plugin_->dictionaries()) {
            const bool selected = useSystem_
                ? bool(dict.flags & SpellCheckPlugin::DictSystemSelected)
                : (selectedBefore.isEmpty() ? bool(dict.flags & SpellCheckPlugin::DictUserSelected)
                                            : selectedBefore.contains(dict.locale.bcp47Name()));
            rows.append({ dict, selected, false, {} });
        }
        model_->setRows(std::move(rows));
    }

    void updateCustomDirty() { setDirty(useSystem_ != originalUseSystem_ || selectedCodes() != originalSelected_); }

    SpellCheckPlugin          *plugin_;
    SpellcheckDictionaryModel *model_;
    bool                       useSystem_ { true };
    bool                       originalUseSystem_ { true };
    QStringList                originalSelected_;
};

//------------------------------------------------------------
// SpellCheckPlugin
//------------------------------------------------------------
SpellCheckPlugin::SpellCheckPlugin(QObject *parent) : QObject(parent), host(nullptr), sei(nullptr) { }

SpellCheckPlugin::~SpellCheckPlugin() { shutdown(); }

int SpellCheckPlugin::metadataVersion() const { return MetadataVersion; }

PluginMetadata SpellCheckPlugin::metadata()
{
    PluginMetadata md;
    md.id          = pluginId;
    md.icon        = QIcon(":/icons/spellcheck-logo");
    md.name        = "Spell check";
    md.description = tr("Realtime spell check.");
    md.author      = "Sergei Ilinykh <rion4ik@gmail.com>";
    md.version     = 0x020000; // plugin's version 0xXXYYZZPP
    md.minVersion  = 0x020300; // minimum compatible version of QtNote
    md.maxVersion  = QTNOTE_VERSION;
    md.extra.insert(QStringLiteral("configurable"), true); // maximum compatible version of QtNote
    md.homepage = QUrl("http://ri0n.github.io/QtNote");
    return md;
}

void SpellCheckPlugin::setHost(PluginHostInterface *host) { this->host = host; }

bool SpellCheckPlugin::initialize()
{
    shutdown();
    if (!host) {
        initializationFailure_ = tr("Plugin host is unavailable");
        return false;
    }
    initializationFailure_.clear();
    engineOwner_ = std::make_shared<HunspellEngine>(host);
    sei          = engineOwner_.get();
    auto langs   = userLanguagePreferences();
    if (langs.empty()) {
        langs = systemLanguagePreferences();
    }
    QSettings s;
    s.beginGroup(QLatin1String("plugins"));
    s.beginGroup(pluginId);
    auto failedDownloads = s.value("failed-dl").toStringList();

    foreach (auto &l, langs) {
        if (sei->addLanguage(l) == SpellEngineInterface::NotInstalledError && sei->canDownload(l)
            && !failedDownloads.contains(l.name())) {
            auto downloader = download(l);
            connect(downloader, &DictionaryDownloader::finished, this, [this, l, downloader]() {
                if (!downloader->hasErrors()) {
                    sei->addLanguage(l);
                }
            });
        }
    }
    qInfo() << "Spell checker initialized with" << sei->loadedDicts().size() << "dictionary/dictionaries";
    auto       provider = std::make_shared<HunspellProvider>(engineOwner_);
    const auto accepted = host->offerSpellCheckProvider(provider);
    qInfo() << "Hunspell provider" << (accepted ? "accepted" : "not selected");
    if (!accepted) {
        const auto failure = provider->disabledReason();
        shutdown();
        initializationFailure_ = failure;
        qInfo() << "Hunspell dictionaries unloaded after provider rejection";
        return false;
    }
    connect(sei, &HunspellEngine::availableDictsUpdated, this, &SpellCheckPlugin::availableDictsUpdated);
    return true;
}

void SpellCheckPlugin::shutdown()
{
    if (sei) {
        disconnect(sei, nullptr, this, nullptr);
        sei = nullptr;
    }
    engineOwner_.reset();
    if (host) {
        host->rehighlight();
    }
}

QString SpellCheckPlugin::tooltip() const
{
    if (!sei) {
        return initializationFailure_.isEmpty() ? tr("Spell checker is disabled.") : initializationFailure_;
    }
    QList<SpellEngineInterface::DictInfo> dicts = sei->loadedDicts();
    QStringList                           dictsHtml;
    foreach (auto &d, dicts) {
        QLocale locale(d.language, d.country);
        QString l = QLatin1String("<i>") + locale.nativeLanguageName() + QLatin1String(" (")
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            + locale.nativeTerritoryName() + QLatin1Char(')');
#else
            + locale.nativeCountryName() + QLatin1Char(')');
#endif
        if (!d.filename.isEmpty()) {
            l += QLatin1String(":</i> ");
            l += d.filename;
        } else {
            l += QLatin1String("</i>");
        }
        dictsHtml.append(l);
    }

    auto ret = tr("<b>Loaded dictionaries:</b> ") + (dictsHtml.isEmpty() ? tr("none") : dictsHtml.join("<br/>  "));
    if (dicts.empty() && sei->supportedLanguages().isEmpty()) {
        ret += QString("<br/><b>%1:</b><br/>").arg(tr("Diagnostics"));
        ret += sei->diagnostics().join(QLatin1String("<br/>"));
    }
    return ret;
}

QUrl SpellCheckPlugin::settingsComponent() const { return QUrl(QStringLiteral("qrc:/qml/SpellcheckSettings.qml")); }

SettingsController *SpellCheckPlugin::createSettingsController(QObject *parent)
{
    return new SpellcheckSettingsController(this, parent);
}
QList<QLocale> SpellCheckPlugin::systemLanguagePreferences() const
{
    QLocale     systemLocale = QLocale::system();
    QStringList uiLangs      = systemLocale.uiLanguages();

    QList<QLocale> ret;
    ret.reserve(uiLangs.size() + 1);
    foreach (const QString &uiLang, uiLangs) {
        auto locale = QLocale(uiLang);
        if (!ret.contains(locale)) {
            ret.append(locale);
        }
    }

    QLocale enLocale = QLocale(QLocale::English, QLocale::UnitedStates);
    if (!ret.contains(enLocale)) {
        ret.append(enLocale);
    }
    return ret;
}

void SpellCheckPlugin::removeDictionary(const QLocale &locale)
{
    if (sei) {
        sei->removeDictionary(locale);
    }
}

QList<SpellCheckPlugin::Dict> SpellCheckPlugin::dictionaries() const
{
    if (!sei) {
        return {};
    }
    auto systemPrefs = systemLanguagePreferences();
    auto userPrefs   = userLanguagePreferences();
    auto downloaded  = sei->supportedLanguages();

    QList<SpellCheckPlugin::Dict> ret;
    for (auto const &d : std::as_const(downloaded)) {
        DictFlags flags = DictInstalled;
        flags |= (d.isWritable ? DictWritable : DictNone);
        flags |= (d.isLoaded ? DictActivated : DictNone);
        ret.append({ QLocale(d.language, d.country), flags });
    }

    QList<SpellCheckPlugin::Dict> systemPrefDicts;
    for (auto const &locale : std::as_const(systemPrefs)) {
        auto it = std::ranges::find_if(ret, [&locale](auto const &d) { return d.locale == locale; });
        if (it != ret.end()) {
            it->flags |= DictSystemSelected;
            systemPrefDicts.push_back(*it);
            ret.erase(it);
        } else {
            if (sei->canDownload(locale)) {
                DictFlags flags = (DictSystemSelected | DictWritable);
                systemPrefDicts.push_back({ locale, flags });
            }
        }
    }
    ret = systemPrefDicts + ret;

    for (auto &d : ret) {
        d.flags |= (userPrefs.contains(d.locale) ? DictUserSelected : DictNone);
    }
    return ret;
}

DictionaryDownloader *SpellCheckPlugin::download(const QLocale &locale)
{
    if (!sei) {
        return nullptr;
    }
    auto downloader = sei->download(locale);
    connect(downloader, &DictionaryDownloader::finished, this, [locale, downloader]() {
        QSettings s;
        s.beginGroup(QLatin1String("plugins"));
        s.beginGroup(pluginId);
        auto failedDownloads = s.value("failed-dl").toStringList();
        auto name            = locale.name();
        auto wasFailed       = failedDownloads.contains(name);

        if (downloader->hasErrors()) {
            if (!wasFailed) {
                failedDownloads.append(name);
                s.setValue("failed-dl", failedDownloads);
            }
        } else {
            if (wasFailed) {
                failedDownloads.removeOne(name);
                if (failedDownloads.isEmpty()) {
                    s.remove("failed-dl");
                } else {
                    s.setValue("failed-dl", failedDownloads);
                }
            }
        }
    });
    return downloader;
}

QList<QLocale> SpellCheckPlugin::userLanguagePreferences() const
{
    QSettings s;
    s.beginGroup(QLatin1String("plugins"));
    s.beginGroup(pluginId);

    auto           userPrefs = s.value(QLatin1String("langs")).toStringList();
    QList<QLocale> ret;
    ret.reserve(userPrefs.size());
    foreach (const QString &code, userPrefs) {
        QLocale locale(code);
        if (locale != QLocale::c()) {
            ret.append(locale);
        }
    }
    return ret;
}

void SpellCheckPlugin::applyLanguagePreferences(const QList<QLocale> &selected, bool useSystemPreferences)
{
    if (!sei)
        return;

    const auto activeLanguages = useSystemPreferences ? systemLanguagePreferences() : selected;
    QSettings  settings;
    settings.beginGroup(QStringLiteral("plugins"));
    settings.beginGroup(pluginId);

    QStringList storedLanguages;
    for (const auto &locale : activeLanguages)
        sei->addLanguage(locale);
    if (!useSystemPreferences) {
        for (const auto &locale : selected)
            storedLanguages.append(locale.bcp47Name());
    }
    for (const auto &dict : sei->loadedDicts()) {
        const auto locale = dict.toLocale();
        if (!activeLanguages.contains(locale))
            sei->removeLanguage(locale);
    }
    settings.setValue(QStringLiteral("langs"), storedLanguages);
}

} // namespace QtNote

#include "spellcheckplugin.moc"
