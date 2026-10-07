// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

#ifndef AUTOFILLPROVIDER_H
#define AUTOFILLPROVIDER_H

#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "../appsettings.h"
#include "../bitsailorcore.h"
#include "autofillsave.h"
#include "candidatetypes.h"

// Implements tech.everadaptive.Autofill1.Provider on top of the BitSailor
// core. The object runs in a headless process. It imports the session
// that the UI process stored in the Sailfish secrets store, and it
// unlocks the vault with the stored user key.
class AutofillProvider : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "tech.everadaptive.Autofill1.Provider")

    // Exported by ProviderAdaptor
    Q_PROPERTY(QString DisplayName READ displayName CONSTANT)
    Q_PROPERTY(uint Version READ version CONSTANT)

public:
    AutofillProvider(BitSailorCore *core, AppSettings *settings, QObject *parent = nullptr);

    QString displayName() const;
    uint version() const;

    // Read the session state and unlock the vault.
    void start();

    // Called by the manual fill bridge when the UI app answers. Both return
    // an empty string on success and an error code otherwise.
    QString manualFillSelected(const QString &requestId, const QString &itemId);
    QString manualFillCancelled(const QString &requestId);

    // Called by the manual fill bridge when the UI app answers a save prompt.
    // choice is "save", "decline", or "never". Return an empty string on
    // success and an error text otherwise.
    QString saveAnswered(const QString &requestId, const QString &choice);

public Q_SLOTS:
    AutofillCandidatesReply GetCandidates(const QVariantMap &request);
    QVariantMap Resolve(const QString &token);
    QDBusObjectPath RequestManualFill(const QVariantMap &options);
    QDBusObjectPath Unlock();
    // Protocol v2 (spec §4.1)
    QDBusObjectPath SaveCredentials(const QVariantMap &request, const QVariantMap &credentials);
    QVariantMap GeneratePassword(const QVariantMap &request, const QVariantMap &policy);

Q_SIGNALS:
    void CredentialsChanged();

private Q_SLOTS:
    void onSessionClosed(const QString &sessionId);
    void onLoginStatus(BitSailorCore::SessionStatus status);
    void onUnlockFinished(bool success, const QString &error);
    void onItemsResolved(const QJsonArray &items);
    void onItemFetched(bool success, const QJsonObject &item);
    void onItemsFailed();
    void onItemCreated(bool success);
    void onItemUpdated(bool success);
    void onPasswordGenerated(bool success, const QString &password);
    void refreshItems();

private:
    struct Entry
    {
        QString id;
        QString name;
        QString username;
        QVector<QPair<QString, int>> uris; // uri, match type
    };
    struct Token
    {
        QString sessionId;
        QString itemId;
        qint64 expiryMs = 0;
    };
    struct Ticket
    {
        QString sessionId;
        qint64 expiryMs = 0;
    };
    // A manual fill waits for the UI app to return the user's choice.
    struct ManualFill
    {
        QString sessionId;
        QString promptPath;
    };
    // A token that a Resolve consumed. A later save names the filled item
    // with it (spec §4.1, filled-token).
    struct Filled
    {
        QString sessionId;
        QString itemId;
        qint64 expiryMs = 0;
    };
    // One SaveCredentials call, from the call to the end of the prompt. The
    // request holds the password in memory until the prompt completes. The
    // password never goes to the UI app.
    struct SaveRequest
    {
        enum State {
            WaitingUnlock, // the vault is locked or the items are not loaded
            Fetching,      // the matching item is being read
            Asking,        // the UI app shows the prompt
            WriteQueued,   // the user accepted, another write runs
            Writing,       // the core writes the item
            Refreshing,    // the item list is being read again
        };
        QString id;
        QString sessionId;
        QString promptPath;
        QString requestOrigin; // as the app sent it, for matching vault URIs
        QString origin;        // as a new item stores it, see Autofill::saveOrigin
        QString appId;
        QString username;      // the email when the app sent no username
        QString email;
        QString password;
        QString filledToken;
        QString itemId;        // the item to update, empty for a create
        QString itemName;
        QString action;        // "create" or "update"
        QJsonObject item;      // the item to update, as the core returned it
        State state = WaitingUnlock;
    };
    // One GeneratePassword call. The core library reports no request id, so
    // the calls run one at a time.
    struct GenerateRequest
    {
        QDBusMessage message;
        Autofill::PasswordPolicy policy;
        int attempts = 0;
    };

    bool callerAllowed(const QVariantMap &request, QString *reason) const;
    QString newPrompt();
    void completePrompt(const QString &path, bool dismissed, const QVariant &result = QString());
    void expireManualFill(const QString &requestId);
    void pruneExpired();

    void startSave(const QString &id);
    void decideSave(const QString &id);
    void afterSaveFetch(const QString &id, const QJsonObject &item);
    void askSave(const QString &id);
    void expireSave(const QString &id);
    void runNextWrite();
    void finishWrite(bool success);
    void finishSave(const QString &id, const QString &outcome, bool dismissed);
    void resumeWaitingSaves();
    void declineWaitingSaves();
    bool entryExists(const QString &itemId) const;
    void runGenerate();

    BitSailorCore *m_core;
    AppSettings *m_settings;
    BitSailorCore::SessionStatus m_status = BitSailorCore::SessionStatusNone;
    bool m_unlockRunning = false;
    bool m_itemsLoaded = false;
    QVector<Entry> m_entries;
    QHash<QString, Token> m_tokens;
    QHash<QString, Ticket> m_tickets;
    QHash<QString, QVector<QDBusMessage>> m_pendingResolves; // item id -> waiting calls
    QHash<QString, ManualFill> m_manualFills;                 // request id -> waiting prompt
    QStringList m_unlockPrompts;
    QHash<QString, Filled> m_filled;                          // consumed token -> item
    QHash<QString, SaveRequest> m_saves;                      // request id -> save
    QHash<QString, QStringList> m_saveFetches;                // item id -> waiting saves
    QStringList m_writeQueue;                                 // accepted saves, in order
    QString m_writeActive;                                    // the save that the core writes
    bool m_writeIsCreate = false;
    QHash<QString, QVector<qint64>> m_saveTimes;              // app -> times of recent saves
    QVector<GenerateRequest> m_generateQueue;
    bool m_generateRunning = false;
    QSet<QString> m_allowedKeyIds; // empty: accept every signed caller
    QTimer m_refreshTimer;
    int m_promptCounter = 0;
};

// D-Bus object for the BitSailor UI app. The UI app reports the login that
// the user picked for a manual fill. Only the UI app may call it.
class ManualFillBridge : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "cz.chrastecky.bitsailor.AutofillControl")

public:
    explicit ManualFillBridge(AutofillProvider *provider, QObject *parent = nullptr);

public Q_SLOTS:
    void Selected(const QString &requestId, const QString &itemId);
    void Cancelled(const QString &requestId);
    void SaveAnswered(const QString &requestId, const QString &choice);

private:
    bool callerIsUi();

    AutofillProvider *m_provider;
};

#endif // AUTOFILLPROVIDER_H
