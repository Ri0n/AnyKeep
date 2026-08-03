#include "androidplatformservices.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QPointer>
#include <QUrlQuery>

#ifdef Q_OS_ANDROID
#include <QGuiApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/private/qandroidextras_p.h>
#endif

namespace AnyKeep {

namespace {
#ifdef Q_OS_ANDROID
    constexpr int ExportRequestCode = 0x514e01;
    constexpr int SpeechRequestCode = 0x514e02;
    constexpr int ImageRequestCode  = 0x514e03;
    constexpr int FileRequestCode   = 0x514e04;
    constexpr int PhotoRequestCode  = 0x514e05;
    constexpr int ActivityResultOk  = -1;

    QJniObject androidContext() { return QNativeInterface::QAndroidApplication::context(); }

    QJniObject javaString(const QString &value) { return QJniObject::fromString(value); }

    QJniObject newIntent(const char *action)
    {
        const auto javaAction = javaString(QString::fromLatin1(action));
        return QJniObject("android/content/Intent", "(Ljava/lang/String;)V", javaAction.object<jstring>());
    }

    void putStringExtra(QJniObject &intent, const char *name, const QString &value)
    {
        const auto key   = javaString(QString::fromLatin1(name));
        const auto extra = javaString(value);
        intent.callObjectMethod("putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;",
                                key.object<jstring>(), extra.object<jstring>());
    }

    bool writeBytesToUri(const QJniObject &uri, const QByteArray &contents)
    {
        const auto context = androidContext();
        if (!context.isValid() || !uri.isValid())
            return false;

        const auto resolver = context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
        if (!resolver.isValid())
            return false;

        const auto stream
            = resolver.callObjectMethod("openOutputStream", "(Landroid/net/Uri;)Ljava/io/OutputStream;", uri.object());
        if (!stream.isValid())
            return false;

        QJniEnvironment environment;
        auto            bytes = environment->NewByteArray(contents.size());
        if (!bytes)
            return false;
        environment->SetByteArrayRegion(bytes, 0, contents.size(),
                                        reinterpret_cast<const jbyte *>(contents.constData()));
        stream.callMethod<void>("write", "([B)V", bytes);
        stream.callMethod<void>("flush", "()V");
        stream.callMethod<void>("close", "()V");
        environment->DeleteLocalRef(bytes);

        if (environment->ExceptionCheck()) {
            environment->ExceptionClear();
            return false;
        }
        return true;
    }

    QByteArray readBytesFromUri(const QJniObject &uri)
    {
        const auto context = androidContext();
        if (!context.isValid() || !uri.isValid())
            return {};
        const auto resolver = context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
        const auto stream
            = resolver.callObjectMethod("openInputStream", "(Landroid/net/Uri;)Ljava/io/InputStream;", uri.object());
        if (!stream.isValid())
            return {};

        QJniObject      output("java/io/ByteArrayOutputStream", "()V");
        QJniEnvironment environment;
        jbyteArray      buffer = environment->NewByteArray(16 * 1024);
        if (!output.isValid() || !buffer)
            return {};
        while (true) {
            const jint count = stream.callMethod<jint>("read", "([B)I", buffer);
            if (count <= 0)
                break;
            output.callMethod<void>("write", "([BII)V", buffer, 0, count);
        }
        stream.callMethod<void>("close", "()V");
        const auto bytes = output.callObjectMethod("toByteArray", "()[B");
        environment->DeleteLocalRef(buffer);
        if (!bytes.isValid() || environment->ExceptionCheck()) {
            environment->ExceptionClear();
            return {};
        }
        const auto  array = bytes.object<jbyteArray>();
        const jsize size  = environment->GetArrayLength(array);
        QByteArray  result(size, Qt::Uninitialized);
        if (size > 0)
            environment->GetByteArrayRegion(array, 0, size, reinterpret_cast<jbyte *>(result.data()));
        return result;
    }

    QString mimeTypeForUri(const QJniObject &uri)
    {
        const auto context = androidContext();
        if (!context.isValid() || !uri.isValid())
            return {};
        const auto resolver = context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
        const auto type = resolver.callObjectMethod("getType", "(Landroid/net/Uri;)Ljava/lang/String;", uri.object());
        return type.isValid() ? type.toString() : QString();
    }

    QString displayNameForUri(const QJniObject &uri)
    {
        const auto context = androidContext();
        if (!context.isValid() || !uri.isValid())
            return {};
        const auto resolver = context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
        if (!resolver.isValid())
            return {};
        const auto cursor = resolver.callObjectMethod("query",
                                                      "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/"
                                                      "lang/String;Ljava/lang/String;)Landroid/database/Cursor;",
                                                      uri.object(), jobjectArray(nullptr), jstring(nullptr),
                                                      jobjectArray(nullptr), jstring(nullptr));
        if (!cursor.isValid())
            return {};
        QString result;
        if (cursor.callMethod<jboolean>("moveToFirst", "()Z")) {
            const auto column = javaString(QStringLiteral("_display_name"));
            const jint index
                = cursor.callMethod<jint>("getColumnIndex", "(Ljava/lang/String;)I", column.object<jstring>());
            if (index >= 0) {
                const auto value = cursor.callObjectMethod("getString", "(I)Ljava/lang/String;", index);
                if (value.isValid())
                    result = value.toString();
            }
        }
        cursor.callMethod<void>("close", "()V");
        QJniEnvironment environment;
        if (environment->ExceptionCheck()) {
            environment->ExceptionClear();
            return {};
        }
        return result;
    }

    QJniObject createPhotoOutputUri(const QString &fileName)
    {
        const auto context = androidContext();
        if (!context.isValid())
            return {};
        const auto resolver   = context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
        const auto collection = QJniObject::getStaticObjectField("android/provider/MediaStore$Images$Media",
                                                                 "EXTERNAL_CONTENT_URI", "Landroid/net/Uri;");
        if (!resolver.isValid() || !collection.isValid())
            return {};

        QJniObject values("android/content/ContentValues", "()V");
        if (!values.isValid())
            return {};
        const auto nameKey = javaString(QStringLiteral("_display_name"));
        const auto mimeKey = javaString(QStringLiteral("mime_type"));
        const auto name    = javaString(fileName);
        const auto mime    = javaString(QStringLiteral("image/jpeg"));
        values.callMethod<void>("put", "(Ljava/lang/String;Ljava/lang/String;)V", nameKey.object<jstring>(),
                                name.object<jstring>());
        values.callMethod<void>("put", "(Ljava/lang/String;Ljava/lang/String;)V", mimeKey.object<jstring>(),
                                mime.object<jstring>());
        auto uri
            = resolver.callObjectMethod("insert", "(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;",
                                        collection.object(), values.object());
        QJniEnvironment environment;
        if (environment->ExceptionCheck()) {
            environment->ExceptionClear();
            return {};
        }
        return uri;
    }

    void deleteUri(const QJniObject &uri)
    {
        const auto context = androidContext();
        if (!context.isValid() || !uri.isValid())
            return;
        const auto resolver = context.callObjectMethod("getContentResolver", "()Landroid/content/ContentResolver;");
        if (!resolver.isValid())
            return;
        resolver.callMethod<jint>("delete", "(Landroid/net/Uri;Ljava/lang/String;[Ljava/lang/String;)I", uri.object(),
                                  jobject(nullptr), jobjectArray(nullptr));
        QJniEnvironment environment;
        if (environment->ExceptionCheck())
            environment->ExceptionClear();
    }

    QByteArray cameraThumbnailJpeg(const QJniObject &data)
    {
        if (!data.isValid())
            return {};
        const auto key    = javaString(QStringLiteral("data"));
        const auto extras = data.callObjectMethod("getExtras", "()Landroid/os/Bundle;");
        const auto bitmap = extras.isValid()
            ? extras.callObjectMethod("get", "(Ljava/lang/String;)Ljava/lang/Object;", key.object<jstring>())
            : QJniObject();
        if (!bitmap.isValid())
            return {};
        QJniObject output("java/io/ByteArrayOutputStream", "()V");
        const auto format = QJniObject::getStaticObjectField("android/graphics/Bitmap$CompressFormat", "JPEG",
                                                             "Landroid/graphics/Bitmap$CompressFormat;");
        if (!output.isValid() || !format.isValid()
            || !bitmap.callMethod<jboolean>("compress",
                                            "(Landroid/graphics/Bitmap$CompressFormat;ILjava/io/OutputStream;)Z",
                                            format.object(), 92, output.object()))
            return {};
        const auto bytesObject = output.callObjectMethod("toByteArray", "()[B");
        if (!bytesObject.isValid())
            return {};
        QJniEnvironment environment;
        const auto      array = bytesObject.object<jbyteArray>();
        const jsize     size  = environment->GetArrayLength(array);
        QByteArray      bytes(size, Qt::Uninitialized);
        if (size > 0)
            environment->GetByteArrayRegion(array, 0, size, reinterpret_cast<jbyte *>(bytes.data()));
        if (environment->ExceptionCheck()) {
            environment->ExceptionClear();
            return {};
        }
        return bytes;
    }
#endif
}

AndroidPlatformServices::AndroidPlatformServices(QObject *parent) : QObject(parent) { }

bool AndroidPlatformServices::speechRecognitionAvailable() const
{
#ifdef Q_OS_ANDROID
    const auto context = androidContext();
    return context.isValid()
        && QJniObject::callStaticMethod<jboolean>("android/speech/SpeechRecognizer", "isRecognitionAvailable",
                                                  "(Landroid/content/Context;)Z", context.object());
#else
    return false;
#endif
}

bool AndroidPlatformServices::homeScreenShortcutAvailable() const
{
#ifdef Q_OS_ANDROID
    const auto context = androidContext();
    if (!context.isValid())
        return false;
    const auto serviceName = javaString(QStringLiteral("shortcut"));
    const auto manager     = context.callObjectMethod("getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
                                                      serviceName.object<jstring>());
    return manager.isValid() && manager.callMethod<jboolean>("isRequestPinShortcutSupported", "()Z");
#else
    return false;
#endif
}

bool AndroidPlatformServices::shareText(const QString &title, const QString &text)
{
#ifdef Q_OS_ANDROID
    auto intent = newIntent("android.intent.action.SEND");
    if (!intent.isValid())
        return false;
    const auto mime = javaString(QStringLiteral("text/plain"));
    intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;", mime.object<jstring>());
    putStringExtra(intent, "android.intent.extra.SUBJECT", title);
    putStringExtra(intent, "android.intent.extra.TEXT", text);

    const auto chooserTitle = javaString(title);
    const auto chooser      = QJniObject::callStaticObjectMethod(
        "android/content/Intent", "createChooser",
        "(Landroid/content/Intent;Ljava/lang/CharSequence;)Landroid/content/Intent;", intent.object(),
        chooserTitle.object<jstring>());
    if (!chooser.isValid())
        return false;
    QtAndroidPrivate::startActivity(chooser, 0);
    return true;
#else
    Q_UNUSED(title)
    Q_UNUSED(text)
    return false;
#endif
}

bool AndroidPlatformServices::exportData(const QString &fileName, const QString &mimeType, const QByteArray &contents)
{
#ifdef Q_OS_ANDROID
    auto intent = newIntent("android.intent.action.CREATE_DOCUMENT");
    if (!intent.isValid())
        return false;
    const auto category = javaString(QStringLiteral("android.intent.category.OPENABLE"));
    intent.callObjectMethod("addCategory", "(Ljava/lang/String;)Landroid/content/Intent;", category.object<jstring>());
    const auto mime = javaString(mimeType);
    intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;", mime.object<jstring>());
    putStringExtra(intent, "android.intent.extra.TITLE", fileName);

    const QPointer<AndroidPlatformServices> guard(this);
    QtAndroidPrivate::startActivity(
        intent, ExportRequestCode, [guard, contents](int requestCode, int resultCode, const QJniObject &data) {
            if (!guard || requestCode != ExportRequestCode || resultCode != ActivityResultOk || !data.isValid())
                return;
            const auto uri = data.callObjectMethod("getData", "()Landroid/net/Uri;");
            if (!writeBytesToUri(uri, contents)) {
                emit guard->operationFailed(AndroidPlatformServices::tr("Could not write the exported file."));
                return;
            }
            emit guard->exportCompleted();
        });
    return true;
#else
    Q_UNUSED(fileName)
    Q_UNUSED(mimeType)
    Q_UNUSED(contents)
    return false;
#endif
}

bool AndroidPlatformServices::requestSpeechRecognition(const QString &language)
{
#ifdef Q_OS_ANDROID
    if (!speechRecognitionAvailable())
        return false;

    auto intent = newIntent("android.speech.action.RECOGNIZE_SPEECH");
    if (!intent.isValid())
        return false;
    putStringExtra(intent, "android.speech.extra.LANGUAGE_MODEL", QStringLiteral("free_form"));
    putStringExtra(intent, "android.speech.extra.PROMPT", tr("Speak now"));
    if (!language.isEmpty())
        putStringExtra(intent, "android.speech.extra.LANGUAGE", language);

    const QPointer<AndroidPlatformServices> guard(this);
    QtAndroidPrivate::startActivity(
        intent, SpeechRequestCode, [guard](int requestCode, int resultCode, const QJniObject &data) {
            if (!guard || requestCode != SpeechRequestCode || resultCode != ActivityResultOk || !data.isValid())
                return;
            const auto key     = javaString(QStringLiteral("android.speech.extra.RESULTS"));
            const auto results = data.callObjectMethod(
                "getStringArrayListExtra", "(Ljava/lang/String;)Ljava/util/ArrayList;", key.object<jstring>());
            if (!results.isValid() || results.callMethod<jint>("size", "()I") <= 0)
                return;
            const auto first = results.callObjectMethod("get", "(I)Ljava/lang/Object;", 0);
            const auto text  = first.toString().trimmed();
            if (!text.isEmpty())
                emit guard->speechRecognized(text);
        });
    return true;
#else
    Q_UNUSED(language)
    return false;
#endif
}

bool AndroidPlatformServices::requestImage()
{
#ifdef Q_OS_ANDROID
    auto intent = newIntent("android.intent.action.OPEN_DOCUMENT");
    if (!intent.isValid())
        return false;
    const auto category = javaString(QStringLiteral("android.intent.category.OPENABLE"));
    intent.callObjectMethod("addCategory", "(Ljava/lang/String;)Landroid/content/Intent;", category.object<jstring>());
    const auto mime = javaString(QStringLiteral("image/*"));
    intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;", mime.object<jstring>());

    const QPointer<AndroidPlatformServices> guard(this);
    QtAndroidPrivate::startActivity(
        intent, ImageRequestCode, [guard](int requestCode, int resultCode, const QJniObject &data) {
            if (!guard || requestCode != ImageRequestCode || resultCode != ActivityResultOk || !data.isValid())
                return;
            const auto       uri   = data.callObjectMethod("getData", "()Landroid/net/Uri;");
            const QByteArray bytes = readBytesFromUri(uri);
            if (bytes.isEmpty()) {
                emit guard->operationFailed(AndroidPlatformServices::tr("Could not read the selected image."));
                return;
            }
            QString name = displayNameForUri(uri);
            if (name.isEmpty())
                name = uri.callObjectMethod("getLastPathSegment", "()Ljava/lang/String;").toString();
            if (name.isEmpty())
                name = QStringLiteral("image");
            emit guard->imageSelected(bytes, name, mimeTypeForUri(uri));
        });
    return true;
#else
    return false;
#endif
}

bool AndroidPlatformServices::requestFile()
{
#ifdef Q_OS_ANDROID
    auto intent = newIntent("android.intent.action.OPEN_DOCUMENT");
    if (!intent.isValid())
        return false;
    const auto category = javaString(QStringLiteral("android.intent.category.OPENABLE"));
    intent.callObjectMethod("addCategory", "(Ljava/lang/String;)Landroid/content/Intent;", category.object<jstring>());
    const auto mime = javaString(QStringLiteral("*/*"));
    intent.callObjectMethod("setType", "(Ljava/lang/String;)Landroid/content/Intent;", mime.object<jstring>());

    const QPointer<AndroidPlatformServices> guard(this);
    QtAndroidPrivate::startActivity(
        intent, FileRequestCode, [guard](int requestCode, int resultCode, const QJniObject &data) {
            if (!guard || requestCode != FileRequestCode || resultCode != ActivityResultOk || !data.isValid())
                return;
            const auto       uri   = data.callObjectMethod("getData", "()Landroid/net/Uri;");
            const QByteArray bytes = readBytesFromUri(uri);
            if (bytes.isEmpty()) {
                emit guard->operationFailed(AndroidPlatformServices::tr("Could not read the selected file."));
                return;
            }
            QString name = displayNameForUri(uri);
            if (name.isEmpty())
                name = uri.callObjectMethod("getLastPathSegment", "()Ljava/lang/String;").toString();
            if (name.isEmpty())
                name = QStringLiteral("attachment");
            emit guard->fileSelected(bytes, name, mimeTypeForUri(uri));
        });
    return true;
#else
    return false;
#endif
}

bool AndroidPlatformServices::requestPhoto()
{
#ifdef Q_OS_ANDROID
    auto intent = newIntent("android.media.action.IMAGE_CAPTURE");
    if (!intent.isValid())
        return false;

    const QString photoName
        = QStringLiteral("Photo_%1.jpg").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QJniObject photoUri = createPhotoOutputUri(photoName);
    if (photoUri.isValid()) {
        const auto outputKey = javaString(QStringLiteral("output"));
        intent.callObjectMethod("putExtra", "(Ljava/lang/String;Landroid/os/Parcelable;)Landroid/content/Intent;",
                                outputKey.object<jstring>(), photoUri.object());
        intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", 0x1 | 0x2);
    }

    const QPointer<AndroidPlatformServices> guard(this);
    QtAndroidPrivate::startActivity(
        intent, PhotoRequestCode,
        [guard, photoUri, photoName](int requestCode, int resultCode, const QJniObject &data) {
            if (!guard || requestCode != PhotoRequestCode) {
                deleteUri(photoUri);
                return;
            }
            if (resultCode != ActivityResultOk) {
                deleteUri(photoUri);
                return;
            }

            QByteArray bytes;
            if (photoUri.isValid()) {
                bytes = readBytesFromUri(photoUri);
                deleteUri(photoUri);
            }
            // Some camera applications ignore EXTRA_OUTPUT. Retain the
            // thumbnail response only as a compatibility fallback; normal
            // captures use the full-resolution content URI above.
            if (bytes.isEmpty())
                bytes = cameraThumbnailJpeg(data);
            if (bytes.isEmpty()) {
                emit guard->operationFailed(AndroidPlatformServices::tr("The camera did not return a photo."));
                return;
            }
            emit guard->imageSelected(bytes, photoName, QStringLiteral("image/jpeg"));
        });
    return true;
#else
    return false;
#endif
}

bool AndroidPlatformServices::addHomeScreenShortcut(const QString &storageId, const QString &noteId,
                                                    const QString &title)
{
#ifdef Q_OS_ANDROID
    if (storageId.isEmpty() || noteId.isEmpty() || !homeScreenShortcutAvailable())
        return false;

    const auto context        = androidContext();
    const auto serviceName    = javaString(QStringLiteral("shortcut"));
    const auto manager        = context.callObjectMethod("getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;",
                                                         serviceName.object<jstring>());
    const auto packageName    = context.callObjectMethod("getPackageName", "()Ljava/lang/String;");
    const auto packageManager = context.callObjectMethod("getPackageManager", "()Landroid/content/pm/PackageManager;");
    auto       launchIntent   = packageManager.callObjectMethod(
        "getLaunchIntentForPackage", "(Ljava/lang/String;)Landroid/content/Intent;", packageName.object<jstring>());
    if (!manager.isValid() || !launchIntent.isValid())
        return false;

    QUrl      url(QStringLiteral("anykeep://note"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("storage"), storageId);
    query.addQueryItem(QStringLiteral("id"), noteId);
    url.setQuery(query);
    const auto uriText = javaString(url.toString(QUrl::FullyEncoded));
    const auto uri     = QJniObject::callStaticObjectMethod(
        "android/net/Uri", "parse", "(Ljava/lang/String;)Landroid/net/Uri;", uriText.object<jstring>());
    launchIntent.callObjectMethod("setData", "(Landroid/net/Uri;)Landroid/content/Intent;", uri.object());
    launchIntent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", 0x04000000); // CLEAR_TOP

    const auto digest
        = QCryptographicHash::hash((storageId + QLatin1Char('\n') + noteId).toUtf8(), QCryptographicHash::Sha256)
              .toHex()
              .left(24);
    const auto shortcutId = javaString(QStringLiteral("note-") + QString::fromLatin1(digest));
    QJniObject builder("android/content/pm/ShortcutInfo$Builder", "(Landroid/content/Context;Ljava/lang/String;)V",
                       context.object(), shortcutId.object<jstring>());
    const auto label = javaString(title.isEmpty() ? tr("AnyKeep note") : title.left(80));
    builder.callObjectMethod("setShortLabel", "(Ljava/lang/CharSequence;)Landroid/content/pm/ShortcutInfo$Builder;",
                             label.object<jstring>());
    builder.callObjectMethod("setIntent", "(Landroid/content/Intent;)Landroid/content/pm/ShortcutInfo$Builder;",
                             launchIntent.object());
    const auto shortcut = builder.callObjectMethod("build", "()Landroid/content/pm/ShortcutInfo;");
    return shortcut.isValid()
        && manager.callMethod<jboolean>("requestPinShortcut",
                                        "(Landroid/content/pm/ShortcutInfo;Landroid/content/IntentSender;)Z",
                                        shortcut.object(), jobject(nullptr));
#else
    Q_UNUSED(storageId)
    Q_UNUSED(noteId)
    Q_UNUSED(title)
    return false;
#endif
}

QUrl AndroidPlatformServices::pendingLaunchUrl() const
{
#ifdef Q_OS_ANDROID
    if (!QNativeInterface::QAndroidApplication::isActivityContext())
        return {};
    const auto context = androidContext();
    const auto intent  = context.callObjectMethod("getIntent", "()Landroid/content/Intent;");
    if (!intent.isValid())
        return {};
    const auto data = intent.callObjectMethod("getDataString", "()Ljava/lang/String;");
    return data.isValid() ? QUrl(data.toString()) : QUrl();
#else
    return {};
#endif
}

} // namespace AnyKeep
