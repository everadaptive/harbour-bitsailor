// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

#include "autofillprovider.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDateTime>
#include <QDebug>
#include <QRegularExpression>
#include <QUuid>
#include <utility>

#include "urimatcher.h"

namespace {

constexpr qint64 tokenTtlMs = 10 * 60 * 1000;
constexpr qint64 ticketTtlMs = 5 * 60 * 1000;
constexpr int manualFillTtlMs = 5 * 60 * 1000;
constexpr int manualFillCallTimeoutMs = 30 * 1000;
constexpr int refreshIntervalMs = 5 * 60 * 1000;
constexpr auto unsignedCaller = "(none)";

QString err(const char *code)
{
    return QStringLiteral("tech.everadaptive.Autofill1.Error.") + QLatin1String(code);
}

QString newId()
{
    // Qt 5.6 has no QUuid::Id128. Strip the braces and hyphens.
    return QUuid::createUuid().toString().remove(QRegularExpression(QStringLiteral("[{}-]")));
}

}

AutofillProvider::AutofillProvider(BitSailorCore *core, QObject *parent)
    : QObject(parent), m_core(core)
{
    const auto keyIds = QString::fromLocal8Bit(qgetenv("BITSAILOR_AUTOFILL_ALLOWED_KEYIDS"));
    for (const auto &keyId : keyIds.split(QLatin1Char(','), QString::SkipEmptyParts)) {
        m_allowedKeyIds.insert(keyId.trimmed().toLower());
    }

    connect(m_core, &BitSailorCore::loginStatusFetched, this, &AutofillProvider::onLoginStatus);
    connect(m_core, &BitSailorCore::unlockFinished, this, &AutofillProvider::onUnlockFinished);
    connect(m_core, &BitSailorCore::itemsResolved, this, &AutofillProvider::onItemsResolved);
    connect(m_core, &BitSailorCore::itemFetchFinished, this, &AutofillProvider::onItemFetched);

    // The daemon sends SessionClosed when a session ends.
    QDBusConnection::sessionBus().connect(QString(),
                                          QStringLiteral("/tech/everadaptive/Autofill1/Provider"),
                                          QStringLiteral("tech.everadaptive.Autofill1.Provider"),
                                          QStringLiteral("SessionClosed"),
                                          this,
                                          SLOT(onSessionClosed(QString)));

    m_refreshTimer.setInterval(refreshIntervalMs);
    connect(&m_refreshTimer, &QTimer::timeout, this, &AutofillProvider::refreshItems);
}

QString AutofillProvider::displayName() const
{
    return QStringLiteral("BitSailor");
}

uint AutofillProvider::version() const
{
    return 1;
}

void AutofillProvider::start()
{
    qInfo() << "autofill provider: starting. Allowed Key IDs:"
            << (m_allowedKeyIds.isEmpty() ? QStringLiteral("any signed caller")
                                          : QStringList(m_allowedKeyIds.values()).join(QLatin1Char(',')));
    m_core->getLoginStatus();
}

void AutofillProvider::onLoginStatus(BitSailorCore::SessionStatus status)
{
    m_status = status;
    qInfo() << "autofill provider: session status" << status;
    if (status == BitSailorCore::SessionStatusLocked) {
        if (!m_unlockRunning) {
            m_unlockRunning = true;
            m_core->unlockVault();
        }
    } else if (status == BitSailorCore::SessionStatusUnlocked) {
        refreshItems();
    }
}

void AutofillProvider::onUnlockFinished(bool success, const QString &error)
{
    m_unlockRunning = false;
    qInfo() << "autofill provider: unlock" << (success ? "succeeded" : "failed") << error;
    if (success) {
        m_status = BitSailorCore::SessionStatusUnlocked;
        refreshItems();
    }

    const QStringList prompts = m_unlockPrompts;
    m_unlockPrompts.clear();
    for (const auto &path : prompts) {
        completePrompt(path, !success);
    }
}

void AutofillProvider::refreshItems()
{
    if (m_status == BitSailorCore::SessionStatusUnlocked) {
        m_core->fetchItems();
    }
}

void AutofillProvider::onItemsResolved(const QJsonArray &items)
{
    QVector<Entry> entries;
    for (const auto &value : items) {
        const QJsonObject item = value.toObject();
        if (item.value("type").toInt() != BitSailorCore::ItemTypeLogin || !item.value("deletedDate").isNull()) {
            continue;
        }

        const QJsonObject login = item.value("login").toObject();
        Entry entry;
        entry.id = item.value("id").toString();
        entry.name = item.value("name").toString();
        entry.username = login.value("username").toString();
        for (const auto &uriValue : login.value("uris").toArray()) {
            const QJsonObject uri = uriValue.toObject();
            entry.uris.append({uri.value("uri").toString(), uri.value("match").toInt(Autofill::MatchDefault)});
        }
        entries.append(entry);
    }

    const bool changed = m_itemsLoaded && entries.size() != m_entries.size();
    m_entries = entries;
    m_itemsLoaded = true;
    if (!m_refreshTimer.isActive()) {
        m_refreshTimer.start();
    }
    qInfo() << "autofill provider: loaded" << m_entries.size() << "login items";
    if (changed) {
        // Outstanding tokens may point to removed items.
        m_tokens.clear();
        Q_EMIT CredentialsChanged();
    }
}

bool AutofillProvider::callerAllowed(const QVariantMap &request, QString *reason) const
{
    // The daemon attests the caller. app-signature carries the rpm Key ID
    // or the literal "(none)". An unsigned caller gets no candidates.
    const QString signature = request.value(QStringLiteral("app-signature")).toString().toLower();
    if (signature.isEmpty() || signature == QLatin1String(unsignedCaller)) {
        *reason = QStringLiteral("caller is not signed");
        return false;
    }
    if (!m_allowedKeyIds.isEmpty() && !m_allowedKeyIds.contains(signature)) {
        *reason = QStringLiteral("signing key %1 is not allowed").arg(signature);
        return false;
    }
    return true;
}

void AutofillProvider::pruneExpired()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it->expiryMs < now) {
            it = m_tokens.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_tickets.begin(); it != m_tickets.end();) {
        if (it->expiryMs < now) {
            it = m_tickets.erase(it);
        } else {
            ++it;
        }
    }
}

AutofillCandidatesReply AutofillProvider::GetCandidates(const QVariantMap &request)
{
    const QString sessionId = request.value(QStringLiteral("autofill-session-id")).toString();
    const QString origin = request.value(QStringLiteral("origin")).toString();

    QString reason;
    if (!callerAllowed(request, &reason)) {
        qWarning() << "autofill provider: GetCandidates rejected:" << reason;
        sendErrorReply(err("AccessDenied"), reason);
        return {};
    }
    if (m_status != BitSailorCore::SessionStatusUnlocked || !m_itemsLoaded) {
        sendErrorReply(err("VaultLocked"), QStringLiteral("the vault is locked"));
        return {};
    }

    pruneExpired();
    AutofillCandidatesReply reply;
    for (const auto &entry : std::as_const(m_entries)) {
        bool matched = false;
        for (const auto &uri : entry.uris) {
            if (Autofill::uriMatches(origin, uri.first, uri.second)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }

        const QString token = newId();
        m_tokens.insert(token, {sessionId, entry.id, QDateTime::currentMSecsSinceEpoch() + tokenTtlMs});
        reply.candidates.append(AutofillCandidate{token,
                                                  {{QStringLiteral("username"), entry.username},
                                                   {QStringLiteral("title"), entry.name}}});
    }

    qInfo() << "autofill provider: GetCandidates origin" << origin << "->" << reply.candidates.size() << "candidates";
    // Issue a manual fill ticket on every request, so that the user can pick
    // another login even when some logins matched. The ticket is
    // session-bound and single-use.
    const QString ticket = newId();
    m_tickets.insert(ticket, {sessionId, QDateTime::currentMSecsSinceEpoch() + ticketTtlMs});
    reply.response.insert(QStringLiteral("manual-fill-ticket"), ticket);
    return reply;
}

QVariantMap AutofillProvider::Resolve(const QString &token)
{
    pruneExpired();
    const auto found = m_tokens.constFind(token);
    if (found == m_tokens.constEnd()) {
        sendErrorReply(err("InvalidToken"), QStringLiteral("token unknown, consumed, or expired"));
        return {};
    }
    if (m_status != BitSailorCore::SessionStatusUnlocked) {
        sendErrorReply(err("VaultLocked"), QStringLiteral("the vault is locked"));
        return {};
    }

    // One-shot: consume the token now. The password comes from the core.
    const QString itemId = found->itemId;
    m_tokens.remove(token);

    const bool alreadyFetching = m_pendingResolves.contains(itemId);
    m_pendingResolves[itemId].append(message());
    setDelayedReply(true);
    if (!alreadyFetching) {
        m_core->fetchItem(itemId);
    }
    return {};
}

void AutofillProvider::onItemFetched(bool success, const QJsonObject &item)
{
    // The signal carries no request id. Answer the waiting calls of the
    // item that came back. On a failure, answer all waiting calls.
    QStringList ids;
    if (success) {
        ids << item.value("id").toString();
    } else {
        ids = m_pendingResolves.keys();
    }

    for (const auto &id : std::as_const(ids)) {
        const auto calls = m_pendingResolves.take(id);
        for (const auto &call : calls) {
            if (!success) {
                QDBusConnection::sessionBus().send(call.createErrorReply(err("ProviderUnavailable"),
                                                                         QStringLiteral("could not read the item")));
                continue;
            }
            const QJsonObject login = item.value("login").toObject();
            QVariantMap credentials{{QStringLiteral("username"), login.value("username").toString()},
                                    {QStringLiteral("password"), login.value("password").toString()}};
            // A custom field named "email" carries an email address apart
            // from the username.
            for (const auto &fieldValue : item.value("fields").toArray()) {
                const QJsonObject field = fieldValue.toObject();
                if (field.value("name").toString().trimmed().toLower() == QLatin1String("email")
                        && !field.value("value").toString().isEmpty()) {
                    credentials.insert(QStringLiteral("email"), field.value("value").toString());
                    break;
                }
            }
            QDBusConnection::sessionBus().send(call.createReply(QVariant::fromValue(credentials)));
        }
    }
}

QString AutofillProvider::newPrompt()
{
    return QStringLiteral("/tech/everadaptive/Autofill1/prompt/p%1").arg(++m_promptCounter);
}

void AutofillProvider::completePrompt(const QString &path, bool dismissed, const QString &result)
{
    QDBusMessage signal = QDBusMessage::createSignal(path,
                                                     QStringLiteral("tech.everadaptive.Autofill1.Prompt"),
                                                     QStringLiteral("Completed"));
    signal.setArguments({dismissed, QVariant::fromValue(QDBusVariant(result))});
    QDBusConnection::sessionBus().send(signal);
}

QDBusObjectPath AutofillProvider::Unlock()
{
    // The path is valid at once. The result follows as a signal.
    const QString path = newPrompt();
    if (m_status == BitSailorCore::SessionStatusUnlocked) {
        QTimer::singleShot(100, this, [this, path]() { completePrompt(path, false); });
        return QDBusObjectPath(path);
    }

    m_unlockPrompts.append(path);
    if (!m_unlockRunning) {
        m_unlockRunning = true;
        m_core->unlockVault();
    }
    return QDBusObjectPath(path);
}

QDBusObjectPath AutofillProvider::RequestManualFill(const QVariantMap &options)
{
    pruneExpired();
    const QString sessionId = options.value(QStringLiteral("autofill-session-id")).toString();
    const QString ticket = options.value(QStringLiteral("manual-fill-ticket")).toString();

    const auto found = m_tickets.constFind(ticket);
    if (found == m_tickets.constEnd() || found->sessionId != sessionId) {
        m_tickets.remove(ticket);
        sendErrorReply(err("InvalidArgs"), QStringLiteral("missing, unknown, expired, or session-mismatched manual-fill-ticket"));
        return QDBusObjectPath();
    }
    m_tickets.remove(ticket);

    if (m_status != BitSailorCore::SessionStatusUnlocked || !m_itemsLoaded) {
        sendErrorReply(err("VaultLocked"), QStringLiteral("the vault is locked"));
        return QDBusObjectPath();
    }

    // The path is valid at once. The UI app shows the picker. It reports the
    // choice through the ManualFillBridge object.
    const QString path = newPrompt();
    const QString requestId = newId();
    m_manualFills.insert(requestId, {sessionId, path});
    QTimer::singleShot(manualFillTtlMs, this, [this, requestId]() { expireManualFill(requestId); });

    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("cz.chrastecky.bitsailor"),
                                                       QStringLiteral("/autofill"),
                                                       QStringLiteral("cz.chrastecky.bitsailor.AutofillPicker"),
                                                       QStringLiteral("ShowPicker"));
    call.setArguments({requestId,
                       options.value(QStringLiteral("origin")).toString(),
                       options.value(QStringLiteral("app-id")).toString()});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, manualFillCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, requestId]() {
        const QDBusPendingReply<> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) {
            qWarning() << "autofill provider: could not open the picker:" << reply.error().message();
            expireManualFill(requestId);
        }
    });
    qInfo() << "autofill provider: manual fill requested, picker requested from the UI app";
    return QDBusObjectPath(path);
}

void AutofillProvider::expireManualFill(const QString &requestId)
{
    const auto fill = m_manualFills.take(requestId);
    if (!fill.promptPath.isEmpty()) {
        completePrompt(fill.promptPath, true);
    }
}

QString AutofillProvider::manualFillSelected(const QString &requestId, const QString &itemId)
{
    const auto found = m_manualFills.constFind(requestId);
    if (found == m_manualFills.constEnd()) {
        return QStringLiteral("unknown or expired request");
    }

    bool known = false;
    for (const auto &entry : std::as_const(m_entries)) {
        if (entry.id == itemId) {
            known = true;
            break;
        }
    }
    if (!known) {
        return QStringLiteral("unknown item");
    }

    // The token follows the same rules as a token from GetCandidates.
    const QString token = newId();
    m_tokens.insert(token, {found->sessionId, itemId, QDateTime::currentMSecsSinceEpoch() + tokenTtlMs});
    const QString path = found->promptPath;
    m_manualFills.remove(requestId);
    completePrompt(path, false, token);
    return QString();
}

QString AutofillProvider::manualFillCancelled(const QString &requestId)
{
    if (!m_manualFills.contains(requestId)) {
        return QStringLiteral("unknown or expired request");
    }
    expireManualFill(requestId);
    return QString();
}

void AutofillProvider::onSessionClosed(const QString &sessionId)
{
    for (auto it = m_manualFills.begin(); it != m_manualFills.end();) {
        if (it->sessionId == sessionId) {
            it = m_manualFills.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_tokens.begin(); it != m_tokens.end();) {
        if (it->sessionId == sessionId) {
            it = m_tokens.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_tickets.begin(); it != m_tickets.end();) {
        if (it->sessionId == sessionId) {
            it = m_tickets.erase(it);
        } else {
            ++it;
        }
    }
}

ManualFillBridge::ManualFillBridge(AutofillProvider *provider, QObject *parent)
    : QObject(parent), m_provider(provider)
{
}

bool ManualFillBridge::callerIsUi()
{
    // The UI app owns this name. Any other caller is refused.
    const auto owner = QDBusConnection::sessionBus().interface()->serviceOwner(QStringLiteral("cz.chrastecky.bitsailor"));
    if (!owner.isValid() || message().service() != owner.value()) {
        sendErrorReply(QStringLiteral("tech.everadaptive.Autofill1.Error.AccessDenied"),
                       QStringLiteral("only the BitSailor app may answer a manual fill"));
        return false;
    }
    return true;
}

void ManualFillBridge::Selected(const QString &requestId, const QString &itemId)
{
    if (!callerIsUi()) {
        return;
    }
    const QString error = m_provider->manualFillSelected(requestId, itemId);
    if (!error.isEmpty()) {
        sendErrorReply(QStringLiteral("tech.everadaptive.Autofill1.Error.InvalidArgs"), error);
    }
}

void ManualFillBridge::Cancelled(const QString &requestId)
{
    if (!callerIsUi()) {
        return;
    }
    const QString error = m_provider->manualFillCancelled(requestId);
    if (!error.isEmpty()) {
        sendErrorReply(QStringLiteral("tech.everadaptive.Autofill1.Error.InvalidArgs"), error);
    }
}
