// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

#include "autofillmode.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDebug>
#include <QJsonObject>
#include <QTimer>

#include "../accountmanager.h"
#include "../appsettings.h"
#include "../bitsailorcore.h"
#include "../secretshandler.h"
#include "autofillpickerservice.h"
#include "autofillprovider.h"
#include "provideradaptor.h"

namespace {

constexpr auto defaultBusName = "cz.chrastecky.bitsailor.Autofill";
constexpr auto providerPath = "/tech/everadaptive/Autofill1/Provider";

// The UI app sets both names from its desktop file. The headless process
// must use the same names to find the same settings.
void setApplicationNames(QCoreApplication &app)
{
    app.setOrganizationName(QStringLiteral("harbour-bitsailor"));
    app.setApplicationName(QStringLiteral("harbour-bitsailor"));
}


// qEnvironmentVariable needs Qt 5.10. The target has Qt 5.6.
QString envString(const char *name)
{
    return QString::fromLocal8Bit(qgetenv(name));
}

}

int runAutofillProvider(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    setApplicationNames(app);
    registerAutofillTypes();

    auto settings = new AppSettings(&app);
    auto accountManager = new AccountManager(settings, &app);
    auto secrets = new SecretsHandler(accountManager, &app);
    auto core = new BitSailorCore(settings, secrets, &app);

    const QString busName = qEnvironmentVariableIsSet("BITSAILOR_AUTOFILL_BUS_NAME")
            ? envString("BITSAILOR_AUTOFILL_BUS_NAME")
            : QString::fromLatin1(defaultBusName);
    if (!QDBusConnection::sessionBus().registerService(busName)) {
        qCritical() << "autofill provider: failed to register bus name" << busName;
        return 1;
    }

    auto provider = new AutofillProvider(core, &app);
    (void)new ProviderAdaptor(provider);
    QDBusConnection::sessionBus().registerObject(QString::fromLatin1(providerPath), provider);
    // The UI app answers manual fill requests through this object.
    auto bridge = new ManualFillBridge(provider, &app);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/cz/chrastecky/bitsailor/Autofill"), bridge,
                                                 QDBusConnection::ExportAllSlots);
    provider->start();

    return app.exec();
}

int runAutofillDevLogin(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    setApplicationNames(app);

    if (envString("BITSAILOR_DEV_RESET") == QLatin1String("1")) {
        // Delete the BitSailor secrets collection. The collection belongs
        // to the application identity that created it.
        AppSettings resetSettings;
        AccountManager resetAccounts(&resetSettings);
        SecretsHandler resetSecrets(&resetAccounts);
        const bool cleared = resetSecrets.clearAllSecrets();
        qInfo() << "dev login: reset secrets" << (cleared ? "done" : "failed");
        return cleared ? 0 : 1;
    }

    const QString server = envString("BITSAILOR_DEV_SERVER");
    const QString email = envString("BITSAILOR_DEV_EMAIL");
    const QString password = envString("BITSAILOR_DEV_PASSWORD");
    if (server.isEmpty() || email.isEmpty() || password.isEmpty()) {
        qCritical() << "dev login: set BITSAILOR_DEV_SERVER, BITSAILOR_DEV_EMAIL and BITSAILOR_DEV_PASSWORD";
        return 2;
    }

    auto settings = new AppSettings(&app);
    auto accountManager = new AccountManager(settings, &app);
    auto secrets = new SecretsHandler(accountManager, &app);

    // Secrets are stored per account. Create the account first.
    accountManager->setCurrentAccount({
        {QStringLiteral("id"), accountManager->generateAccountId()},
        {QStringLiteral("name"), QStringLiteral("Dev account")},
        {QStringLiteral("email"), email},
        {QStringLiteral("server"), server},
    });
    settings->setMigratedToMultiAccounts(true);
    if (envString("BITSAILOR_DEV_INSECURE") == QLatin1String("1")) {
        secrets->allowInvalidCertificates();
    }

    auto core = new BitSailorCore(settings, secrets, &app);

    QObject::connect(core, &BitSailorCore::serverUrlChanged, &app, [=](bool success) {
        if (!success) {
            qCritical() << "dev login: could not set the server URL";
            QCoreApplication::exit(1);
            return;
        }
        qInfo() << "dev login: server set, logging in";
        core->loginEmailPassword(email, password);
    });
    QObject::connect(core, &BitSailorCore::loginFinished, &app, [=](bool success, const QString &error) {
        if (!success) {
            qCritical() << "dev login: login failed:" << error;
            QCoreApplication::exit(1);
            return;
        }
        qInfo() << "dev login: logged in, unlocking";
        core->unlockVault(password);
    });
    QObject::connect(core, &BitSailorCore::unlockFinished, &app, [=](bool success, const QString &error) {
        if (!success) {
            qCritical() << "dev login: unlock failed:" << error;
            QCoreApplication::exit(1);
            return;
        }
        if (!secrets->storeUserKeyFromSessionJson()) {
            qCritical() << "dev login: could not store the user key";
            QCoreApplication::exit(1);
            return;
        }
        qInfo() << "dev login: unlocked, user key stored, syncing";
        core->syncVault();
    });
    QObject::connect(core, &BitSailorCore::syncVaultFinished, &app, [=](bool success) {
        if (!success) {
            qCritical() << "dev login: sync failed";
            QCoreApplication::exit(1);
            return;
        }
        core->fetchItems();
    });
    QObject::connect(core, &BitSailorCore::itemsResolved, &app, [=](const QJsonArray &items) {
        qInfo() << "dev login: done," << items.size() << "items in the vault";
        QCoreApplication::exit(0);
    });

    core->changeServerUrl(server);
    return app.exec();
}

int runAutofillDevPicker(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    setApplicationNames(app);

    const QString itemId = envString("BITSAILOR_DEV_PICK_ID");
    if (itemId.isEmpty()) {
        qCritical() << "dev picker: set BITSAILOR_DEV_PICK_ID";
        return 2;
    }

    QObject *context = &app;
    auto picker = new AutofillPickerService(context);
    QObject::connect(picker, &AutofillPickerService::pickerRequested, context, [=]() {
        const QVariantMap request = picker->takePending();
        qInfo() << "dev picker: picker requested for" << request.value(QStringLiteral("origin")).toString()
                << "by" << request.value(QStringLiteral("appId")).toString();
        QTimer::singleShot(300, context, [=]() {
            picker->select(request.value(QStringLiteral("requestId")).toString(), itemId);
            qInfo() << "dev picker: selected" << itemId;
        });
    });

    QDBusConnection::sessionBus().registerObject(QStringLiteral("/autofill"), picker, QDBusConnection::ExportAllSlots);
    if (!QDBusConnection::sessionBus().registerService(QStringLiteral("cz.chrastecky.bitsailor"))) {
        qCritical() << "dev picker: failed to register the UI bus name";
        return 1;
    }
    qInfo() << "dev picker: ready";
    return app.exec();
}
