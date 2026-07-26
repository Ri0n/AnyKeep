#include "desktopnoteactions.h"

#include "noteeditor.h"

#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QPrintDialog>
#include <QPrinter>
#include <QStandardPaths>
#include <QTextDocument>

namespace QtNote {

DesktopNoteActions::DesktopNoteActions(QObject *parent) : QObject(parent) { }

void DesktopNoteActions::setEditor(NoteEditor *editor) { editor_ = editor; }

bool DesktopNoteActions::printNote()
{
    if (!editor_ || editor_->text().isEmpty())
        return false;

    static QPrinter::OutputFormat lastFormat = QPrinter::NativeFormat;
    static QString                lastOutputFileName;
    static QString                lastPrinterName;
    QPrinter                      printer;
    if (!lastPrinterName.isEmpty())
        printer.setPrinterName(lastPrinterName);
    printer.setOutputFileName(lastOutputFileName);
    printer.setOutputFormat(lastFormat);

    QPrintDialog dialog(&printer);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    QTextDocument document;
    if (editor_->isMarkdown())
        document.setMarkdown(editor_->text(), QTextDocument::MarkdownDialectGitHub);
    else
        document.setPlainText(editor_->text());
    document.print(&printer);
    lastOutputFileName = printer.outputFileName();
    lastFormat         = printer.outputFormat();
    lastPrinterName    = printer.printerName();
    return true;
}

bool DesktopNoteActions::exportNote()
{
    if (!editor_)
        return false;

    const QString textFilter = tr("Text files (*.txt)");
    const QString htmlFilter = tr("HTML files (*.html)");
    QStringList   filters { textFilter, htmlFilter };
    if (exportFileName_.isEmpty() || !QFile::exists(exportFileName_)) {
        exportFileName_ = QFileDialog::getSaveFileName(
            nullptr, tr("Export Note"), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
            filters.join(QStringLiteral(";;")), &exportFilter_);
    }
    if (exportFileName_.isEmpty())
        return false;

    QString contents;
    if (exportFilter_ == htmlFilter) {
        QTextDocument document;
        if (editor_->isMarkdown())
            document.setMarkdown(editor_->text(), QTextDocument::MarkdownDialectGitHub);
        else
            document.setPlainText(editor_->text());
        contents = document.toHtml();
    } else {
        contents = editor_->text();
    }

    QFile file(exportFileName_);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit operationFailed(tr("Could not export the note: %1").arg(file.errorString()));
        return false;
    }
    QByteArray data = contents.toUtf8();
#ifdef Q_OS_WIN
    data.replace("\n", "\r\n");
#endif
    if (file.write(data) != data.size()) {
        emit operationFailed(tr("Could not export the note: %1").arg(file.errorString()));
        return false;
    }
    emit operationCompleted(tr("Note exported."));
    return true;
}

} // namespace QtNote
