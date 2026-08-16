#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDataStream>
#include <QDialog>
#include <QDir>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLibraryInfo>
#include <QLocale>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QPluginLoader>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QTranslator>
#include <QWindow>
#ifdef Q_OS_MAC
#include <ApplicationServices/ApplicationServices.h>
#endif

#include "aboutdlg.h"
#include "actionnotificationinterface.h"
#include "anykeep.h"
#include "anykeep_config.h"
#include "corestorageregistry.h"
#include "defaults.h"
#include "deintegrationinterface.h"
#include "desktopeditorplatformbackend.h"
#include "draftmanager.h"
#include "foldercatalogmanager.h"
#include "globalshortcutsinterface.h"
#include "notedialog.h"
#include "notemanager.h"
#include "noteruleapplicationcontroller.h"
#include "noterulemanager.h"
#include "notesmanagerwindow.h"
#include "notificationinterface.h"
#include "optionsdlg.h"
#include "optionsplugins.h"
#include "pluginmanager.h"
#ifdef ANYKEEP_DBUS_AVAILABLE
#include "anykeepdbus.h"
#endif
#include "shortcutsmanager.h"
#include "stickynotesmanager.h"
#include "trayimpl.h"
#ifdef Q_OS_WIN
#include "updatecontroller.h"
#endif
#include "utils.h"

// #define MAIN_DEBUG
#ifdef MAIN_DEBUG
#include <QDebug>
#endif

void initResources() { Q_INIT_RESOURCE(main); }

namespace AnyKeep {

Q_LOGGING_CATEGORY(logMain, "anykeep.main")

class Main::Private : public QObject {
    Q_OBJECT

public:
    Main                        *q;
    DEIntegrationInterface      *de;
    TrayImpl                    *tray;
    bool                         externalTrayAvailable;
    GlobalShortcutsInterface    *globalShortcuts;
    NotificationInterface       *notifier;
    ActionNotificationInterface *actionNotifier;
    StickyNotesManager          *stickyNotes;
    QSet<QUuid>                  recoveredDraftIds;
    QPointer<NotesManagerWindow> notesManagerWindow;
    QFont                        editorFont;
    QColor                       titleHighlightColor;
    UpdateController            *updates;
#ifdef ANYKEEP_DBUS_AVAILABLE
    AnyKeepDBus *dbus;
#endif

    Private(Main *parent) :
        QObject(parent), q(parent), de(0), tray(0), externalTrayAvailable(false), globalShortcuts(0), notifier(0),
        actionNotifier(0), stickyNotes(new StickyNotesManager(parent)), updates(nullptr)
#ifdef ANYKEEP_DBUS_AVAILABLE
        ,
        dbus(0)
#endif
    {
    }
};

Main::Main(QObject *parent) : QObject(parent), d(new Private(this)), _inited(false)
{
    // loading localization
    QString      langFile     = APPNAME;
    QTranslator *translator   = new QTranslator(qApp);
    QTranslator *qtTranslator = new QTranslator(qApp);
    QStringList  langDirs;
    QStringList  qtLangDirs;
    QString      dlTrDir
        = Utils::anykeepDataDir() + QLatin1String("/translations"); // where translaations could be downloaded

#if defined(ANYKEEP_DEVEL) || defined(Q_OS_UNIX)
    // Qt's translation target puts development .qm files beside the
    // executable, while TRANSLATIONSDIR contains only the source .ts files.
    // Prefer the generated files but retain the source/download locations as
    // fallbacks for Unix development and manually installed translations.
    langDirs << qApp->applicationDirPath() + QLatin1String("/translations") << TRANSLATIONSDIR << dlTrDir;
    qtLangDirs << QLibraryInfo::path(QLibraryInfo::TranslationsPath);
#else
    langDirs << qApp->applicationDirPath() + QLatin1String("/translations") << dlTrDir;
    qtLangDirs << langDirs;
#endif

    QSettings     settings;
    const QString serializedEditorFont = settings.value(QStringLiteral("ui.default-font")).toString();
    if (serializedEditorFont.isEmpty() || !d->editorFont.fromString(serializedEditorFont))
        d->editorFont = qApp->font();
    d->titleHighlightColor
        = settings.value(QStringLiteral("ui.title-color"), Defaults::firstLineHighlightColor()).value<QColor>();
    QString forcedLangName = settings.value(QLatin1String("language")).toString();
    bool    autoLang       = (forcedLangName.isEmpty() || forcedLangName == "auto");

    QLocale locale = autoLang ? QLocale::system() : QLocale(forcedLangName);
    // qDebug() << forcedLangName;
    qDebug() << "Requested locale:" << locale.name() << "language:" << locale.language();
    // Resolve the application catalogue explicitly. Besides making the
    // development output directory unambiguous, this avoids QTranslator's
    // implicit locale fallback selecting an unexpected catalogue variant.
    QStringList   languageCandidates { locale.name() };
    const QString languageOnly = locale.name().section(QLatin1Char('_'), 0, 0);
    if (!languageOnly.isEmpty() && !languageCandidates.contains(languageOnly))
        languageCandidates << languageOnly;
    for (const QString &langDir : langDirs) {
        const QDir directory(langDir);
        for (const QString &language : languageCandidates) {
            const QString translationFile
                = directory.filePath(langFile + QLatin1Char('_') + language + QLatin1String(".qm"));
            if (translator->load(translationFile)) {
                qDebug() << "Translator installed: " << qApp->installTranslator(translator)
                         << ", translator empty: " << translator->isEmpty();
                break;
            }
        }
        if (!translator->isEmpty())
            break;
    }

    foreach (const QString &langDir, qtLangDirs) {
        if (qtTranslator->load(locale, "qt", "_", langDir)) {
            qApp->installTranslator(qtTranslator);
            break;
        }
    }

    initResources();

    auto   *draftManager = DraftManager::instance();
    QString draftStoreError;
    while (!draftManager->initialize(&draftStoreError)) {
        QMessageBox recoveryDialog(QMessageBox::Critical, tr("Draft Recovery Needed"),
                                   tr("AnyKeep could not read its encrypted crash-recovery drafts. Existing drafts "
                                      "have not been deleted.\n\n"
                                      "You can quit and investigate the problem, or start with a new empty draft "
                                      "store. Recreating it keeps the unreadable drafts in a backup folder, but "
                                      "they will not be available in AnyKeep.\n\n%1")
                                       .arg(draftStoreError),
                                   QMessageBox::NoButton);
        auto       *recreateButton = recoveryDialog.addButton(tr("Recreate Draft Store"), QMessageBox::DestructiveRole);
        recoveryDialog.addButton(tr("Quit AnyKeep"), QMessageBox::RejectRole);
        recoveryDialog.exec();
        if (recoveryDialog.clickedButton() != static_cast<QAbstractButton *>(recreateButton))
            return;
        if (draftManager->recreateStore(&draftStoreError))
            break;
    }
    connect(draftManager, &DraftManager::publicationAbandoned, this, &Main::notifyError);
    connect(draftManager, &DraftManager::conflictResolved, this, &Main::notifyError);
    connect(draftManager, &DraftManager::draftPublished, this, [](const QUuid &draftId, const Note &note) {
        // A new note has no stable note id while its window is closing. The compositor
        // therefore saves its final frame under the draft id first. Migrate it after the
        // asynchronous unmanaged/store round trip has had time to complete.
        QTimer::singleShot(750, [draftId, note]() {
            const QString temporary = QString("geometry.draft.%1").arg(draftId.toString(QUuid::WithoutBraces));
            const QString permanent = QString("geometry.%1.%2").arg(note.storageId(), note.id());
            QSettings     settings;
            const QRect   rect = settings.value(temporary).toRect();
            if (rect.isValid())
                settings.setValue(permanent, rect);
            settings.remove(temporary);
        });
    });

    // Folder organisation is useful but must never make the editor
    // unavailable.  Unlike the draft store, a corrupt folder catalog starts
    // as an empty, read-only Unsorted projection until the user explicitly
    // restores a backup or recreates it from the future Folders UI.
    auto   *folderCatalog = FolderCatalogManager::instance();
    QString folderCatalogError;
    if (!folderCatalog->initialize(&folderCatalogError))
        qCWarning(logMain) << "Folder catalog recovery is required:" << folderCatalogError;
    folderCatalog->observeNoteManager(NoteManager::instance());

    // Rules are optional automation. If their encrypted state cannot be
    // opened, keep notes usable and disable routing until explicit recovery.
    auto   *ruleManager = NoteRuleManager::instance();
    QString ruleStoreError;
    if (!ruleManager->initialize(&ruleStoreError))
        qCWarning(logMain) << "Rule store recovery is required:" << ruleStoreError;
    NoteRuleApplicationController::instance()->initialize();

    // Storage registration starts asynchronous initialization. Never touch a
    // storage while restoring drafts until its init job has completed.
    connect(NoteManager::instance(), &NoteManager::storageReady, this, [this](const NoteStorage::Ptr &storage) {
        if (!storage)
            return;
        for (const auto &draft : DraftManager::instance()->recoverableDrafts()) {
            // An unassigned draft is opened through the first ready storage only
            // to obtain an editable Note shell. Its empty origin is preserved in
            // DraftStore and it will still go through routing on publication.
            if ((!draft.storageId.isEmpty() && draft.storageId != storage->systemName())
                || d->recoveredDraftIds.contains(draft.id))
                continue;
            auto note = draft.remoteNoteId.isEmpty() ? storage->createNote() : storage->note(draft.remoteNoteId);
            if (note.isNull())
                continue;
            note.setTitle(draft.title);
            note.setText(draft.body, draft.format);
            note.setFolderId(draft.folderId);
            note.setMedia(draft.media);
            note.setBackendData(draft.backendData);
            auto *dialog = new NoteDialog(note, this, draft.id);
            d->recoveredDraftIds.insert(draft.id);
            dialog->show();
        }
    });

    _pluginManager = new PluginManager(this);
    _pluginManager->loadPlugins();
    QString pluginError;
    // TODO load translations from plugins;
    if (!d->de) {
        pluginError = tr("Desktop integration plugin is not loaded");
    } else if (!d->tray && !d->externalTrayAvailable) {
        pluginError = tr("Tray icon is not initialized");
    } else if (!d->notifier) {
        pluginError = tr("Notifications plugin is not loaded");
    }

    if (!pluginError.isEmpty()) {
        QMessageBox::critical(0, tr("Initialization Error"),
                              pluginError + "\n"
                                  + tr("Enable a plugin with required functionality and restart AnyKeep"));
        QDialog dlg;
        dlg.setLayout(new QHBoxLayout);
        dlg.layout()->addWidget(new OptionsPlugins(this));
        dlg.exec();
        return;
    }

    registerCoreStorages();

    _inited = NoteManager::instance()->loadAll();
    if (!NoteManager::instance()->loadAll()) {
        QMessageBox::critical(0, "AnyKeep",
                              QObject::tr("no one of note "
                                          "storages is accessible. can't continue.."));
        return; // TODO review removing this
    }
    NoteManager::instance()->setPriorities(settings.value("storage.priority", QStringList()).toStringList());

    _shortcutsManager = new ShortcutsManager(d->globalShortcuts, this);

    if (d->tray) {
        connect(d->tray, SIGNAL(exitTriggered()), SLOT(exitAnyKeep()));
        connect(d->tray, SIGNAL(newNoteTriggered()), SLOT(createNewNote()));
        connect(d->tray, SIGNAL(noteManagerTriggered()), SLOT(showNoteManager()));
        connect(d->tray, SIGNAL(optionsTriggered()), SLOT(showOptions()));
        connect(d->tray, SIGNAL(aboutTriggered()), SLOT(showAbout()));
        connect(d->tray, SIGNAL(showNoteTriggered(QString, QString)), SLOT(openNoteDialog(QString, QString)));
    }

#ifdef ANYKEEP_DBUS_AVAILABLE
    d->dbus = new AnyKeepDBus(this, this);
    connect(d->dbus, &AnyKeepDBus::quitRequested, this, &Main::exitAnyKeep);
    connect(d->dbus, &AnyKeepDBus::createNoteRequested, this, &Main::createNewNote);
    connect(d->dbus, &AnyKeepDBus::globalShortcutActivated, _shortcutsManager, &ShortcutsManager::triggerGlobal);
    connect(d->dbus, &AnyKeepDBus::noteManagerRequested, this, &Main::showNoteManager);
    connect(d->dbus, &AnyKeepDBus::optionsRequested, this, &Main::showOptions);
    connect(d->dbus, &AnyKeepDBus::aboutRequested, this, &Main::showAbout);
    connect(d->dbus, &AnyKeepDBus::openNoteRequested, this, &Main::openNoteDialog);
#endif

    // TODO it's a little ugly. refactor
    QAction *actNoteFromSel = new QAction(_shortcutsManager->friendlyName(ShortcutsManager::SKNoteFromSelection), this);
    connect(actNoteFromSel, SIGNAL(triggered(bool)), SLOT(createNewNoteFromSelection()));
    _shortcutsManager->registerGlobal(ShortcutsManager::SKNoteFromSelection, actNoteFromSel);

#ifdef Q_OS_WIN
    // Windows updates use our versioned-installation protocol. Linux packages
    // are owned by their distribution's package manager, so do not start or
    // expose the Windows update controller there.
    d->updates = new UpdateController(this);
    d->updates->confirmStartupProbe(qApp->arguments());
    connect(d->updates, &UpdateController::applyRequested, this, &Main::applyPreparedUpdate);
    const auto notifyPreparedUpdate = [this](const QString &version) {
        notify(tr("AnyKeep update ready"),
               tr("AnyKeep %1 has been downloaded and prepared. Click to update and restart.").arg(version),
               tr("Update"), [this] {
                   if (d->updates)
                       d->updates->applyUpdate();
               });
    };
    connect(d->updates, &UpdateController::updatePrepared, this, notifyPreparedUpdate);
    connect(d->updates, &UpdateController::storeUpdatePrepared, this, [this](const QString &version, bool automatic) {
        const bool downloaded = d->updates && d->updates->storePackageDownloaded();
        const bool canSilent  = d->updates && d->updates->canApplyStoreUpdateSilently();

        // Microsoft explicitly leaves the "good time to restart" policy
        // to the application. If Store auto-update is allowed and no note
        // is being edited, install immediately and let Windows restart us.
        if (automatic && downloaded && canSilent && !hasVisibleNoteEditor()) {
            applyPreparedUpdateInternal(true);
            return;
        }

        QString title;
        QString message;
        QString action;
        if (!downloaded) {
            title   = tr("AnyKeep update available");
            message = tr("AnyKeep %1 is available in Microsoft Store. Click to download it.").arg(version);
            action  = tr("Download");
        } else if (hasVisibleNoteEditor()) {
            title   = tr("AnyKeep update ready");
            message = tr("AnyKeep %1 is ready to install. A note is open, so AnyKeep will wait until you choose to "
                         "restart.")
                          .arg(version);
            action  = tr("Update");
        } else {
            title   = tr("AnyKeep update ready");
            message = tr("AnyKeep %1 is ready to install. Click to update and restart.").arg(version);
            action  = tr("Update");
        }
        notify(title, message, action, [this] {
            if (d->updates)
                d->updates->applyUpdate();
        });
    });
    if (d->updates->state() == UpdateController::Failed && !d->updates->errorString().isEmpty()) {
        notify(tr("AnyKeep update rolled back"), d->updates->errorString(), {}, {});
    }
    if (d->updates->updateReady())
        notifyPreparedUpdate(d->updates->availableVersion());
    d->updates->startAutomaticChecks();
#endif

    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        // Covers SIGTERM/session shutdown paths which bypass Main::exitAnyKeep().
        // Each shell performs its final synchronous checkpoint before closing.
        if (d->notesManagerWindow)
            d->notesManagerWindow->close();
        for (auto *dialog : NoteDialog::openDialogs())
            dialog->close();
    });
}

Main::~Main() {}

void Main::parseAppArguments(const QStringList &args)
{
    int  i           = 0;
    bool argsHandled = false;
    while (i < args.size()) {
        if (args.at(i) == QLatin1String("-n")) {
            if (i < args.size() + 1 && args.at(i + 1)[0] != '-') {
                i++;
                if (args.at(i) == QLatin1String("selection")) {
                    createNewNoteFromSelection();
                }
            } else {
                createNewNote();
            }
            argsHandled = true;
        } else if (args.at(i) == QLatin1String("-m") || args.at(i) == QLatin1String("--note-manager")) {
            showNoteManager();
            argsHandled = true;
        } else if (args.at(i) == QLatin1String("--safe-mode") || args.at(i) == QLatin1String("--safemode")) {
            // Safe mode is consumed before Main is constructed so plugin
            // loading can be restricted.  Treat it as a handled argument here
            // as well to avoid showing first-start UI.
            argsHandled = true;
        }
        i++;
    }
    QSettings s;
    if (!argsHandled && !s.value("first-start").toBool()) {
        QMessageBox *mb = new QMessageBox(
            QMessageBox::Information, tr("First Start"),
            tr("This is your first start of AnyKeep note-taking application.\n\n"
               "To start using just click on pencil in the system tray and choose \"New\" item to create new note.\n"
               "Notes will be automatically saved to special storage, so you should not worry about this."),
            QMessageBox::Ok);
        mb->setModal(false);
        mb->setAttribute(Qt::WA_DeleteOnClose);
        mb->show();
        s.setValue("first-start", true);
    }
}

void Main::exitAnyKeep()
{
    if (d->notesManagerWindow && !d->notesManagerWindow->close())
        return;
    for (auto *dialog : NoteDialog::openDialogs())
        dialog->close();
    for (auto *dialog : NoteDialog::openDialogs()) {
        if (dialog->isVisible())
            return; // A draft checkpoint failed; keep the application alive.
    }

    auto *drafts = DraftManager::instance();
    connect(drafts, &DraftManager::publishingIdle, qApp, &QApplication::quit);
    QTimer::singleShot(5000, qApp, &QApplication::quit);
    drafts->publishPending();
}

bool Main::hasVisibleNoteEditor() const
{
    if (d->notesManagerWindow && d->notesManagerWindow->isVisible() && d->notesManagerWindow->hasOpenNote())
        return true;
    for (auto *dialog : NoteDialog::openDialogs()) {
        if (dialog && dialog->isVisible())
            return true;
    }
    return false;
}

bool Main::checkpointOpenEditorsForUpdate()
{
    if (d->notesManagerWindow && d->notesManagerWindow->hasOpenNote() && !d->notesManagerWindow->checkpoint())
        return false;
    for (auto *dialog : NoteDialog::openDialogs()) {
        if (dialog && !dialog->checkpoint())
            return false;
    }
    return true;
}

void Main::applyPreparedUpdateInternal(bool silentStoreOnly)
{
#ifdef Q_OS_WIN
    if (!d->updates || !d->updates->updateReady())
        return;

    if (d->updates->managedByStore()) {
        if (!d->updates->storePackageDownloaded()) {
            if (!silentStoreOnly)
                d->updates->applyUpdate();
            return;
        }
        if (!checkpointOpenEditorsForUpdate()) {
            notifyError(tr("The Microsoft Store update is ready, but an open note could not be checkpointed."));
            return;
        }

        QString    error;
        const bool silent = silentStoreOnly || d->updates->canApplyStoreUpdateSilently();
        if (!d->updates->installStoreUpdate(silent, &error)) {
            notifyError(error);
            return;
        }
        // Do not quit here. Store package deployment terminates the packaged
        // desktop process at the correct point. RegisterApplicationRestart in
        // StoreUpdateBackend asks Windows to launch the new version afterwards.
        return;
    }

    if (d->notesManagerWindow && !d->notesManagerWindow->close()) {
        notifyError(tr("The update is ready, but the note manager could not checkpoint the current note."));
        return;
    }
    for (auto *dialog : NoteDialog::openDialogs())
        dialog->close();
    for (auto *dialog : NoteDialog::openDialogs()) {
        if (dialog->isVisible()) {
            notifyError(tr("The update is ready, but an open note could not be checkpointed."));
            return;
        }
    }

    QString error;
    if (!d->updates->launchPreparedUpdater(QCoreApplication::applicationPid(), &error)) {
        notifyError(error);
        return;
    }
    QCoreApplication::quit();
#else
    Q_UNUSED(silentStoreOnly)
#endif
}

void Main::applyPreparedUpdate() { applyPreparedUpdateInternal(false); }

void Main::appMessageReceived(const QString &message)
{
    const QStringList arguments = message.split(QLatin1String("!anykeep_argdelim!"));
#ifdef Q_OS_WIN
    if (d->updates)
        d->updates->confirmStartupProbe(arguments);
#endif
    parseAppArguments(arguments);
}

void Main::showAbout()
{
    AboutDlg *d = new AboutDlg;
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->show();
    activateWidget(d);
}

void Main::showNoteManager()
{
    if (!d->notesManagerWindow) {
        d->notesManagerWindow = new NotesManagerWindow(d->updates, this);
        d->notesManagerWindow->platformBackend()->setEditorFont(editorFont());
        d->notesManagerWindow->platformBackend()->setTitleHighlightColor(titleHighlightColor());
        connect(this, &Main::editorFontChanged, d->notesManagerWindow->platformBackend(),
                &EditorPlatformBackend::setEditorFont);
        connect(this, &Main::titleHighlightColorChanged, d->notesManagerWindow->platformBackend(),
                &EditorPlatformBackend::setTitleHighlightColor);
        _pluginManager->attachEditorPlatformBackend(d->notesManagerWindow->platformBackend());
        d->notesManagerWindow->setSpeechRecognitionProvider(_pluginManager->speechRecognitionProvider());
        connect(d->notesManagerWindow, &NotesManagerWindow::openNoteRequested, this, &Main::openNoteDialog);
        connect(d->notesManagerWindow, &NotesManagerWindow::operationFailed, this, &Main::notifyError);
        connect(this, &Main::settingsUpdated, d->notesManagerWindow, [this] {
            d->notesManagerWindow->platformBackend()->reloadVisualSettings();
            d->notesManagerWindow->setSpeechRecognitionProvider(_pluginManager->speechRecognitionProvider());
        });
    }
    if (!d->notesManagerWindow->isReady()) {
        notifyError(tr("The note manager QML window could not be created"));
        return;
    }
    d->notesManagerWindow->show();
}

void Main::showOptions()
{
    OptionsDlg *d = new OptionsDlg(this);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->show();
    connect(d, SIGNAL(accepted()), SIGNAL(settingsUpdated()));
    activateWidget(d);
}

UpdateController *Main::updateController() const { return d->updates; }

QFont Main::editorFont() const { return d->editorFont; }

void Main::setEditorFontPreview(const QFont &font)
{
    if (d->editorFont == font)
        return;
    d->editorFont = font;
    emit editorFontChanged(font);
}

QColor Main::titleHighlightColor() const { return d->titleHighlightColor; }

void Main::setTitleHighlightColorPreview(const QColor &color)
{
    if (d->titleHighlightColor == color)
        return;
    d->titleHighlightColor = color;
    emit titleHighlightColorChanged(color);
}

StickyNotesManager *Main::stickyNotesManager() const { return d->stickyNotes; }

void Main::setStickyNotesImpl(StickyNotesIntegrationInterface *stickyNotes) { d->stickyNotes->setBackend(stickyNotes); }

void Main::pinNote(const Note &note, const QUuid &draftId, bool awaitingPublication, const QRect &preferredGeometry)
{
    d->stickyNotes->requestPin(note, draftId, awaitingPublication, preferredGeometry);
}

void Main::openNoteDialog(const QString &storageId, const QString &noteId)
{
    if (auto *dlg = NoteDialog::findDialog(storageId, noteId)) {
        dlg->show();
        activateWindow(dlg);
        return;
    }
    if (noteId.isEmpty()) {
        if (auto *dlg = makeNoteDialog(storageId)) {
            dlg->show();
            activateWindow(dlg);
        }
        return;
    }

    auto *job = NoteManager::instance()->loadNoteAsync(storageId, noteId, this);
    connect(job, &StorageJob::finished, this, [this, job, storageId, noteId]() {
        if (job->state() != StorageJob::Succeeded) {
            notifyError(job->error().message.isEmpty() ? tr("Failed to load note") : job->error().message);
            job->deleteLater();
            return;
        }
        auto *dlg = NoteDialog::findDialog(storageId, noteId);
        if (!dlg) {
            dlg = new NoteDialog(job->result(), this);
        }
        dlg->show();
        activateWindow(dlg);
        job->deleteLater();
    });
}

NoteDialog *Main::makeNoteDialog(const QString &storageId, const QString &noteId)
{
    auto storage = NoteManager::instance()->storage(storageId);
    if (!storage) {
        qWarning("failed to load storage: %s", qPrintable(storageId));
        return nullptr;
    }

    auto note = noteId.isEmpty() ? storage->createNote() : storage->note(noteId);
    if (note.isNull()) {
        qWarning("failed to load note: %s", qPrintable(noteId));
        return nullptr;
    }

    return new NoteDialog(note, this);
}

void Main::notifyError(const QString &text) { d->notifier->notifyError(text); }

void Main::notify(const QString &title, const QString &message, const QString &actionText, std::function<void()> action)
{
    if (d->actionNotifier)
        d->actionNotifier->notify(title, message, actionText, std::move(action));
    else
        d->notifier->notifyError(message);
}

namespace {
    QWindow *windowForWidget(QWidget *widget)
    {
        if (!widget)
            return nullptr;
        widget->winId();
        return widget->windowHandle();
    }
} // namespace

void Main::activateWidget(QWidget *widget) const { activateWindow(windowForWidget(widget)); }

void Main::activateWindow(QWindow *window) const
{
    if (window)
        d->de->activateWindow(window);
}

WindowGeometryRestoreResult Main::restoreWindowGeometry(QWidget *widget, const QString &key) const
{
    return restoreWindowGeometry(windowForWidget(widget), key);
}

WindowGeometryRestoreResult Main::restoreWindowGeometry(QWindow *window, const QString &key) const
{
    return window ? d->de->restoreWindowGeometry(window, key) : WindowGeometryRestoreResult::Unsupported;
}

bool Main::saveWindowGeometry(QWidget *widget, const QString &key) const
{
    return saveWindowGeometry(windowForWidget(widget), key);
}

bool Main::saveWindowGeometry(QWindow *window, const QString &key) const
{
    return window && d->de->saveWindowGeometry(window, key);
}

bool Main::removeWindowGeometry(const QString &key) const { return d->de->removeWindowGeometry(key); }

QString Main::takePendingWindowGeometryKey() const { return d->de->takePendingWindowGeometryKey(); }

void Main::windowGeometryBridgeReady() const
{
    d->de->windowGeometryBridgeReady();
    for (auto *dialog : NoteDialog::openDialogs())
        dialog->registerWindowGeometry();
}

void Main::setTrayImpl(TrayImpl *tray) { d->tray = tray; }

void Main::setExternalTrayAvailable(bool available) { d->externalTrayAvailable = available; }

void Main::setDesktopImpl(DEIntegrationInterface *de) { d->de = de; }

void Main::setGlobalShortcutsImpl(GlobalShortcutsInterface *gs) { d->globalShortcuts = gs; }

void Main::setNotificationImpl(NotificationInterface *notifier) { d->notifier = notifier; }

void Main::setActionNotificationImpl(ActionNotificationInterface *notifier) { d->actionNotifier = notifier; }

void Main::registerStorage(std::unique_ptr<NoteStorage> storage)
{
    auto *storagePtr = storage.get();
    connect(storagePtr, SIGNAL(noteRemoved(Note)), SLOT(note_removed(Note)));
    connect(storagePtr, SIGNAL(storageErorr(QString)), SLOT(notifyError(QString)));
    NoteManager::instance()->registerStorage(std::move(storage));
}

void Main::unregisterStorage(NoteStorage *storage)
{
    if (storage)
        NoteManager::instance()->unregisterStorage(storage);
}

void Main::createNewNote()
{
    auto dlg = makeNoteDialog(NoteManager::instance()->defaultStorage()->systemName());
    dlg->show();
    activateWindow(dlg);
}

void Main::createNewNoteFromSelection()
{
    QString contents;
#ifdef Q_OS_LINUX
    const QString platformName = QGuiApplication::platformName();
    if (platformName.contains(QLatin1String("wayland"), Qt::CaseInsensitive)) {
        const QString wlPaste = QStandardPaths::findExecutable(QStringLiteral("wl-paste"));
        if (wlPaste.isEmpty()) {
            qCWarning(logMain) << "wl-paste not found, falling back to QClipboard::Selection";
            contents = QApplication::clipboard()->text(QClipboard::Selection);
        } else {
            QProcess proc;
            proc.start(wlPaste, { QStringLiteral("-p") });
            if (!proc.waitForStarted()) {
                qCWarning(logMain) << "failed to start wl-paste:" << proc.errorString();
            } else if (!proc.waitForFinished(3000)) {
                qCWarning(logMain) << "wl-paste timed out";
                proc.kill();
                proc.waitForFinished();
            } else if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
                qCWarning(logMain) << "wl-paste failed" << proc.exitStatus() << proc.exitCode()
                                   << proc.readAllStandardError();
            } else {
                contents = QString::fromUtf8(proc.readAllStandardOutput());
            }
        }
    } else {
        contents = QApplication::clipboard()->text(QClipboard::Selection);
    }
#endif
#ifdef Q_OS_WIN
    int            n = 0;
    QVector<INPUT> input(10);
    memset(input.data(), 0, input.size() * sizeof(INPUT));

    if (GetAsyncKeyState(VK_MENU) & (1 < 15)) {
        input[n].ki.dwFlags = KEYEVENTF_KEYUP;
        input[n].ki.wVk     = VK_MENU;
        input[n++].ki.wScan = MapVirtualKey(VK_MENU, MAPVK_VK_TO_VSC);
    }
    if (GetAsyncKeyState(VK_SHIFT) & (1 < 15)) {
        input[n].ki.dwFlags = KEYEVENTF_KEYUP;
        input[n].ki.wVk     = VK_SHIFT;
        input[n++].ki.wScan = MapVirtualKey(VK_SHIFT, MAPVK_VK_TO_VSC);
    }
    input[n].ki.wVk     = VK_CONTROL;
    input[n].ki.dwFlags = 0;
    input[n++].ki.wScan = MapVirtualKey(VK_CONTROL, MAPVK_VK_TO_VSC);

    input[n].ki.wVk     = 0x43; // Virtual key code for 'c'
    input[n].ki.dwFlags = 0;
    input[n++].ki.wScan = MapVirtualKey(0x56, MAPVK_VK_TO_VSC);

    input[n].ki.dwFlags = KEYEVENTF_KEYUP;
    input[n].ki.wVk     = input[0].ki.wVk;
    input[n++].ki.wScan = input[0].ki.wScan;

    input[n].ki.dwFlags = KEYEVENTF_KEYUP;
    input[n].ki.wVk     = input[1].ki.wVk;
    input[n++].ki.wScan = input[1].ki.wScan;

    bool sent = true;
    for (int i = 0; i < n; i++) {
        input[i].type = INPUT_KEYBOARD;
        if (!SendInput(1, (LPINPUT) & (input[i]), sizeof(INPUT))) {
            sent = false;
            break;
        }
        Sleep(30);
    }
    if (sent) {
        contents = QApplication::clipboard()->text();
    }
#endif
#ifdef Q_OS_MAC
    // copied from
    // http://stackoverflow.com/questions/9758053/programming-with-qt-creator-and-cocoa-copying-the-selected-text-from-the-curre
    CGEventSourceRef source          = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
    CGEventRef       saveCommandDown = CGEventCreateKeyboardEvent(source, (CGKeyCode)8, true);
    CGEventSetFlags(saveCommandDown, kCGEventFlagMaskCommand);
    CGEventRef saveCommandUp = CGEventCreateKeyboardEvent(source, (CGKeyCode)8, false);

    CGEventPost(kCGAnnotatedSessionEventTap, saveCommandDown);
    CGEventPost(kCGAnnotatedSessionEventTap, saveCommandUp);

    CFRelease(saveCommandUp);
    CFRelease(saveCommandDown);
    CFRelease(source);

    contents = QApplication::clipboard()->text();
#endif
    if (contents.isEmpty()) {
        qCWarning(logMain) << "createNewNoteFromSelection: empty selection, nothing to create";
        return;
    }
    if (contents.size()) {
        auto dlg = makeNoteDialog(NoteManager::instance()->defaultStorage()->systemName());
        dlg->setText(contents);
        dlg->show();
        activateWindow(dlg);
    }
}

void Main::note_removed(const Note &note)
{
    NoteDialog *dlg = NoteDialog::findDialog(note.storageId(), note.id());
    if (dlg) {
#ifdef MAIN_DEBUG
        qDebug() << "Main::note_removed";
#endif
        dlg->trashRequested();
    }
}

} // namespace AnyKeep

#include "anykeep.moc"
