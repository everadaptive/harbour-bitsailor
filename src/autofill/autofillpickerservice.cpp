// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

#include "autofillpickerservice.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDebug>

namespace {

constexpr auto providerService = "cz.chrastecky.bitsailor.Autofill";
constexpr auto providerPath = "/cz/chrastecky/bitsailor/Autofill";
constexpr auto providerInterface = "cz.chrastecky.bitsailor.AutofillControl";

}

AutofillPickerService::AutofillPickerService(QObject *parent) : QObject(parent)
{
}

bool AutofillPickerService::hasPending() const
{
    return !m_pending.isEmpty();
}

QVariantMap AutofillPickerService::takePending()
{
    if (!m_pending.isEmpty()) {
        qInfo() << "autofill picker: request taken by the UI";
    }
    const QVariantMap pending = m_pending;
    m_pending.clear();
    return pending;
}

bool AutofillPickerService::callerIsProvider()
{
    const auto owner = QDBusConnection::sessionBus().interface()->serviceOwner(QString::fromLatin1(providerService));
    if (!owner.isValid() || message().service() != owner.value()) {
        sendErrorReply(QStringLiteral("tech.everadaptive.Autofill1.Error.AccessDenied"),
                       QStringLiteral("only the autofill provider may open the picker"));
        return false;
    }
    return true;
}

void AutofillPickerService::ShowPicker(const QString &requestId, const QString &origin, const QString &appId)
{
    // Only the autofill provider may open the picker.
    if (!callerIsProvider()) {
        return;
    }

    // A new request replaces an unshown one. Tell the provider.
    if (!m_pending.isEmpty()) {
        cancel(m_pending.value(QStringLiteral("requestId")).toString());
    }

    qInfo() << "autofill picker: requested for" << origin << "by" << appId;
    m_pending = {{QStringLiteral("requestId"), requestId},
                 {QStringLiteral("origin"), origin},
                 {QStringLiteral("appId"), appId}};
    Q_EMIT pickerRequested();
}

void AutofillPickerService::ShowSavePrompt(const QString &requestId, const QString &origin, const QString &appId,
                                           const QString &action, const QString &username, const QString &itemName)
{
    // Only the autofill provider may open the save prompt.
    if (!callerIsProvider()) {
        return;
    }

    // A new request replaces an unshown one. Tell the provider. Log no username.
    if (!m_pendingSave.isEmpty()) {
        answerSave(m_pendingSave.value(QStringLiteral("requestId")).toString(), QStringLiteral("decline"));
    }

    qInfo() << "autofill picker: save prompt requested for" << origin << "by" << appId << "action" << action;
    m_pendingSave = {{QStringLiteral("requestId"), requestId},
                     {QStringLiteral("origin"), origin},
                     {QStringLiteral("appId"), appId},
                     {QStringLiteral("action"), action},
                     {QStringLiteral("username"), username},
                     {QStringLiteral("itemName"), itemName}};
    Q_EMIT savePromptRequested();
}

bool AutofillPickerService::hasPendingSave() const
{
    return !m_pendingSave.isEmpty();
}

QVariantMap AutofillPickerService::takePendingSave()
{
    const QVariantMap pending = m_pendingSave;
    m_pendingSave.clear();
    return pending;
}

void AutofillPickerService::answerSave(const QString &requestId, const QString &choice)
{
    qInfo() << "autofill picker: save answered:" << choice;
    callProvider(QStringLiteral("SaveAnswered"), {requestId, choice});
}

void AutofillPickerService::report(const QString &message) const
{
    qInfo() << "autofill picker:" << message;
}

void AutofillPickerService::select(const QString &requestId, const QString &itemId)
{
    qInfo() << "autofill picker: login selected";
    callProvider(QStringLiteral("Selected"), {requestId, itemId});
}

void AutofillPickerService::cancel(const QString &requestId)
{
    qInfo() << "autofill picker: request cancelled";
    callProvider(QStringLiteral("Cancelled"), {requestId});
}

void AutofillPickerService::callProvider(const QString &method, const QVariantList &arguments)
{
    QDBusMessage call = QDBusMessage::createMethodCall(QString::fromLatin1(providerService),
                                                       QString::fromLatin1(providerPath),
                                                       QString::fromLatin1(providerInterface),
                                                       method);
    call.setArguments(arguments);
    QDBusConnection::sessionBus().asyncCall(call);
}
