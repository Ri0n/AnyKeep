#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDebug>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPrintDialog>
#include <QPrinter>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QTextDocument>
#include <QToolBar>
#include <QToolButton>
#include <QToolTip>
#include <QUuid>

#include "defaults.h"
#include "desktopeditorplatformbackend.h"
#include "desktopnoteeditorhost.h"
#include "iconutils.h"
#include "note.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "notehighlighter.h"
#include "notestorage.h"
#include "notewidget.h"
#include "speechaudiorecorder.h"
#include "speechrecognitionprovider.h"
#include "ui_notewidget.h"
#include "utils.h"

namespace QtNote {

static std::shared_ptr<HighlighterExtension> firstLineHighlighter;
static QColor                                firstLineColor;

namespace {
    QAction *initAction(const QIcon icon, const QString &title, const QString &toolTip, const char *hotkey,
                        QWidget *parent)
    {
        QAction *act = new QAction(icon, title, parent);
        act->setToolTip(toolTip);
        act->setShortcut(QKeySequence(QLatin1String(hotkey)));
        act->setShortcutContext(Qt::WindowShortcut);
        return act;
    }
}

class FirstLineHighlighter : public HighlighterExtension {
public:
    using HighlighterExtension::HighlighterExtension;

    void reset() { }

    void highlight(NoteHighlighter *nh, const QString &text)
    {
        Q_UNUSED(text)
        QTextBlock tb = nh->currentBlock();
        if (tb.position() == 0) {
            QTextCharFormat titleHighlightFormat;
            titleHighlightFormat.setForeground(firstLineColor);
            auto pointSize = tb.charFormat().font().pointSizeF();
            if (pointSize <= 0)
                pointSize = tb.document()->defaultFont().pointSizeF();
            if (pointSize > 0)
                titleHighlightFormat.setFontPointSize(pointSize * 1.5);
            nh->addFormat(0, tb.length(), titleHighlightFormat);
        }
    }
};

NoteWidget::NoteWidget(const Note &note, const QUuid &draftId) : ui(new Ui::NoteWidget)
{
    ui->setupUi(this);

    editor = new NoteEditor(note, draftId, this);

    qmlEditor = new DesktopNoteEditorHost(editor, this);
    ui->noteLayout->insertWidget(1, qmlEditor);

    if (!editor->note().isNull() && editor->note().storage()) {
        auto storageFormats = editor->note().storage()->availableFormats();
        setAcceptRichText(storageFormats.contains(Note::Markdown) || storageFormats.contains(Note::Html));
    }

    setFocusProxy(qmlEditor);
    // ui->saveBtn->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));

    _autosaveTimer.setInterval(10000);
    _lastChangeElapsed.start();
    connect(&_autosaveTimer, SIGNAL(timeout()), SLOT(autosave()));
    // connect(parent, SIGNAL(destroyed()), SLOT(close()));

    QToolBar *tbar = new QToolBar(this);
    ui->toolbarLayout->addWidget(tbar);

    QAction *exportAction = initAction(nullptr, tr("Export"), tr("Export note to file"), "Ctrl+S");
    exportAction->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    connect(exportAction, SIGNAL(triggered()), SLOT(onSaveClicked()));

    speechRecorder  = new SpeechAudioRecorder(this);
    auto speechIcon = QIcon::fromTheme(QLatin1String("audio-input-microphone"));
    if (speechIcon.isNull()) {
        speechIcon = IconUtils::symbolicIcon(QLatin1String(":/svg/microphone"));
    }
    speechIdleIcon = speechIcon;
    speechAction   = QtNote::initAction(speechIcon, tr("Mic"), tr("Hold to dictate text"), "", this);
    speechAction->setVisible(false);
    tbar->addAction(speechAction);
    speechButton = qobject_cast<QToolButton *>(tbar->widgetForAction(speechAction));
    if (speechButton) {
        speechButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        speechButton->setAutoRaise(true);
        connect(speechButton, &QToolButton::pressed, this, &NoteWidget::startSpeechRecognition);
        connect(speechButton, &QToolButton::released, this, &NoteWidget::finishSpeechRecognition);
    }
    connect(speechRecorder, &SpeechAudioRecorder::elapsedChanged, this, &NoteWidget::updateSpeechRecognitionProgress);
    connect(speechRecorder, &SpeechAudioRecorder::maxDurationReached, this, &NoteWidget::finishSpeechRecognition);
    connect(speechRecorder, &SpeechAudioRecorder::failed, this,
            [this](const QString &error) { showSpeechRecognitionError(error); });

    tbar->addSeparator();

    QAction *act = initAction(":/icons/copy", tr("Copy"), tr("Copy note to clipboard"), "Ctrl+Shift+C");
    tbar->addAction(act);
    connect(act, SIGNAL(triggered()), SLOT(onCopyClicked()));

    auto pinIcon = QIcon::fromTheme(QLatin1String("view-pin-symbolic"));
    if (pinIcon.isNull())
        pinIcon = QIcon::fromTheme(QLatin1String("window-pin"));
    if (pinIcon.isNull())
        pinIcon = IconUtils::symbolicIcon(QLatin1String(":/svg/pin"));
    auto *pinButton = new QToolButton(tbar);
    pinButton->setIcon(pinIcon);
    pinButton->setText(tr("Pin"));
    pinButton->setToolTip(tr("Choose how to pin this note"));
    pinButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    pinButton->setPopupMode(QToolButton::InstantPopup);
    auto *pinMenu   = new QMenu(pinButton);
    stickyPinAction = pinMenu->addAction(tr("Pin to desktop"));
    stickyPinAction->setToolTip(tr("Create a desktop note using the active sticky-notes plugin"));
    stickyPinAction->setVisible(false);
    stickyPinAction->setEnabled(!editor->noteId().isEmpty());
    connect(stickyPinAction, &QAction::triggered, this, &NoteWidget::pinRequested);
    alwaysOnTopAction = pinMenu->addAction(tr("Always on top"));
    alwaysOnTopAction->setCheckable(true);
    alwaysOnTopAction->setToolTip(tr("Keep this window above normal windows"));
    connect(alwaysOnTopAction, &QAction::toggled, this, &NoteWidget::alwaysOnTopChanged);
    pinButton->setMenu(pinMenu);
    tbar->addWidget(pinButton);

    act = initAction(":/icons/trash", tr("Delete"), tr("Delete note"), "Ctrl+D");
    tbar->addAction(act);
    connect(act, SIGNAL(triggered()), SLOT(onTrashClicked()));

    auto *toolbarSpacer = new QWidget(tbar);
    toolbarSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tbar->addWidget(toolbarSpacer);

    auto *printAction = initAction(":/icons/print", tr("Print"), tr("Print note"), "Ctrl+P");
    tbar->addAction(printAction);
    connect(printAction, SIGNAL(triggered()), SLOT(onPrintClicked()));
    tbar->addAction(exportAction);

    QSettings s;
    auto      fs = s.value("ui.default-font").toString();
    if (!fs.isEmpty()) {
        QFont defaultFont;
        defaultFont.fromString(fs);
        setFont(defaultFont);
    }

    connect(editor, &NoteEditor::textChanged, this, &NoteWidget::textChanged);
    connect(qmlEditor, &DesktopNoteEditorHost::focusLost, this, &NoteWidget::save);
    connect(qmlEditor, &DesktopNoteEditorHost::focusReceived, this, &NoteWidget::focusReceived, Qt::QueuedConnection);

    updateFirstLineColor();
    if (!firstLineHighlighter)
        firstLineHighlighter = std::make_shared<FirstLineHighlighter>();
    addHighlightExtension(firstLineHighlighter, NoteHighlighter::Title);
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    connect(qGuiApp, &QGuiApplication::paletteChanged, this, [this](const QPalette &) { updateFirstLineColor(); });
#endif
    initFromNote();
}

NoteWidget::~NoteWidget()
{
    _autosaveTimer.stop();
    delete ui;
}

QAction *NoteWidget::initAction(const char *icon, const QString &title, const QString &toolTip, const char *hotkey)
{
    return QtNote::initAction(icon ? QIcon(icon) : QIcon(), title, toolTip, hotkey, this);
}

void NoteWidget::changeEvent(QEvent *e)
{
    switch (e->type()) {
    case QEvent::LanguageChange:
        ui->retranslateUi(this);
        break;
    default:
        break;
    }
}

void NoteWidget::save()
{
    qmlEditor->flushPendingEditorChanges();
    if (!editor->isDirty())
        return;
    if (!editor->save())
        return;
    editor->breakHistoryMerge();
    _lastChangeElapsed.restart();
}

bool NoteWidget::prepareToClose()
{
    qmlEditor->flushPendingEditorChanges();
    return editor->close();
}

void NoteWidget::discardDraft() { editor->discardDraft(); }

void NoteWidget::autosave()
{
    if (!text().isEmpty() && editor->isDirty())
        save();
    else
        _autosaveTimer.stop();
}

void NoteWidget::focusReceived()
{
    if (_trashRequested || editor->isDirty())
        return;
    if (editor->reloadNewerDraft())
        textChanged();
}

void NoteWidget::textChanged()
{
    if (stickyPinAction)
        stickyPinAction->setEnabled(!text().isEmpty());
    if (editor->isDirty() && !_autosaveTimer.isActive()) {
        _autosaveTimer.start();
        _lastChangeElapsed.restart();
    }
    const QString firstLine = text().section(QLatin1Char('\n'), 0, 0).trimmed();
    if (firstLine != _firstLine || firstLine.isEmpty()) {
        _firstLine = firstLine;
        emit firstLineChanged();
    }
}

void NoteWidget::setText(QString text)
{
    editor->loadDocument(text, editor->format(), NoteEditor::LoadPolicy::ResetHistory);
    _autosaveTimer.stop();
    _lastChangeElapsed.restart();
}

void NoteWidget::initFromNote()
{
    const auto current = editor->note();
    if (current.isNull()) {
        setContents(QString(), QString(), Note::PlainText);
        return;
    }
    setContents(current.title(), current.text(), current.format());
}

void NoteWidget::setContents(const QString &title, const QString &body, Note::Format format, ContentLoadPolicy policy)
{
    const auto editorLoadPolicy = policy == ContentLoadPolicy::RecordFormatConversion
        ? NoteEditor::LoadPolicy::RecordFormatConversion
        : NoteEditor::LoadPolicy::ResetHistory;

    QString      contents;
    Note::Format targetFormat = format;
    switch (format) {
    case Note::Html: {
        QTextDocument document;
        document.setHtml(title + QLatin1String("<br/>") + body);
        contents     = document.toMarkdown(QTextDocument::MarkdownDialectGitHub);
        targetFormat = Note::Markdown;
        break;
    }
    case Note::Markdown:
        contents = title + QLatin1String("\n\n") + body;
        break;
    case Note::PlainText:
        contents = title + QLatin1Char('\n') + body;
        break;
    }

    editor->setMedia(editor->note().media());
    editor->loadDocument(contents, targetFormat, editorLoadPolicy);
    _autosaveTimer.stop();
    _lastChangeElapsed.restart();
    textChanged();
}

QString NoteWidget::text() { return editor ? editor->text().trimmed() : QString(); }

bool NoteWidget::isMarkdown() const { return editor && editor->isMarkdown(); }

Note NoteWidget::note() const { return editor->note(); }

QString NoteWidget::storageId() const { return editor->storageId(); }

QString NoteWidget::noteId() const { return editor->noteId(); }

QUuid NoteWidget::draftId() const { return editor->draftId(); }

bool NoteWidget::hasPersistedDraft() const { return editor->hasPersistedDraft(); }

void NoteWidget::setAcceptRichText(bool state) { _features.setFlag(RichText, state); }

void NoteWidget::setSpeechRecognitionProvider(SpeechRecognitionProviderInterface *provider)
{
    speechProvider = provider;
    updateSpeechRecognitionAction();
}

void NoteWidget::setStickyNotesAvailable(bool available)
{
    if (stickyPinAction) {
        stickyPinAction->setVisible(available);
        stickyPinAction->setEnabled(!editor->noteId().isEmpty() || !text().isEmpty());
    }
}

void NoteWidget::setAlwaysOnTop(bool enabled)
{
    if (alwaysOnTopAction)
        alwaysOnTopAction->setChecked(enabled);
}

bool NoteWidget::alwaysOnTop() const { return alwaysOnTopAction && alwaysOnTopAction->isChecked(); }

void NoteWidget::rehighlight() { qmlEditor->platformBackend()->rehighlight(); }

void NoteWidget::addHighlightExtension(const std::shared_ptr<HighlighterExtension> &extension, int type)
{
    qmlEditor->platformBackend()->addHighlightExtension(extension, type);
}

void NoteWidget::startSpeechRecognition()
{
    if (speechRecognitionBusy) {
        return;
    }
    if (!speechProvider || !speechProvider->isSpeechRecognitionReady() || !speechRecorder->isAvailable()) {
        updateSpeechRecognitionAction();
        return;
    }
    if (speechJob) {
        speechJob->cancel();
        speechJob->deleteLater();
        speechJob.clear();
    }

    auto caps    = speechProvider->speechRecognitionCapabilities();
    auto maxMs   = caps.maxOneShotDurationMs > 0 ? caps.maxOneShotDurationMs : 60000;
    auto started = speechRecorder->start(maxMs);
    if (!started) {
        showSpeechRecognitionError(speechRecorder->errorString());
    }
}

void NoteWidget::finishSpeechRecognition()
{
    if (!speechRecorder || !speechRecorder->isRecording()) {
        return;
    }

    auto audio = speechRecorder->stop();
    QToolTip::hideText();
    if (audio.data.isEmpty() || !speechProvider || !speechProvider->isSpeechRecognitionReady()) {
        updateSpeechRecognitionAction();
        return;
    }

    SpeechRecognitionRequest request;
    request.contextId = speechContextId();
    request.language  = speechRecognitionLanguage();
    request.locale    = request.language.isEmpty() ? QLocale() : QLocale(request.language);

    speechJob = speechProvider->recognizeSpeech(audio, request);
    if (!speechJob) {
        showSpeechRecognitionError(tr("Speech recognition provider did not start a recognition job"));
        return;
    }
    speechRecognitionBusy = true;
    updateSpeechRecognitionAction();
    if (speechButton) {
        QToolTip::showText(speechButton->mapToGlobal(QPoint(0, -speechButton->height())), tr("Recognizing speech..."),
                           speechButton, QRect(), 3000);
    }
    auto job = speechJob;
    connect(job, &SpeechRecognitionJob::finished, this, [this, job](const QString &text) {
        appendRecognizedText(text);
        if (speechJob == job) {
            speechJob.clear();
        }
        speechRecognitionBusy = false;
        updateSpeechRecognitionAction();
        job->deleteLater();
    });
    connect(job, &SpeechRecognitionJob::failed, this, [this, job](const QString &error) {
        showSpeechRecognitionError(error);
        if (speechJob == job) {
            speechJob.clear();
        }
        speechRecognitionBusy = false;
        updateSpeechRecognitionAction();
        job->deleteLater();
    });
}

void NoteWidget::cancelSpeechRecognition()
{
    if (speechRecorder) {
        speechRecorder->cancel();
    }
    if (speechJob) {
        speechJob->cancel();
        speechJob->deleteLater();
        speechJob.clear();
    }
    speechRecognitionBusy = false;
    updateSpeechRecognitionAction();
    QToolTip::hideText();
}

void NoteWidget::updateSpeechRecognitionProgress(qint64 elapsedMs, qint64 maxDurationMs)
{
    if (!speechButton || maxDurationMs <= 0) {
        return;
    }
    auto remainingMs = qMax<qint64>(0, maxDurationMs - elapsedMs);
    auto seconds     = (remainingMs + 999) / 1000;
    QToolTip::showText(speechButton->mapToGlobal(QPoint(0, -speechButton->height())),
                       tr("%n second(s) left", nullptr, int(seconds)), speechButton);
}

void NoteWidget::showSpeechRecognitionError(const QString &error)
{
    if (error.isEmpty()) {
        return;
    }
    if (speechButton) {
        QToolTip::showText(speechButton->mapToGlobal(QPoint(0, -speechButton->height())), error, speechButton, QRect(),
                           6000);
    }
}

void NoteWidget::onCopyClicked()
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(text());
}

void NoteWidget::onPrintClicked()
{
    static QPrinter::OutputFormat lastFormat = QPrinter::NativeFormat;
    static QString                lastOutputFilename;
    static QString                lastPrinterName;
    QPrinter                      printer;

    if (!text().isEmpty()) {
        if (lastPrinterName.size()) {
            printer.setPrinterName(lastPrinterName);
        }
        printer.setOutputFileName(lastOutputFilename);
        printer.setOutputFormat(lastFormat);
        QPrintDialog printDialog(&printer, this);

        if (printDialog.exec() == QDialog::Accepted) {
            QTextDocument document;
            if (isMarkdown())
                document.setMarkdown(text(), QTextDocument::MarkdownDialectGitHub);
            else
                document.setPlainText(text());
            document.print(&printer);
            lastOutputFilename = printer.outputFileName();
            lastFormat         = printer.outputFormat();
            lastPrinterName    = printer.printerName();
        }
    }
}

void NoteWidget::onSaveClicked()
{
    enum class Format { Text, RichText };
    static QMap<Format, QString> allFormats;
    if (allFormats.isEmpty()) {
        allFormats.insert(Format::Text, tr("Text files (*.txt)"));
        allFormats.insert(Format::RichText, tr("HTML files (*.html)"));
    }

    if (_extFileName.isEmpty() || !QFile::exists(_extFileName)) {
        QStringList filters = QStringList() << allFormats[Format::Text];
        if (_features & RichText) {
            filters << allFormats[Format::RichText];
        }
        _extFileName = QFileDialog::getSaveFileName(this, tr("Save Note As"),
                                                    QStandardPaths::writableLocation(QStandardPaths::DesktopLocation),
                                                    filters.join(";;"), &_extSelecteFilter);
    }
    if (!_extFileName.isEmpty()) {
        QFile f(_extFileName);
        if (f.open(QIODevice::WriteOnly)) {
            QString text;
            Format  format = allFormats.key(_extSelecteFilter);
            if (format == Format::RichText) {
                QTextDocument document;
                if (isMarkdown())
                    document.setMarkdown(this->text(), QTextDocument::MarkdownDialectGitHub);
                else
                    document.setPlainText(this->text());
                text = document.toHtml();
            } else {
                text = this->text();
            }
            QByteArray data = text.toLocal8Bit();
#ifdef Q_OS_WIN
            data = data.replace("\n", 1, "\r\n", 2);
#endif
            f.write(data);
            f.close();
        }
    }
}

void NoteWidget::onTrashClicked()
{
    QSettings s;
    if (!text().isEmpty() && s.value("ui.ask-on-delete", true).toBool()) {
        auto mb = new QMessageBox(QMessageBox::Question, tr("Deletion confirmation"),
                                  tr("Are you sure you want to delete this note?"), QMessageBox::Yes | QMessageBox::No);
        mb->setCheckBox(new QCheckBox(tr("Don't ask again")));
        auto res     = mb->exec();
        auto dontAsk = mb->checkBox()->isChecked();
        delete mb;
        if (res != QMessageBox::Yes) {
            return;
        }
        if (dontAsk) {
            s.setValue("ui.ask-on-delete", false);
        }
    }
    _trashRequested = true;
    emit trashRequested();
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool NoteWidget::event(QEvent *event)
{
    if (event->type() == QEvent::ApplicationPaletteChange) {
        updateFirstLineColor();
    }
    return QWidget::event(event);
}
#endif

void NoteWidget::updateFirstLineColor()
{
    QColor hlColor = QSettings().value("ui.title-color", Defaults::firstLineHighlightColor()).value<QColor>();
    QColor merged  = Utils::mergeColors(hlColor, palette().color(QPalette::Text));
    if (merged != firstLineColor) {
        firstLineColor = merged;
        if (qmlEditor)
            qmlEditor->platformBackend()->rehighlight();
    }
}

void NoteWidget::rereadSettings()
{
    QFont     f;
    QSettings s;
    f.fromString(s.value("ui.default-font").toString());
    setFont(f);

    updateFirstLineColor();
    updateSpeechRecognitionAction();
}

void NoteWidget::updateSpeechRecognitionAction()
{
    if (!speechAction || !speechButton || !speechRecorder) {
        return;
    }
    auto language          = speechRecognitionLanguage();
    auto recorderAvailable = speechRecorder->isAvailable();
    auto providerReady     = speechProvider && speechProvider->isSpeechRecognitionReady();
    auto caps = speechProvider ? speechProvider->speechRecognitionCapabilities() : SpeechRecognitionCapabilities();
    bool languageSupported = !language.isEmpty() || caps.languages.isEmpty();
    bool available         = providerReady && recorderAvailable && languageSupported;
    speechAction->setVisible(available || speechRecognitionBusy);
    speechAction->setEnabled(available && !speechRecognitionBusy);
    speechButton->setEnabled(available && !speechRecognitionBusy);
    if (speechRecognitionBusy) {
        speechAction->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
        speechAction->setToolTip(tr("Recognizing speech..."));
    } else {
        speechAction->setIcon(speechIdleIcon);
    }
    if (available && !speechRecognitionBusy) {
        speechAction->setToolTip(language.isEmpty() ? tr("Hold to dictate text")
                                                    : tr("Hold to dictate text (%1)").arg(language));
    }
}

void NoteWidget::appendRecognizedText(const QString &text)
{
    auto trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    qmlEditor->insertText(trimmed);
}

QString NoteWidget::speechRecognitionLanguage() const
{
    if (!speechProvider) {
        return QString();
    }

    auto caps          = speechProvider->speechRecognitionCapabilities();
    auto supportedList = caps.languages;
    for (auto &language : supportedList) {
        language = normalizeSpeechRecognitionLanguage(language);
    }
    supportedList.removeAll(QString());
    supportedList.removeDuplicates();

    auto systemLanguage = normalizeSpeechRecognitionLanguage(QLocale().name());
    if (supportedList.isEmpty()) {
        return systemLanguage;
    }

    if (supportedList.contains(systemLanguage, Qt::CaseInsensitive)) {
        return systemLanguage;
    }

    auto languageOnly = systemLanguage.section(QLatin1Char('-'), 0, 0);
    for (const auto &supported : supportedList) {
        if (supported.section(QLatin1Char('-'), 0, 0).compare(languageOnly, Qt::CaseInsensitive) == 0) {
            return supported;
        }
    }

    auto preferred = normalizeSpeechRecognitionLanguage(caps.preferredLanguage);
    if (!preferred.isEmpty() && supportedList.contains(preferred, Qt::CaseInsensitive)) {
        return preferred;
    }

    return supportedList.value(0);
}

QString NoteWidget::normalizeSpeechRecognitionLanguage(const QString &language) const
{
    auto normalized = language.trimmed();
    normalized.replace(QLatin1Char('_'), QLatin1Char('-'));
    if (normalized.isEmpty()) {
        return QString();
    }

    auto parts = normalized.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return QString();
    }

    parts[0] = parts[0].toLower();
    if (parts.size() > 1) {
        parts[1] = parts[1].toUpper();
    }
    return parts.join(QLatin1Char('-'));
}

QString NoteWidget::speechContextId() const
{
    if (!noteId().isEmpty()) {
        auto key = storageId().toUtf8() + '\0' + noteId().toUtf8();
        return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
    }
    if (localSpeechContextId.isEmpty()) {
        const_cast<NoteWidget *>(this)->localSpeechContextId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    return localSpeechContextId;
}

} // namespace QtNote
