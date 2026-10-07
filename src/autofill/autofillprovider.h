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

#include "../bitsailorcore.h"
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
    explicit AutofillProvider(BitSailorCore *core, QObject *parent = nullptr);

    QString displayName() const;
    uint version() const;

    // Read the session state and unlock the vault.
    void start();

public Q_SLOTS:
    AutofillCandidatesReply GetCandidates(const QVariantMap &request);
    QVariantMap Resolve(const QString &token);
    QDBusObjectPath RequestManualFill(const QVariantMap &options);
    QDBusObjectPath Unlock();

Q_SIGNALS:
    void CredentialsChanged();

private Q_SLOTS:
    void onSessionClosed(const QString &sessionId);
    void onLoginStatus(BitSailorCore::SessionStatus status);
    void onUnlockFinished(bool success, const QString &error);
    void onItemsResolved(const QJsonArray &items);
    void onItemFetched(bool success, const QJsonObject &item);
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

    bool callerAllowed(const QVariantMap &request, QString *reason) const;
    QString newPrompt();
    void completePrompt(const QString &path, bool dismissed);
    void pruneExpired();

    BitSailorCore *m_core;
    BitSailorCore::SessionStatus m_status = BitSailorCore::SessionStatusNone;
    bool m_unlockRunning = false;
    bool m_itemsLoaded = false;
    QVector<Entry> m_entries;
    QHash<QString, Token> m_tokens;
    QHash<QString, Ticket> m_tickets;
    QHash<QString, QVector<QDBusMessage>> m_pendingResolves; // item id -> waiting calls
    QStringList m_unlockPrompts;
    QSet<QString> m_allowedKeyIds; // empty: accept every signed caller
    QTimer m_refreshTimer;
    int m_promptCounter = 0;
};

#endif // AUTOFILLPROVIDER_H
