#ifdef QT_QML_DEBUG
#include <QtQuick>
#endif

#include <QQuickView>
#include <QScopedPointer>
#include <QGuiApplication>
#include <QtQml>
#include <QQmlEngine>
#include <QDBusConnection>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>

#include <sailfishapp.h>

#include "secretshandler.h"
#include "appsettings.h"
#include "runtimecache.h"
#include "systemauthchecker.h"
#include "fileaccessor.h"
#include "randompingenerator.h"
#include "cachekey.h"
#include "otp/onetimepasswordgenerator.h"
#include "parsedurl.h"
#include "urlparser.h"
#include "bitsailorcore.h"
#include "clipboardhandler.h"
#include "accountmanager.h"
#include "autofill/autofillmode.h"
#include "autofill/autofillpickerservice.h"

namespace {

QMutex messageHandlerMutex;
QtMessageHandler defaultMessageHandler = nullptr;

QString messageTypeName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("debug");
    case QtInfoMsg:
        return QStringLiteral("info");
    case QtWarningMsg:
        return QStringLiteral("warning");
    case QtCriticalMsg:
        return QStringLiteral("critical");
    case QtFatalMsg:
        return QStringLiteral("fatal");
    }

    return QStringLiteral("unknown");
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (defaultMessageHandler != nullptr) {
        defaultMessageHandler(type, context, message);
    } else {
        fprintf(stderr, "%s\n", qPrintable(message));
        fflush(stderr);
    }

    if (type != QtWarningMsg) {
        return;
    }

    const QMutexLocker lock(&messageHandlerMutex);
    const auto logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (logDir.isEmpty() || !QDir().mkpath(logDir)) {
        return;
    }

    QFile file(QDir(logDir).filePath(QStringLiteral("warnings.log")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream stream(&file);
    stream << QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
           << " [" << messageTypeName(type) << "] " << message;

    if (context.file != nullptr) {
        stream << " (" << context.file << ':' << context.line << ')';
    }

    stream << '\n';
}

}

int main(int argc, char *argv[])
{
    // Headless modes for the autofill provider. They skip the UI.
    // BITSAILOR_MODE selects a mode without a command line argument. The
    // secrets daemon identifies an application by its command line, so a
    // plain command line keeps the same identity as the UI process.
    const QByteArray mode = qgetenv("BITSAILOR_MODE");
    bool provider = mode == "autofill-provider";
    bool devLogin = mode == "autofill-dev-login";
    const bool devPicker = mode == "autofill-dev-picker";
    for (int i = 1; i < argc; ++i) {
        provider = provider || qstrcmp(argv[i], "--autofill-provider") == 0;
        devLogin = devLogin || qstrcmp(argv[i], "--autofill-dev-login") == 0;
    }
    if (provider) {
        return runAutofillProvider(argc, argv);
    }
    if (devLogin) {
        return runAutofillDevLogin(argc, argv);
    }
    if (devPicker) {
        return runAutofillDevPicker(argc, argv);
    }

    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    defaultMessageHandler = qInstallMessageHandler(messageHandler);
    QScopedPointer<QQuickView> v(SailfishApp::createView());

    auto settings = new AppSettings(app.data());
    auto accountManager = new AccountManager(settings, app.data());
    auto secrets = new SecretsHandler(accountManager, app.data());
    auto core = new BitSailorCore(settings, secrets, app.data());

    qmlRegisterType<SystemAuthChecker>("cz.chrastecky.bitsailor", 1, 0, "SystemAuthChecker");
    qmlRegisterType<FileAccessor>("cz.chrastecky.bitsailor", 1, 0, "FileAccessor");
    qmlRegisterType<RandomPinGenerator>("cz.chrastecky.bitsailor", 1, 0, "RandomPinGenerator");
    qmlRegisterType<OneTimePasswordGenerator>("cz.chrastecky.bitsailor", 1, 0, "OneTimePasswordGenerator");
    qmlRegisterType<ParsedUrl>("cz.chrastecky.bitasilor", 1, 0, "ParsedUrl");
    qmlRegisterSingletonType<CacheKey>("cz.chrastecky.bitsailor", 1, 0, "CacheKey", [](QQmlEngine* engine, QJSEngine* scriptEngine) -> QObject* {
        Q_UNUSED(engine);
        Q_UNUSED(scriptEngine);

        return new CacheKey();
    });
    qmlRegisterSingletonType<BitSailorCore>("cz.chrastecky.bitsailor", 1, 0, "BitSailorCore", [](QQmlEngine* engine, QJSEngine* scriptEngine) -> QObject* {
        Q_UNUSED(engine);
        Q_UNUSED(scriptEngine);

        return new BitSailorCore(QCoreApplication::instance());
    });

    auto clipboardHandler = new ClipboardHandler(QGuiApplication::clipboard(), settings, app.data());

    // The autofill provider opens the login picker through this object.
    auto autofillPicker = new AutofillPickerService(app.data());
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/autofill"), autofillPicker,
                                                 QDBusConnection::ExportAllSlots);
    v->rootContext()->setContextProperty("autofillPicker", autofillPicker);

    v->rootContext()->setContextProperty("accountManager", accountManager);
    v->rootContext()->setContextProperty("clipboardHandler", clipboardHandler);
    v->rootContext()->setContextProperty("settings", settings);
    v->rootContext()->setContextProperty("secrets", secrets);
    v->rootContext()->setContextProperty("core", core);
    v->rootContext()->setContextProperty("runtimeCache", new RuntimeCache(secrets, app.data()));
    v->rootContext()->setContextProperty("urlParser", new UrlParser(app.data()));

#ifdef QT_DEBUG
    v->rootContext()->setContextProperty("isDebug", true);
#else
    v->rootContext()->setContextProperty("isDebug", false);
#endif

#ifdef HARBOUR_STORE
    v->rootContext()->setContextProperty("isStoreBuild", true);
#else
    v->rootContext()->setContextProperty("isStoreBuild", false);
#endif

    QObject::connect(app.data(), &QCoreApplication::aboutToQuit, clipboardHandler, &ClipboardHandler::clearOnClose);
    v->setSource(SailfishApp::pathToMainQml());
    QDBusConnection::sessionBus().registerService("cz.chrastecky.bitsailor");
    v->show();

    return app->exec();
}
