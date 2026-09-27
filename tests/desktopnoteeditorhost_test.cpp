#include <QFocusEvent>
#include <QKeyEvent>
#include <QQmlContext>
#include <QQuickView>
#include <QQuickWidget>
#include <QSignalSpy>
#include <QtTest>

#include "desktopeditorplatformbackend.h"
#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "editorcursorcontroller.h"
#include "noteblockmodel.h"
#include "noteeditor.h"
#include "themediconimageprovider.h"

#include "editortestsupport.h"
#include "quicktestsupport.h"

using namespace AnyKeep;
using namespace AnyKeep::TestSupport;

class DesktopNoteEditorHostTest : public QObject {
    Q_OBJECT

private slots:
    void listKeysReachFocusedDocument_data()
    {
        QTest::addColumn<bool>("embedded");
        QTest::addColumn<int>("key");
        QTest::addColumn<quint32>("scan");
        QTest::addColumn<quint32>("virtualKey");
        QTest::addColumn<int>("listType");
        QTest::addColumn<QString>("paragraph");
        const int types[] = { NoteBlockModel::NumberedList, NoteBlockModel::BulletList, NoteBlockModel::CheckList };
        for (bool embedded : { true, false }) {
            for (int i = 0; i < 3; ++i) {
                const auto name = QByteArray(embedded ? "widget-" : "window-") + QByteArray::number(7 + i);
                QTest::newRow(name.constData())
                    << embedded << int(Qt::Key_7 + i) << quint32(0) << quint32(0) << types[i] << QString();
                QTest::newRow((name + "-paragraph").constData())
                    << embedded << int(Qt::Key_7 + i) << quint32(0) << quint32(0) << types[i]
                    << QStringLiteral("Paragraph");
#if defined(Q_OS_WIN)
                const int us[] = { Qt::Key_Ampersand, Qt::Key_Asterisk, Qt::Key_ParenLeft };
                const int ru[] = { Qt::Key_Question, Qt::Key_Asterisk, Qt::Key_ParenLeft };
                QTest::newRow((name + "-us-native").constData())
                    << embedded << us[i] << quint32(0x08 + i) << quint32(0x37 + i) << types[i] << QString();
                QTest::newRow((name + "-ru-native").constData())
                    << embedded << ru[i] << quint32(0x08 + i) << quint32(0x37 + i) << types[i] << QString();
#elif defined(Q_OS_MACOS)
                const quint32 codes[] = { 0x1a, 0x1c, 0x19 };
                QTest::newRow((name + "-native").constData())
                    << embedded << int(Qt::Key_unknown) << quint32(0) << codes[i] << types[i] << QString();
#elif defined(Q_OS_LINUX)
                if (QGuiApplication::platformName() == QLatin1String("xcb")
                    || QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
                    QTest::newRow((name + "-native").constData())
                        << embedded << int(Qt::Key_unknown) << quint32(16 + i) << quint32(0) << types[i] << QString();
#endif
            }
        }
    }

    void listKeysReachFocusedDocument()
    {
        QFETCH(bool, embedded);
        QFETCH(int, key);
        QFETCH(quint32, scan);
        QFETCH(quint32, virtualKey);
        QFETCH(int, listType);
        QFETCH(QString, paragraph);
        DraftManager drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor   editor(plainNote(), drafts);
        editor.setMarkdown(true);
        editor.model()->insertTextBlock(1);
        const int rows = editor.model()->rowCount();
        QVERIFY(editor.model()->isExplicitEmptyTextBlock(1));
        if (!paragraph.isEmpty())
            editor.model()->setBlockText(1, paragraph);

        // Also exercise a pure Quick window: production manager/standalone
        // windows do not instantiate DesktopNoteEditorHost at all.
        std::unique_ptr<DesktopNoteEditorHost> host;
        std::unique_ptr<QQuickView>            window;
        DesktopEditorPlatformBackend           platform(&editor);
        QQuickItem                            *root;
        QQuickWindow                          *quickWindow;
        QObject                               *receiver;
        if (embedded) {
            host = std::make_unique<DesktopNoteEditorHost>(&editor);
            host->resize(640, 480);
            host->show();
            QCOMPARE(host->quickWidget()->status(), QQuickWidget::Ready);
            root        = host->quickWidget()->rootObject();
            quickWindow = host->quickWidget()->quickWindow();
            receiver    = host->quickWidget();
        } else {
            window = std::make_unique<QQuickView>();
            window->setResizeMode(QQuickView::SizeRootObjectToView);
            installThemedIconImageProvider(window->engine());
            installEditorCursorController(window->rootContext());
            window->rootContext()->setContextProperty(QStringLiteral("noteBlockModel"), editor.model());
            window->rootContext()->setContextProperty(QStringLiteral("noteEditor"), &editor);
            window->rootContext()->setContextProperty(QStringLiteral("desktopEditorPlatform"), &platform);
            window->setSource(QUrl(QStringLiteral("qrc:/qml/DesktopNoteEditor.qml")));
            QCOMPARE(window->status(), QQuickView::Ready);
            window->resize(640, 480);
            window->show();
            root        = window->rootObject();
            quickWindow = window.get();
            receiver    = window.get();
        }
        QVERIFY(root);
        QQuickItem *text     = nullptr;
        const auto  findBody = [&]() {
            text = quickItemByNameAndProperty(root, QStringLiteral("noteBlockTextArea"), "blockIndex", 1);
            return text != nullptr;
        };
        QTRY_VERIFY(findBody());
        if (host)
            host->quickWidget()->setFocus();
        const auto modifiers = Qt::ControlModifier | Qt::ShiftModifier;
        QQuickItem outsideDocument(root);
        outsideDocument.forceActiveFocus();
        QTRY_COMPARE(quickWindow->activeFocusItem(), &outsideDocument);
        QKeyEvent outsidePress(QEvent::KeyPress, key, modifiers, scan, virtualKey, 0);
        QCoreApplication::sendEvent(receiver, &outsidePress);
        QCOMPARE(editor.model()->blockTypeAt(1), NoteBlockModel::Text);
        QCOMPARE(editor.model()->rowCount(), rows);
        text->forceActiveFocus();
        QTRY_COMPARE(quickWindow->activeFocusItem(), text);
        text->setProperty("cursorPosition", paragraph.size() / 2);
        for (const auto extra : { Qt::AltModifier, Qt::MetaModifier, Qt::KeypadModifier }) {
            QKeyEvent otherPress(QEvent::KeyPress, key, modifiers | extra, scan, virtualKey, 0);
            QCoreApplication::sendEvent(receiver, &otherPress);
            QCOMPARE(editor.model()->blockTypeAt(1), NoteBlockModel::Text);
            QCOMPARE(editor.model()->rowCount(), rows);
        }
#if defined(Q_OS_WIN)
        // A matching character from another physical key must not trigger it.
        QKeyEvent otherKey(QEvent::KeyPress, key, modifiers, 0x07, virtualKey, 0);
        QCoreApplication::sendEvent(receiver, &otherKey);
        QCOMPARE(editor.model()->blockTypeAt(1), NoteBlockModel::Text);
#endif
        QKeyEvent overrideEvent(QEvent::ShortcutOverride, key, modifiers, scan, virtualKey, 0);
        QCoreApplication::sendEvent(receiver, &overrideEvent);
        QCOMPARE(editor.model()->blockTypeAt(1), NoteBlockModel::Text);
        QKeyEvent press(QEvent::KeyPress, key, modifiers, scan, virtualKey, 0);
        QCoreApplication::sendEvent(receiver, &press);
        QTRY_COMPARE(editor.model()->blockTypeAt(1), listType);
        QCOMPARE(editor.model()->rowCount(), rows); // Replace this empty line, not the next one.
        QCOMPARE(editor.model()->data(editor.model()->index(1), NoteBlockModel::ItemsRole).toStringList(),
                 QStringList { paragraph });
        QTRY_VERIFY(quickWindow->activeFocusItem()
                    && quickWindow->activeFocusItem()->property("listItemIndex").toInt() == 0
                    && quickWindow->activeFocusItem()->property("blockIndex").toInt() == 1);
        QKeyEvent repeat(QEvent::KeyPress, key, modifiers, scan, virtualKey, 0, {}, true);
        QCoreApplication::sendEvent(receiver, &repeat);
        QCOMPARE(editor.model()->rowCount(), rows);
        // The same routing must change an existing list, not append another one.
        const int nextKey = listType == NoteBlockModel::NumberedList ? Qt::Key_8 : Qt::Key_7;
        const int nextType
            = listType == NoteBlockModel::NumberedList ? NoteBlockModel::BulletList : NoteBlockModel::NumberedList;
        QKeyEvent convert(QEvent::KeyPress, nextKey, modifiers);
        QCoreApplication::sendEvent(receiver, &convert);
        // Mixed lists keep a block type plus a marker type for each item.
        const auto itemType = [&] {
            return editor.model()
                ->data(editor.model()->index(1), NoteBlockModel::ItemTypesRole)
                .toList()
                .value(0)
                .toInt();
        };
        QTRY_COMPARE(itemType(), nextType);
        QCOMPARE(editor.model()->rowCount(), rows);
        QVERIFY(editor.undo());
        QTRY_COMPARE(itemType(), listType);
        QVERIFY(editor.undo());
        QTRY_COMPARE(editor.model()->blockTypeAt(1), NoteBlockModel::Text);
    }

    void modelAndControllerStayShared()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        editor.model()->setBlockText(0, QStringLiteral("Changed\nBody"));
        QCOMPARE(editor.text(), QStringLiteral("Changed\nBody"));
        QVERIFY(editor.isDirty());

        editor.setMarkdown(true);
        QVERIFY(editor.isMarkdown());
        QVERIFY(host.model()->markdown());
    }

    void structuralCommandsUseTheCommonHistory()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        editor.setMarkdown(true);
        const int before = editor.model()->rowCount();
        host.insertTable();
        QCoreApplication::processEvents();
        QCOMPARE(editor.model()->rowCount(), before + 1);
        QVERIFY(editor.canUndo());
        QVERIFY(editor.undo());
        QCOMPARE(editor.model()->rowCount(), before);
    }

    void focusAdapterEmitsCheckpointSignal()
    {
        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);
        QSignalSpy            received(&host, &DesktopNoteEditorHost::focusReceived);
        QSignalSpy            lost(&host, &DesktopNoteEditorHost::focusLost);

        QFocusEvent focusIn(QEvent::FocusIn, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(host.quickWidget(), &focusIn);
        QCOMPARE(received.size(), 1);

        QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(host.quickWidget(), &focusOut);
        QCOMPARE(lost.size(), 1);
    }
};

QTEST_MAIN(DesktopNoteEditorHostTest)

#include "desktopnoteeditorhost_test.moc"
