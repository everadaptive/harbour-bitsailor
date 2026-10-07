// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT
//
// The autofill provider of BitSailor on the stub core. It does what
// runAutofillProvider() in src/autofill/autofillmode.cpp does, without the
// secrets stack.

#include <QCoreApplication>
#include <QDBusConnection>

#include "../appsettings.h"
#include "../bitsailorcore.h"
#include "autofillprovider.h"
#include "provideradaptor.h"

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("harbour-bitsailor"));
    app.setApplicationName(QStringLiteral("harbour-bitsailor"));
    registerAutofillTypes();

    auto settings = new AppSettings(&app);
    auto core = new BitSailorCore(&app);
    if (!QDBusConnection::sessionBus().registerService(QStringLiteral("cz.chrastecky.bitsailor.Autofill"))) {
        return 1;
    }
    auto provider = new AutofillProvider(core, settings, &app);
    (void)new ProviderAdaptor(provider);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/tech/everadaptive/Autofill1/Provider"), provider);
    auto bridge = new ManualFillBridge(provider, &app);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/cz/chrastecky/bitsailor/Autofill"), bridge,
                                                 QDBusConnection::ExportAllSlots);
    provider->start();
    return app.exec();
}
