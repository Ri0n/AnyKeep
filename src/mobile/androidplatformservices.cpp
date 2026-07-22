#include "androidplatformservices.h"

#include <QCryptographicHash>
#include <QPointer>
#include <QUrlQuery>

#ifdef Q_OS_ANDROID
#include <QGuiApplication>
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/private/qandroidextras_p.h>
#endif

namespace QtNote {

namespace {
#ifdef Q_OS_ANDROID
    constexpr int ExportRequestCode = 0x514e01;
    constexpr int SpeechRequestCode = 0x514e02;
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

bool AndroidPlatformServices::exportText(const QString &fileName, const QString &mimeType, const QByteArray &contents)
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
                emit guard->operationFailed(AndroidPlatformServices::tr("Could not write the exported note."));
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

    QUrl      url(QStringLiteral("qtnote://note"));
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
    const auto label = javaString(title.isEmpty() ? tr("QtNote note") : title.left(80));
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

} // namespace QtNote
