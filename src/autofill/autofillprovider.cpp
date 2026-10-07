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
constexpr int saveTtlMs = 5 * 60 * 1000;
constexpr qint64 saveRateWindowMs = 60 * 1000;
constexpr int saveRateLimit = 5; // save prompts for each app in the window
constexpr int generateAttempts = 50;
constexpr auto unsignedCaller = "(none)";

static_assert(BitSailorCore::ItemTypeLogin == Autofill::LoginItemType, "the core uses another login item type");

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

AutofillProvider::AutofillProvider(BitSailorCore *core, AppSettings *settings, QObject *parent)
    : QObject(parent), m_core(core), m_settings(settings)
{
    const auto keyIds = QString::fromLocal8Bit(qgetenv("BITSAILOR_AUTOFILL_ALLOWED_KEYIDS"));
    for (const auto &keyId : keyIds.split(QLatin1Char(','), QString::SkipEmptyParts)) {
        m_allowedKeyIds.insert(keyId.trimmed().toLower());
    }

    connect(m_core, &BitSailorCore::loginStatusFetched, this, &AutofillProvider::onLoginStatus);
    connect(m_core, &BitSailorCore::unlockFinished, this, &AutofillProvider::onUnlockFinished);
    connect(m_core, &BitSailorCore::itemsResolved, this, &AutofillProvider::onItemsResolved);
    connect(m_core, &BitSailorCore::itemFetchFinished, this, &AutofillProvider::onItemFetched);
    connect(m_core, &BitSailorCore::itemResolvingFailed, this, &AutofillProvider::onItemsFailed);
    connect(m_core, &BitSailorCore::itemCreationFinished, this, &AutofillProvider::onItemCreated);
    connect(m_core, &BitSailorCore::itemUpdated, this, &AutofillProvider::onItemUpdated);
    connect(m_core, &BitSailorCore::passwordGeneratingFinished, this, &AutofillProvider::onPasswordGenerated);

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
    return 2; // spec §6: 2 = SaveCredentials and GeneratePassword
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

    // On a success the saves resume when the item list arrives.
    if (!success) {
        declineWaitingSaves();
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

    resumeWaitingSaves();
    // The write of a save is complete when the new item list is here.
    const auto active = m_saves.constFind(m_writeActive);
    if (active != m_saves.constEnd() && active->state == SaveRequest::Refreshing) {
        finishWrite(true);
    }
}

void AutofillProvider::onItemsFailed()
{
    // The item list could not be read again. The write itself worked.
    const auto active = m_saves.constFind(m_writeActive);
    if (active != m_saves.constEnd() && active->state == SaveRequest::Refreshing) {
        finishWrite(true);
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
    for (auto it = m_filled.begin(); it != m_filled.end();) {
        if (it->expiryMs < now) {
            it = m_filled.erase(it);
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
    // A later save may name this item with the consumed token.
    m_filled.insert(token, {found->sessionId, itemId, QDateTime::currentMSecsSinceEpoch() + tokenTtlMs});
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
        ids = m_pendingResolves.keys() + m_saveFetches.keys();
    }

    for (const auto &id : std::as_const(ids)) {
        // The saves that wait for this item: unchanged, or ask for an update.
        const auto saves = m_saveFetches.take(id);
        for (const auto &saveId : saves) {
            if (success) {
                afterSaveFetch(saveId, item);
            } else {
                finishSave(saveId, QStringLiteral("failed"), false);
            }
        }

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

void AutofillProvider::completePrompt(const QString &path, bool dismissed, const QVariant &result)
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

QDBusObjectPath AutofillProvider::SaveCredentials(const QVariantMap &request, const QVariantMap &credentials)
{
    const QString sessionId = request.value(QStringLiteral("autofill-session-id")).toString();
    const QString requestOrigin = request.value(QStringLiteral("origin")).toString();
    const QString email = credentials.value(QStringLiteral("email")).toString().trimmed();
    QString username = credentials.value(QStringLiteral("username")).toString().trimmed();
    if (username.isEmpty()) {
        username = email;
    }
    const QString password = credentials.value(QStringLiteral("password")).toString();

    // Never log a credential value, and never put one into an error text.
    if (sessionId.isEmpty() || requestOrigin.isEmpty() || password.isEmpty()) {
        sendErrorReply(err("InvalidArgs"), QStringLiteral("session id, origin, and password are required"));
        return QDBusObjectPath();
    }

    // The caller rules of GetCandidates (spec §6, obligation 9).
    QString reason;
    if (!callerAllowed(request, &reason)) {
        qWarning() << "autofill provider: SaveCredentials rejected:" << reason;
        sendErrorReply(err("OriginMismatch"), reason);
        return QDBusObjectPath();
    }
    const QString origin = Autofill::saveOrigin(requestOrigin);
    if (origin.isEmpty()) {
        qWarning() << "autofill provider: SaveCredentials rejected: origin cannot be saved:" << requestOrigin;
        sendErrorReply(err("OriginMismatch"), QStringLiteral("the origin cannot be saved"));
        return QDBusObjectPath();
    }

    // A hostile app could flood the user with prompts (spec §4.1).
    const QString appId = request.value(QStringLiteral("app-id")).toString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVector<qint64> &times = m_saveTimes[request.value(QStringLiteral("app-signature")).toString() + QLatin1Char('/') + appId];
    while (!times.isEmpty() && times.first() < now - saveRateWindowMs) {
        times.removeFirst();
    }
    if (times.size() >= saveRateLimit) {
        sendErrorReply(err("Busy"), QStringLiteral("too many save requests from this app"));
        return QDBusObjectPath();
    }
    times.append(now);

    SaveRequest save;
    save.id = newId();
    save.sessionId = sessionId;
    save.promptPath = newPrompt();
    save.requestOrigin = requestOrigin;
    save.origin = origin;
    save.appId = appId;
    save.username = username;
    save.email = email;
    save.password = password;
    save.filledToken = request.value(QStringLiteral("filled-token")).toString();
    m_saves.insert(save.id, save);

    // The path is valid at once. The work starts after the reply is sent, so
    // that an immediate Completed signal follows the reply.
    const QString id = save.id;
    QTimer::singleShot(0, this, [this, id]() { startSave(id); });
    QTimer::singleShot(saveTtlMs, this, [this, id]() { expireSave(id); });
    qInfo() << "autofill provider: SaveCredentials for" << origin << "by" << appId;
    return QDBusObjectPath(save.promptPath);
}

void AutofillProvider::startSave(const QString &id)
{
    const auto save = m_saves.find(id);
    if (save == m_saves.end()) {
        return;
    }

    // The user chose "Never for this site" earlier. No prompt.
    if (m_settings->autofillNeverSaveOrigins().contains(save->origin)) {
        finishSave(id, QStringLiteral("never"), true);
        return;
    }

    if (m_status != BitSailorCore::SessionStatusUnlocked || !m_itemsLoaded) {
        // A save needs the unlocked vault. Unlock without a UI, like Unlock().
        // The save goes on when the item list arrives.
        save->state = SaveRequest::WaitingUnlock;
        if (m_status != BitSailorCore::SessionStatusUnlocked) {
            if (!m_unlockRunning) {
                m_unlockRunning = true;
                m_core->unlockVault();
            }
        } else {
            refreshItems();
        }
        return;
    }
    decideSave(id);
}

void AutofillProvider::resumeWaitingSaves()
{
    if (m_status != BitSailorCore::SessionStatusUnlocked || !m_itemsLoaded) {
        return;
    }
    QStringList waiting;
    for (auto it = m_saves.constBegin(); it != m_saves.constEnd(); ++it) {
        if (it->state == SaveRequest::WaitingUnlock) {
            waiting << it.key();
        }
    }
    for (const auto &id : std::as_const(waiting)) {
        decideSave(id);
    }
}

void AutofillProvider::declineWaitingSaves()
{
    QStringList waiting;
    for (auto it = m_saves.constBegin(); it != m_saves.constEnd(); ++it) {
        if (it->state == SaveRequest::WaitingUnlock) {
            waiting << it.key();
        }
    }
    for (const auto &id : std::as_const(waiting)) {
        finishSave(id, QStringLiteral("declined"), true);
    }
}

bool AutofillProvider::entryExists(const QString &itemId) const
{
    for (const auto &entry : std::as_const(m_entries)) {
        if (entry.id == itemId) {
            return true;
        }
    }
    return false;
}

void AutofillProvider::decideSave(const QString &id)
{
    const auto save = m_saves.find(id);
    if (save == m_saves.end()) {
        return;
    }

    // The item that autofill filled comes first. Then an item with the same
    // username and a URI that matches the origin of the request.
    QString target;
    const auto filled = m_filled.constFind(save->filledToken);
    if (!save->filledToken.isEmpty() && filled != m_filled.constEnd()
            && filled->sessionId == save->sessionId && entryExists(filled->itemId)) {
        target = filled->itemId;
    }
    if (target.isEmpty()) {
        for (const auto &entry : std::as_const(m_entries)) {
            if (entry.username.compare(save->username, Qt::CaseInsensitive) != 0) {
                continue;
            }
            for (const auto &uri : entry.uris) {
                if (Autofill::uriMatches(save->requestOrigin, uri.first, uri.second)) {
                    target = entry.id;
                    break;
                }
            }
            if (!target.isEmpty()) {
                break;
            }
        }
    }

    if (target.isEmpty()) {
        save->action = QStringLiteral("create");
        save->itemName = Autofill::itemNameForOrigin(save->origin);
        askSave(id);
        return;
    }

    // Read the item: the same credentials end the save at once, and an update
    // needs the complete item.
    save->itemId = target;
    save->state = SaveRequest::Fetching;
    const bool alreadyFetching = m_pendingResolves.contains(target) || m_saveFetches.contains(target);
    m_saveFetches[target].append(id);
    if (!alreadyFetching) {
        m_core->fetchItem(target);
    }
}

void AutofillProvider::afterSaveFetch(const QString &id, const QJsonObject &item)
{
    const auto save = m_saves.find(id);
    if (save == m_saves.end()) {
        return;
    }
    if (Autofill::hasCredentials(item, save->username, save->password)) {
        finishSave(id, QStringLiteral("unchanged"), false);
        return;
    }
    save->item = item;
    save->action = QStringLiteral("update");
    save->itemName = item.value(QStringLiteral("name")).toString();
    askSave(id);
}

void AutofillProvider::askSave(const QString &id)
{
    const auto save = m_saves.find(id);
    if (save == m_saves.end()) {
        return;
    }
    save->state = SaveRequest::Asking;

    // The UI app shows the prompt. The call carries no password. The UI app
    // answers through the ManualFillBridge object.
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("cz.chrastecky.bitsailor"),
                                                       QStringLiteral("/autofill"),
                                                       QStringLiteral("cz.chrastecky.bitsailor.AutofillPicker"),
                                                       QStringLiteral("ShowSavePrompt"));
    call.setArguments({id, save->origin, save->appId, save->action, save->username, save->itemName});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, manualFillCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, id]() {
        const QDBusPendingReply<> reply = *watcher;
        watcher->deleteLater();
        if (reply.isError()) {
            qWarning() << "autofill provider: could not open the save prompt:" << reply.error().message();
            finishSave(id, QStringLiteral("declined"), true);
        }
    });
}

void AutofillProvider::expireSave(const QString &id)
{
    const auto save = m_saves.constFind(id);
    if (save == m_saves.constEnd()) {
        return;
    }
    // A save that the user accepted ends by itself.
    if (save->state == SaveRequest::WriteQueued || save->state == SaveRequest::Writing
            || save->state == SaveRequest::Refreshing) {
        return;
    }
    finishSave(id, QStringLiteral("declined"), true);
}

QString AutofillProvider::saveAnswered(const QString &requestId, const QString &choice)
{
    const auto save = m_saves.find(requestId);
    if (save == m_saves.end() || save->state != SaveRequest::Asking) {
        return QStringLiteral("unknown or expired request");
    }

    if (choice == QLatin1String("save")) {
        save->state = SaveRequest::WriteQueued;
        m_writeQueue.append(requestId);
        runNextWrite();
    } else if (choice == QLatin1String("never")) {
        m_settings->addAutofillNeverSaveOrigin(save->origin);
        finishSave(requestId, QStringLiteral("never"), true);
    } else if (choice == QLatin1String("decline")) {
        finishSave(requestId, QStringLiteral("declined"), true);
    } else {
        return QStringLiteral("unknown choice");
    }
    return QString();
}

void AutofillProvider::runNextWrite()
{
    // The core reports no request id for a write. One write runs at a time.
    while (m_writeActive.isEmpty() && !m_writeQueue.isEmpty()) {
        const QString id = m_writeQueue.takeFirst();
        const auto save = m_saves.find(id);
        if (save == m_saves.end()) {
            continue;
        }
        save->state = SaveRequest::Writing;
        m_writeActive = id;
        m_writeIsCreate = save->itemId.isEmpty();
        if (m_writeIsCreate) {
            m_core->createItem(Autofill::newLoginItem(save->origin, save->username, save->password, save->email));
        } else {
            m_core->updateItem(save->itemId, Autofill::withCredentials(save->item, save->username, save->password, save->email));
        }
    }
}

void AutofillProvider::onItemCreated(bool success)
{
    if (m_writeActive.isEmpty() || !m_writeIsCreate) {
        return;
    }
    const auto save = m_saves.find(m_writeActive);
    if (!success || save == m_saves.end()) {
        finishWrite(false);
        return;
    }
    // Read the item list again. The new login is a candidate from now on.
    save->state = SaveRequest::Refreshing;
    m_core->fetchItems();
}

void AutofillProvider::onItemUpdated(bool success)
{
    if (m_writeActive.isEmpty() || m_writeIsCreate) {
        return;
    }
    const auto save = m_saves.find(m_writeActive);
    if (!success || save == m_saves.end()) {
        finishWrite(false);
        return;
    }
    save->state = SaveRequest::Refreshing;
    m_core->fetchItems();
}

void AutofillProvider::finishWrite(bool success)
{
    const QString id = m_writeActive;
    const bool created = m_writeIsCreate;
    m_writeActive.clear();
    finishSave(id, success ? (created ? QStringLiteral("saved") : QStringLiteral("updated")) : QStringLiteral("failed"), false);
    if (success) {
        // Spec §6, obligation 13: the candidate lists of all sessions are stale.
        Q_EMIT CredentialsChanged();
    }
    runNextWrite();
}

void AutofillProvider::finishSave(const QString &id, const QString &outcome, bool dismissed)
{
    const auto save = m_saves.find(id);
    if (save == m_saves.end()) {
        return;
    }
    const QString path = save->promptPath;
    save->password.fill(QLatin1Char('\0')); // best effort, other copies may exist
    m_saves.erase(save);
    completePrompt(path, dismissed, QVariantMap{{QStringLiteral("outcome"), outcome}});
}

QVariantMap AutofillProvider::GeneratePassword(const QVariantMap &request, const QVariantMap &policy)
{
    if (request.value(QStringLiteral("origin")).toString().isEmpty()) {
        sendErrorReply(err("InvalidArgs"), QStringLiteral("origin is required"));
        return {};
    }
    QString reason;
    if (!callerAllowed(request, &reason)) {
        qWarning() << "autofill provider: GeneratePassword rejected:" << reason;
        sendErrorReply(err("OriginMismatch"), reason);
        return {};
    }
    Autofill::PasswordPolicy parsed;
    if (!Autofill::parsePasswordPolicy(policy, &parsed)) {
        sendErrorReply(err("PolicyViolation"), QStringLiteral("no character class is enabled"));
        return {};
    }

    // The vault can be locked: generation needs no vault data (spec §4.1).
    GenerateRequest generate;
    generate.message = message();
    generate.policy = parsed;
    m_generateQueue.append(generate);
    setDelayedReply(true);
    runGenerate();
    return {};
}

void AutofillProvider::runGenerate()
{
    // The core reports no request id. One generation runs at a time, so that
    // a password never answers a call with another policy.
    if (m_generateRunning || m_generateQueue.isEmpty()) {
        return;
    }
    m_generateRunning = true;
    const Autofill::PasswordPolicy &policy = m_generateQueue.first().policy;
    m_core->generatePassword(policy.lowercase, policy.uppercase, policy.digits, policy.symbols,
                             false, policy.digits ? 1 : 0, policy.symbols ? 1 : 0, policy.length);
}

void AutofillProvider::onPasswordGenerated(bool success, const QString &password)
{
    if (!m_generateRunning || m_generateQueue.isEmpty()) {
        return;
    }

    GenerateRequest &generate = m_generateQueue.first();
    if (success && Autofill::containsAny(password, generate.policy.excludeChars)
            && ++generate.attempts < generateAttempts) {
        // The core has no exclusion list. Generate again.
        const Autofill::PasswordPolicy &policy = generate.policy;
        m_core->generatePassword(policy.lowercase, policy.uppercase, policy.digits, policy.symbols,
                                 false, policy.digits ? 1 : 0, policy.symbols ? 1 : 0, policy.length);
        return;
    }

    const GenerateRequest done = m_generateQueue.takeFirst();
    m_generateRunning = false;
    if (success && !Autofill::containsAny(password, done.policy.excludeChars)) {
        QDBusConnection::sessionBus().send(done.message.createReply(
            QVariant::fromValue(QVariantMap{{QStringLiteral("password"), password}})));
    } else {
        QDBusConnection::sessionBus().send(done.message.createErrorReply(
            err("PolicyViolation"), QStringLiteral("the password policy cannot be met")));
    }
    runGenerate();
}

void AutofillProvider::onSessionClosed(const QString &sessionId)
{
    // Drop the saves of the session that the user has not accepted yet. A save
    // that is being written ends by itself.
    const QStringList saveIds = m_saves.keys();
    for (const auto &id : saveIds) {
        const auto save = m_saves.constFind(id);
        const bool writing = save->state == SaveRequest::WriteQueued || save->state == SaveRequest::Writing
                || save->state == SaveRequest::Refreshing;
        if (save->sessionId == sessionId && !writing) {
            m_saves.remove(id);
        }
    }
    for (auto it = m_filled.begin(); it != m_filled.end();) {
        if (it->sessionId == sessionId) {
            it = m_filled.erase(it);
        } else {
            ++it;
        }
    }

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

void ManualFillBridge::SaveAnswered(const QString &requestId, const QString &choice)
{
    if (!callerIsUi()) {
        return;
    }
    const QString error = m_provider->saveAnswered(requestId, choice);
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
