#include <QBuffer>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include "pluginmetadata.h"

using namespace QtNote;

class PluginMetadataTest : public QObject {
    Q_OBJECT

private slots:
    void localizedValuesAndIcon();
    void languageFallback();
    void rejectsUnsupportedSchema();
    void rejectsInvalidSemanticVersion();
    void rejectsReversedVersionRange();
    void readsLegacyDesktopEnvironmentMetadata();
    void rejectsMalformedDesktopEnvironmentMetadata();
    void semanticVersionPrecedence_data();
    void semanticVersionPrecedence();
    void semanticVersionRange();
};

static QJsonObject metadataObject()
{
    QImage image(2, 2, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QByteArray png;
    QBuffer    buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");

    QJsonObject data {
        { QStringLiteral("schemaVersion"), PluginMetadataSchemaVersion },
        { QStringLiteral("id"), QStringLiteral("test") },
        { QStringLiteral("name"),
          QJsonObject { { QStringLiteral("en"), QStringLiteral("Test plugin") },
                        { QStringLiteral("ru"), QStringLiteral("Тестовый модуль") },
                        { QStringLiteral("ru_RU"), QStringLiteral("Тестовый плагин") } } },
        { QStringLiteral("description"),
          QJsonObject { { QStringLiteral("en"), QStringLiteral("Description") },
                        { QStringLiteral("ru"), QStringLiteral("Описание") } } },
        { QStringLiteral("author"), QStringLiteral("Author") },
        { QStringLiteral("version"), QStringLiteral("1.2.0") },
        { QStringLiteral("minVersion"), QStringLiteral("3.0.0") },
        { QStringLiteral("maxVersion"), QStringLiteral("3.2.0") },
        { QStringLiteral("homepage"), QStringLiteral("https://example.org") },
        { QStringLiteral("features"), QJsonArray { QStringLiteral("regular") } },
        { QStringLiteral("desktopEnvironments"),
          QJsonArray { QStringLiteral("Cinnamon"), QStringLiteral("X-Cinnamon") } },
        { QStringLiteral("extra"), QJsonObject { { QStringLiteral("configurable"), true } } },
        { QStringLiteral("icon"),
          QJsonObject { { QStringLiteral("mimeType"), QStringLiteral("image/png") },
                        { QStringLiteral("base64"), QString::fromLatin1(png.toBase64()) } } },
    };
    return QJsonObject { { QStringLiteral("MetaData"), data } };
}

void PluginMetadataTest::localizedValuesAndIcon()
{
    PluginMetadata metadata;
    QString        error;
    QVERIFY2(pluginMetadataFromJson(metadataObject(), QLocale(QStringLiteral("ru_RU")), &metadata, &error),
             qPrintable(error));
    QCOMPARE(metadata.id, QStringLiteral("test"));
    QCOMPARE(metadata.name, QStringLiteral("Тестовый плагин"));
    QCOMPARE(metadata.description, QStringLiteral("Описание"));
    QCOMPARE(metadata.version, QStringLiteral("1.2.0"));
    QCOMPARE(metadata.features, QStringList { QStringLiteral("regular") });
    QCOMPARE(metadata.desktopEnvironments, QStringList({ QStringLiteral("cinnamon"), QStringLiteral("x-cinnamon") }));
    QVERIFY(metadata.extra.value(QStringLiteral("configurable")).toBool());
    QVERIFY(!metadata.icon.isNull());
}

void PluginMetadataTest::languageFallback()
{
    PluginMetadata metadata;
    QVERIFY(pluginMetadataFromJson(metadataObject(), QLocale(QStringLiteral("ru_BY")), &metadata));
    QCOMPARE(metadata.name, QStringLiteral("Тестовый модуль"));

    QVERIFY(pluginMetadataFromJson(metadataObject(), QLocale(QStringLiteral("de_DE")), &metadata));
    QCOMPARE(metadata.name, QStringLiteral("Test plugin"));
}

void PluginMetadataTest::rejectsUnsupportedSchema()
{
    auto root = metadataObject();
    auto data = root.value(QStringLiteral("MetaData")).toObject();
    data.insert(QStringLiteral("schemaVersion"), PluginMetadataSchemaVersion + 1);
    root.insert(QStringLiteral("MetaData"), data);

    PluginMetadata metadata;
    QString        error;
    QVERIFY(!pluginMetadataFromJson(root, QLocale::c(), &metadata, &error));
    QVERIFY(error.contains(QStringLiteral("schema")));
}

void PluginMetadataTest::rejectsInvalidSemanticVersion()
{
    auto root = metadataObject();
    auto data = root.value(QStringLiteral("MetaData")).toObject();
    data.insert(QStringLiteral("version"), QStringLiteral("1.02.0"));
    root.insert(QStringLiteral("MetaData"), data);

    PluginMetadata metadata;
    QString        error;
    QVERIFY(!pluginMetadataFromJson(root, QLocale::c(), &metadata, &error));
    QVERIFY(error.contains(QStringLiteral("SemVer")));
}

void PluginMetadataTest::rejectsReversedVersionRange()
{
    auto root = metadataObject();
    auto data = root.value(QStringLiteral("MetaData")).toObject();
    data.insert(QStringLiteral("minVersion"), QStringLiteral("4.0.0"));
    data.insert(QStringLiteral("maxVersion"), QStringLiteral("3.9.9"));
    root.insert(QStringLiteral("MetaData"), data);

    PluginMetadata metadata;
    QString        error;
    QVERIFY(!pluginMetadataFromJson(root, QLocale::c(), &metadata, &error));
    QVERIFY(error.contains(QStringLiteral("greater")));
}

void PluginMetadataTest::readsLegacyDesktopEnvironmentMetadata()
{
    auto root = metadataObject();
    auto data = root.value(QStringLiteral("MetaData")).toObject();
    data.remove(QStringLiteral("desktopEnvironments"));
    auto extra = data.value(QStringLiteral("extra")).toObject();
    extra.insert(QStringLiteral("de"), QJsonArray { QStringLiteral("GNOME") });
    data.insert(QStringLiteral("extra"), extra);
    root.insert(QStringLiteral("MetaData"), data);

    PluginMetadata metadata;
    QString        error;
    QVERIFY2(pluginMetadataFromJson(root, QLocale::c(), &metadata, &error), qPrintable(error));
    QCOMPARE(metadata.desktopEnvironments, QStringList { QStringLiteral("gnome") });
}

void PluginMetadataTest::rejectsMalformedDesktopEnvironmentMetadata()
{
    auto root = metadataObject();
    auto data = root.value(QStringLiteral("MetaData")).toObject();
    data.insert(QStringLiteral("desktopEnvironments"), QStringLiteral("cinnamon"));
    root.insert(QStringLiteral("MetaData"), data);

    PluginMetadata metadata;
    QString        error;
    QVERIFY(!pluginMetadataFromJson(root, QLocale::c(), &metadata, &error));
    QVERIFY(error.contains(QStringLiteral("desktopEnvironments")));
}

void PluginMetadataTest::semanticVersionPrecedence_data()
{
    QTest::addColumn<QString>("left");
    QTest::addColumn<QString>("right");
    QTest::addColumn<int>("expected");

    QTest::newRow("major") << QStringLiteral("2.0.0") << QStringLiteral("1.99.99") << 1;
    QTest::newRow("large component") << QStringLiteral("3.2.287") << QStringLiteral("3.2.28") << 1;
    QTest::newRow("prerelease before release") << QStringLiteral("1.0.0-rc.1") << QStringLiteral("1.0.0") << -1;
    QTest::newRow("numeric prerelease") << QStringLiteral("1.0.0-alpha.2") << QStringLiteral("1.0.0-alpha.10") << -1;
    QTest::newRow("numeric before text") << QStringLiteral("1.0.0-1") << QStringLiteral("1.0.0-alpha") << -1;
    QTest::newRow("build ignored") << QStringLiteral("1.0.0+linux.1") << QStringLiteral("1.0.0+windows.9") << 0;
}

void PluginMetadataTest::semanticVersionPrecedence()
{
    QFETCH(QString, left);
    QFETCH(QString, right);
    QFETCH(int, expected);

    int     result = 0;
    QString error;
    QVERIFY2(compareSemanticVersions(left, right, &result, &error), qPrintable(error));
    QCOMPARE(result, expected);
}

void PluginMetadataTest::semanticVersionRange()
{
    QVERIFY(
        semanticVersionInRange(QStringLiteral("3.2.287"), QStringLiteral("3.0.2"), QStringLiteral("3.2.287+local")));
    QVERIFY(
        !semanticVersionInRange(QStringLiteral("3.3.0-alpha.1"), QStringLiteral("3.0.2"), QStringLiteral("3.2.999")));
}

QTEST_MAIN(PluginMetadataTest)
#include "pluginmetadata_test.moc"
