#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QSettings>
#include <QtTest>

#include "desktopeditorplatformbackend.h"
#include "desktopnoteeditorhost.h"
#include "draftmanager.h"
#include "noteeditor.h"
#include "pluginlistmodel.h"
#include "themediconimageprovider.h"
#include "windowgeometryutils.h"

#include "desktopqmltestsupport.h"
#include "editortestsupport.h"
#include "quicktestsupport.h"

using namespace AnyKeep;
using namespace AnyKeep::TestSupport;

class SettingsQmlTest : public QObject {
    Q_OBJECT

private slots:
    void restoredWindowGeometryIsConstrainedToAvailableScreens()
    {
        const QList<QRect> screens {
            QRect(0, 0, 1920, 1040),
            QRect(1920, 0, 2560, 1400),
        };

        QCOMPARE(WindowGeometryUtils::constrainToAvailableScreens(QRect(3900, 100, 560, 520), screens, QSize(320, 240)),
                 QRect(3900, 100, 560, 520));

        const QList<QRect> remaining { QRect(0, 0, 1920, 1040) };
        QCOMPARE(
            WindowGeometryUtils::constrainToAvailableScreens(QRect(3900, 100, 560, 520), remaining, QSize(320, 240)),
            QRect(1360, 100, 560, 520));
        QCOMPARE(
            WindowGeometryUtils::constrainToAvailableScreens(QRect(-400, -300, 2600, 1600), remaining, QSize(320, 240)),
            QRect(0, 0, 1920, 1040));
    }

    void themedIconsStayUntintedAndFallbackRecoloringIsExplicit()
    {
        QQmlEngine engine;
        installThemedIconImageProvider(&engine);
        auto *provider = dynamic_cast<QQuickImageProvider *>(engine.imageProvider(QStringLiteral("anykeepicons")));
        QVERIFY(provider);

        const auto request = [provider](const QString &id, const QSize &requestedSize = QSize(20, 20)) {
            QSize  actualSize;
            QImage image = provider->requestImage(id, &actualSize, requestedSize);
            return image;
        };

        const QIcon themed = QIcon::fromTheme(QStringLiteral("preferences-system-symbolic"));
        if (!themed.isNull()) {
            const QImage expected = themed.pixmap(20, 20).toImage();
            const QImage actual
                = request(QStringLiteral("preferences-system-symbolic/preferences-system-symbolic.svg/%23ff0000"));
            QCOMPARE(actual, expected);
        }

        const QString missingTheme = QStringLiteral("__missing_theme_icon__/");
        const QImage  original     = request(missingTheme + QStringLiteral("preferences-system-symbolic.svg/original"));
        const QImage  recolored = request(missingTheme + QStringLiteral("preferences-system-symbolic.svg/%23ff0000"));
        QVERIFY(!original.isNull());
        QVERIFY(!recolored.isNull());
        QCOMPARE(original.size(), recolored.size());
        QVERIFY(original != recolored);

        bool foundRedPixel = false;
        for (int y = 0; y < recolored.height() && !foundRedPixel; ++y) {
            for (int x = 0; x < recolored.width(); ++x) {
                const auto pixel = recolored.pixel(x, y);
                if (qAlpha(pixel) > 0 && qRed(pixel) == 255 && qGreen(pixel) == 0 && qBlue(pixel) == 0) {
                    foundRedPixel = true;
                    break;
                }
            }
        }
        QVERIFY(foundRedPixel);

        QImage expectedAtFractionalScale
            = QIcon(QStringLiteral(":/svg/preferences-system-symbolic.svg")).pixmap(QSize(25, 25)).toImage();
        {
            QPainter painter(&expectedAtFractionalScale);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(expectedAtFractionalScale.rect(), QColor(Qt::red));
        }
        const QImage recoloredAtFractionalScale
            = request(missingTheme + QStringLiteral("preferences-system-symbolic.svg/%23ff0000"), QSize(25, 25));
        QCOMPARE(recoloredAtFractionalScale, expectedAtFractionalScale);
    }

    void themedIconSourceTracksApplicationPalette()
    {
        class PaletteRestorer {
        public:
            PaletteRestorer() : original(qApp->palette()) { }
            ~PaletteRestorer()
            {
                qApp->setPalette(original);
                QCoreApplication::processEvents();
            }

            QPalette original;
        } restore;

        QQuickWidget quick;
        installThemedIconImageProvider(quick.engine());
        QQmlComponent component(quick.engine(), QUrl(QStringLiteral("qrc:/qml/ThemedIcon.qml")));
        QCOMPARE(component.status(), QQmlComponent::Ready);

        QVariantMap properties;
        properties.insert(QStringLiteral("themeName"), QStringLiteral("__missing_theme_icon__"));
        properties.insert(QStringLiteral("fallbackName"), QStringLiteral("preferences-system-symbolic.svg"));
        properties.insert(QStringLiteral("recolorFallback"), true);
        auto *item = qobject_cast<QQuickItem *>(component.createWithInitialProperties(properties));
        QVERIFY2(item, qPrintable(component.errorString()));
        quick.setContent(QUrl(QStringLiteral("qrc:/qml/ThemedIcon.qml")), &component, item);
        quick.show();

        const QUrl originalSource = item->property("iconSource").toUrl();
        QVERIFY(originalSource.isValid());

        QPalette changed = qApp->palette();
        changed.setColor(QPalette::Window, QColor(QStringLiteral("#123456")));
        changed.setColor(QPalette::WindowText, QColor(QStringLiteral("#fedcba")));
        changed.setColor(QPalette::Button, QColor(QStringLiteral("#234567")));
        changed.setColor(QPalette::ButtonText, QColor(QStringLiteral("#edcba9")));
        qApp->setPalette(changed);

        QTRY_VERIFY(item->property("iconSource").toUrl() != originalSource);
    }

    void genericSettingsFormCreatesBoundEditors()
    {
        SettingsFormTestController controller;
        QQmlEngine                 engine;
        QQmlComponent              component(&engine, QUrl(QStringLiteral("qrc:/qml/SettingsForm.qml")));
        QCOMPARE(component.status(), QQmlComponent::Ready);

        QVariantMap properties;
        properties.insert(QStringLiteral("controller"), QVariant::fromValue(static_cast<QObject *>(&controller)));
        std::unique_ptr<QObject> form(component.createWithInitialProperties(properties));
        QVERIFY2(form, qPrintable(component.errorString()));
        auto *formItem = qobject_cast<QQuickItem *>(form.get());
        QVERIFY(formItem);
        for (int row = 0; row < controller.rowCount(); ++row) {
            const auto objectName = QStringLiteral("settingsFieldEditor-%1").arg(row);
            QTRY_VERIFY2(quickItemByName(formItem, objectName), qPrintable(objectName));
        }
        auto *usage = quickItemByName(formItem, QStringLiteral("settingsFieldEditor-3"));
        QVERIFY(usage);
        QVERIFY(usage->property("textFormat").toInt() != 0);
    }

    void settingsListsUseAnimatedReordering()
    {
        const auto exercise = [](bool pluginMode) {
            SettingsReorderTestModel model;
            QQuickWidget             quick;
            quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
            quick.resize(360, 180);
            installThemedIconImageProvider(quick.engine());
            quick.rootContext()->setContextProperty(QStringLiteral("settingsReorderModel"), &model);
            quick.rootContext()->setContextProperty(QStringLiteral("settingsPluginMode"), pluginMode);
            quick.setSource(QUrl(QStringLiteral("qrc:/qml/AnimatedSettingsList.qml")));
            QCOMPARE(quick.status(), QQuickWidget::Ready);
            quick.show();
            QTest::qWait(30);

            auto *root = qobject_cast<QQuickItem *>(quick.rootObject());
            QVERIFY(root);
            QCOMPARE(root->property("backgroundColor").value<QColor>(),
                     QGuiApplication::palette().color(QPalette::Base));
            auto *first  = quickItemByName(root, QStringLiteral("settingsRow-a"));
            auto *second = quickItemByName(root, QStringLiteral("settingsRow-b"));
            auto *last   = quickItemByName(root, QStringLiteral("settingsRow-c"));
            QTRY_VERIFY(first);
            QTRY_VERIFY(second);
            QTRY_VERIFY(last);
            if (pluginMode) {
                auto *firstCheck  = quickItemByName(root, QStringLiteral("settingsPolicyCheck-a"));
                auto *secondCheck = quickItemByName(root, QStringLiteral("settingsPolicyCheck-b"));
                auto *configure   = quickItemByName(root, QStringLiteral("settingsConfigureButton-a"));
                QTRY_VERIFY(firstCheck);
                QTRY_VERIFY(secondCheck);
                QTRY_VERIFY(configure);
                const qreal firstX  = firstCheck->mapToItem(root, QPointF()).x();
                const qreal secondX = secondCheck->mapToItem(root, QPointF()).x();
                QVERIFY(qAbs(firstX - secondX) < 0.5);
                QVERIFY(configure->mapToItem(root, QPointF()).x() < firstX);
            }

            const QPointF from = first->mapToItem(root, QPointF(13, first->height() / 2));
            const QPointF to   = last->mapToItem(root, QPointF(13, last->height() / 2));
            QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, from.toPoint());
            for (int step = 1; step <= 8; ++step)
                QTest::mouseMove(&quick, (from + (to - from) * (qreal(step) / 8)).toPoint(), 15);
            QTRY_VERIFY(root->property("dragging").toBool());
            QTRY_COMPARE(root->property("previewCount").toInt(), 1);
            QTRY_VERIFY(second->property("reorderOffset").toReal() < -1);

            QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, to.toPoint());
            QTRY_VERIFY(!root->property("dragging").toBool());
            QCOMPARE(model.ids(), QStringList({ QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("a") }));
            QCOMPARE(model.storageMoves, pluginMode ? 0 : 1);
            QCOMPARE(model.pluginMoves, pluginMode ? 1 : 0);
        };

        exercise(false);
        exercise(true);

        // PluginListModel persists through its source and synchronously resets
        // itself during the drop. Keep that real lifecycle covered as well.
        SettingsPluginSource source;
        PluginListModel      model(&source);
        QQuickWidget         quick;
        quick.setResizeMode(QQuickWidget::SizeRootObjectToView);
        quick.resize(360, 180);
        installThemedIconImageProvider(quick.engine());
        quick.rootContext()->setContextProperty(QStringLiteral("settingsReorderModel"), &model);
        quick.rootContext()->setContextProperty(QStringLiteral("settingsPluginMode"), true);
        quick.setSource(QUrl(QStringLiteral("qrc:/qml/AnimatedSettingsList.qml")));
        QCOMPARE(quick.status(), QQuickWidget::Ready);
        quick.show();
        QTest::qWait(30);

        auto *root  = qobject_cast<QQuickItem *>(quick.rootObject());
        auto *first = quickItemByName(root, QStringLiteral("settingsRow-a"));
        auto *last  = quickItemByName(root, QStringLiteral("settingsRow-c"));
        QTRY_VERIFY(first);
        QTRY_VERIFY(last);
        const QPointF from = first->mapToItem(root, QPointF(13, first->height() / 2));
        const QPointF to   = last->mapToItem(root, QPointF(13, last->height() / 2));
        QTest::mousePress(&quick, Qt::LeftButton, Qt::NoModifier, from.toPoint());
        for (int step = 1; step <= 8; ++step)
            QTest::mouseMove(&quick, (from + (to - from) * (qreal(step) / 8)).toPoint(), 15);
        QTRY_VERIFY(root->property("dragging").toBool());
        QTest::mouseRelease(&quick, Qt::LeftButton, Qt::NoModifier, to.toPoint());
        QTRY_VERIFY(!root->property("dragging").toBool());
        QCOMPARE(source.ids, QStringList({ QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("a") }));
        QCOMPARE(source.orderChanges, 1);
    }

    void quickWidgetClearColorTracksApplicationPalette()
    {
        class PaletteRestorer {
        public:
            PaletteRestorer() : original(qApp->palette()) { }
            ~PaletteRestorer()
            {
                qApp->setPalette(original);
                QCoreApplication::processEvents();
            }

            QPalette original;
        } restore;

        DraftManager          drafts(std::make_unique<MemoryDraftStore>());
        NoteEditor            editor(plainNote(), drafts);
        DesktopNoteEditorHost host(&editor);

        // A parent widget may retain an explicit palette while the desktop
        // application palette changes. The Quick scene background must still
        // follow the application palette.
        const QColor staleWidgetBase(QStringLiteral("#334455"));
        QPalette     widgetPalette = host.palette();
        widgetPalette.setColor(QPalette::Base, staleWidgetBase);
        host.setPalette(widgetPalette);

        const QColor changedBase(QStringLiteral("#f1f3f4"));
        QPalette     changed = qApp->palette();
        changed.setColor(QPalette::Base, changedBase);
        qApp->setPalette(changed);

        QCOMPARE(host.palette().color(QPalette::Base), staleWidgetBase);
        QTRY_COMPARE(host.quickWidget()->quickWindow()->color(), changedBase);
    }

    void customSpellingDictionaryPersistsAndCanBeEdited()
    {
        QSettings      settings;
        const QString  key      = QStringLiteral("editor/customSpellingDictionary");
        const QVariant previous = settings.value(key);

        {
            DesktopEditorPlatformBackend backend;
            backend.setCustomSpellingDictionary(
                { QStringLiteral("Beta"), QStringLiteral("alpha"), QStringLiteral("ALPHA"), QStringLiteral(" ") });
            const auto words = backend.customSpellingDictionary();
            QCOMPARE(words.size(), 2);
            QVERIFY(words.contains(QStringLiteral("alpha")));
            QVERIFY(words.contains(QStringLiteral("Beta")));
        }
        {
            DesktopEditorPlatformBackend backend;
            const auto                   words = backend.customSpellingDictionary();
            QCOMPARE(words.size(), 2);
            QVERIFY(words.contains(QStringLiteral("alpha")));
            QVERIFY(words.contains(QStringLiteral("Beta")));
        }

        if (previous.isValid())
            settings.setValue(key, previous);
        else
            settings.remove(key);
    }
};

QTEST_MAIN(SettingsQmlTest)

#include "settingsqml_test.moc"
