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
#include "corestorageregistry.h"
#include "deintegrationinterface.h"
#include "draftmanager.h"
#include "globalshortcutsinterface.h"
#include "notedialog.h"
#include "notemanager.h"
#include "notesmanagerwindow.h"
#include "notewidget.h"
#include "notificationinterface.h"
#include "optionsdlg.h"
#include "optionsplugins.h"
#include "pluginmanager.h"
#include "qtnote.h"
#include "qtnote_config.h"
#ifdef QTNOTE_DBUS_AVAILABLE
#include "qtnotedbus.h"
#endif
#include "shortcutsmanager.h"
#include "stickynotesmanager.h"
#include "trayimpl.h"
#include "utils.h"

// #define MAIN_DEBUG
#ifdef MAIN_DEBUG
#include <QDebug>
#endif

void initResources() { Q_INIT_RESOURCE(main); }

namespace QtNote {

Q_LOGGING_CATEGORY(logMain, "qtnote.main")

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
#ifdef QTNOTE_DBUS_AVAILABLE
    QtNoteDBus *dbus;
#endif

    Private(Main *parent) :
        QObject(parent), q(parent), de(0), tray(0), externalTrayAvailable(false), globalShortcuts(0), notifier(0),
        actionNotifier(0), stickyNotes(new StickyNotesManager(parent))
#ifdef QTNOTE_DBUS_AVAILABLE
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
        = Utils::qtnoteDataDir() + QLatin1String("/translations"); // where translaations could be downloaded

#if defined(QTNOTE_DEVEL) || defined(Q_OS_UNIX)
    langDirs << TRANSLATIONSDIR << dlTrDir; // in devel mode TRANSLATIONSDIR will refer to source/translations
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    qtLangDirs << QLibraryInfo::location(QLibraryInfo::TranslationsPath);
#else
    qtLangDirs << QLibraryInfo::path(QLibraryInfo::TranslationsPath);
#endif
#else
    langDirs << qApp->applicationDirPath() + QLatin1String("/translations") << dlTrDir;
    qtLangDirs << langDirs;
#endif

    QSettings settings;
    QString   forcedLangName = settings.value(QLatin1String("language")).toString();
    bool      autoLang       = (forcedLangName.isEmpty() || forcedLangName == "auto");

    QLocale locale = autoLang ? QLocale::system() : QLocale(forcedLangName);
    // qDebug() << forcedLangName;
    foreach (const QString &langDir, langDirs) {
        if (translator->load(locale, langFile, "_", langDir)) {
            qApp->installTranslator(translator);
            break;
        }
    }

    foreach (const QString &langDir, qtLangDirs) {
        if (qtTranslator->load(locale, "qt", "_", langDir)) {
            qApp->installTranslator(qtTranslator);
            break;
        }
    }

    initResources();

    QString draftStoreError;
    if (!DraftManager::instance()->initialize(&draftStoreError)) {
        QMessageBox::critical(nullptr, tr("Initialization Error"),
                              tr("The encrypted draft store could not be opened. QtNote will not start because "
                                 "editing without crash-safe storage could lose sensitive data.\n\n%1")
                                  .arg(draftStoreError));
        return;
    }
    connect(DraftManager::instance(), &DraftManager::publicationAbandoned, this, &Main::notifyError);
    connect(DraftManager::instance(), &DraftManager::conflictResolved, this, &Main::notifyError);
    connect(DraftManager::instance(), &DraftManager::draftPublished, this, [](const QUuid &draftId, const Note &note) {
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
            note.setMedia(draft.media);
            note.setBackendData(draft.backendData);
            auto *widget = noteWidget(note, draft.id);
            auto *dialog = new NoteDialog(widget, this);
            dialog->setWindowIcon(storage->noteIcon());
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
                                  + tr("Enable a plugin with required functionality and restart QtNote"));
        QDialog dlg;
        dlg.setLayout(new QHBoxLayout);
        dlg.layout()->addWidget(new OptionsPlugins(this));
        dlg.exec();
        return;
    }

    registerCoreStorages();

    _inited = NoteManager::instance()->loadAll();
    if (!NoteManager::instance()->loadAll()) {
        QMessageBox::critical(0, "QtNote",
                              QObject::tr("no one of note "
                                          "storages is accessible. can't continue.."));
        return; // TODO review removing this
    }
    NoteManager::instance()->setPriorities(settings.value("storage.priority", QStringList()).toStringList());

    _shortcutsManager = new ShortcutsManager(d->globalShortcuts, this);

    if (d->tray) {
        connect(d->tray, SIGNAL(exitTriggered()), SLOT(exitQtNote()));
        connect(d->tray, SIGNAL(newNoteTriggered()), SLOT(createNewNote()));
        connect(d->tray, SIGNAL(noteManagerTriggered()), SLOT(showNoteManager()));
        connect(d->tray, SIGNAL(optionsTriggered()), SLOT(showOptions()));
        connect(d->tray, SIGNAL(aboutTriggered()), SLOT(showAbout()));
        connect(d->tray, SIGNAL(showNoteTriggered(QString, QString)), SLOT(openNoteDialog(QString, QString)));
    }

#ifdef QTNOTE_DBUS_AVAILABLE
    d->dbus = new QtNoteDBus(this, this);
    connect(d->dbus, &QtNoteDBus::quitRequested, this, &Main::exitQtNote);
    connect(d->dbus, &QtNoteDBus::createNoteRequested, this, &Main::createNewNote);
    connect(d->dbus, &QtNoteDBus::globalShortcutActivated, _shortcutsManager, &ShortcutsManager::triggerGlobal);
    connect(d->dbus, &QtNoteDBus::noteManagerRequested, this, &Main::showNoteManager);
    connect(d->dbus, &QtNoteDBus::optionsRequested, this, &Main::showOptions);
    connect(d->dbus, &QtNoteDBus::aboutRequested, this, &Main::showAbout);
    connect(d->dbus, &QtNoteDBus::openNoteRequested, this, &Main::openNoteDialog);
#endif

    // TODO it's a little ugly. refactor
    QAction *actNoteFromSel = new QAction(_shortcutsManager->friendlyName(ShortcutsManager::SKNoteFromSelection), this);
    connect(actNoteFromSel, SIGNAL(triggered(bool)), SLOT(createNewNoteFromSelection()));
    _shortcutsManager->registerGlobal(ShortcutsManager::SKNoteFromSelection, actNoteFromSel);

    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        // Covers SIGTERM/session shutdown paths which bypass Main::exitQtNote().
        // Each shell performs its final synchronous checkpoint before closing.
        if (d->notesManagerWindow)
            d->notesManagerWindow->close();
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (widget->objectName() == QLatin1String("noteDlg"))
                widget->close();
        }
    });
}

Main::~Main() { }

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
        }
        i++;
    }
    QSettings s;
    if (!argsHandled && !s.value("first-start").toBool()) {
        QMessageBox *mb = new QMessageBox(
            QMessageBox::Information, tr("First Start"),
            tr("This is your first start of QtNote note-taking application.\n\n"
               "To start using just click on pencil in the system tray and choose \"New\" item to create new note.\n"
               "Notes will be automatically saved to special storage, so you should not worry about this."),
            QMessageBox::Ok);
        mb->setModal(false);
        mb->setAttribute(Qt::WA_DeleteOnClose);
        mb->show();
        s.setValue("first-start", true);
    }
}

void Main::exitQtNote()
{
    if (d->notesManagerWindow && !d->notesManagerWindow->close())
        return;
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QLatin1String("noteDlg"))
            widget->close();
    }
    for (auto *widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QLatin1String("noteDlg") && widget->isVisible())
            return; // A draft checkpoint failed; keep the application alive.
    }

    auto *drafts = DraftManager::instance();
    connect(drafts, &DraftManager::publishingIdle, qApp, &QApplication::quit);
    QTimer::singleShot(5000, qApp, &QApplication::quit);
    drafts->publishPending();
}

void Main::appMessageReceived(const QString &message)
{
    parseAppArguments(message.split(QLatin1String("!qtnote_argdelim!")));
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
        d->notesManagerWindow = new NotesManagerWindow(this);
        _pluginManager->attachEditorPlatformBackend(d->notesManagerWindow->platformBackend());
        connect(d->notesManagerWindow, &NotesManagerWindow::openNoteRequested, this, &Main::openNoteDialog);
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

NoteWidget *Main::noteWidget(const Note &note, const QUuid &draftId)
{
    NoteWidget *w = new NoteWidget(note, draftId);
    w->setSpeechRecognitionProvider(_pluginManager->speechRecognitionProvider());
    w->setStickyNotesAvailable(d->stickyNotes->isAvailable());

    emit noteWidgetCreated(w);

    connect(this, SIGNAL(settingsUpdated()), w, SLOT(rereadSettings()));
    connect(this, &Main::settingsUpdated, w,
            [this, w]() { w->setSpeechRecognitionProvider(_pluginManager->speechRecognitionProvider()); });
    connect(w, SIGNAL(trashRequested()), SLOT(note_trashRequested()));
    return w;
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
        activateWidget(dlg);
        return;
    }
    if (noteId.isEmpty()) {
        if (auto *dlg = makeNoteDialog(storageId)) {
            dlg->show();
            activateWidget(dlg);
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
            auto  storage = NoteManager::instance()->storage(storageId);
            auto *nw      = noteWidget(job->result());
            dlg           = new NoteDialog(nw, this);
            dlg->setWindowIcon(storage->noteIcon());
            dlg->setWindowState(Qt::WindowNoState);
        }
        dlg->show();
        activateWidget(dlg);
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

    auto nw = noteWidget(note);
    if (!nw) {
        return nullptr;
    }
    auto dlg = new NoteDialog(nw, this);
    dlg->setWindowIcon(storage->noteIcon());
    dlg->setWindowState(Qt::WindowNoState);
    return dlg;
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
    activateWidget(dlg);
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
        dlg->weidget()->setText(contents);
        dlg->show();
        activateWidget(dlg);
    }
}

void Main::note_trashRequested()
{
    NoteWidget *nw      = static_cast<NoteWidget *>(sender());
    auto        storage = NoteManager::instance()->storage(nw->storageId());
    if (!nw->noteId().isEmpty()) {
#ifdef MAIN_DEBUG
        qDebug() << "Main::note_trashRequested";
#endif
        const auto error = DraftManager::instance()->queueRemoval(storage->systemName(), nw->noteId());
        if (error)
            notifyError(tr("Failed to queue note removal: %1").arg(error.message));
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

} // namespace QtNote

#include "qtnote.moc"
