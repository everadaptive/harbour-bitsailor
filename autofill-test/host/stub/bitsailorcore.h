// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT
//
// A stand-in for BitSailorCore, for the host test of the autofill provider.
// The real class needs the Go core library and the Sailfish secrets stack.
// This stub keeps the vault in memory and emits the same signals as the
// real class, one event loop turn after the call, like the real worker
// threads do. The environment sets its behavior:
//   STUB_LOCKED=1        start with a locked vault (unlockVault() unlocks it)
//   STUB_UNLOCK_FAIL=1   unlockVault() fails
//   STUB_UNLOCK_DELAY_MS the time that unlockVault() takes (default 10)
//   STUB_WRITE_FAIL=1    createItem() and updateItem() fail

#ifndef BITSAILORCORE_H
#define BITSAILORCORE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QRandomGenerator>
#include <QTimer>
#include <QUuid>

class BitSailorCore : public QObject
{
    Q_OBJECT
public:
    enum SessionStatus {
        SessionStatusUnlocked = 0,
        SessionStatusLocked = 1,
        SessionStatusNone = 2,
    };
    enum ItemType {
        ItemTypeLogin = 1,
    };
    Q_ENUM(SessionStatus)

    explicit BitSailorCore(QObject *parent = nullptr) : QObject(parent)
    {
        m_locked = qEnvironmentVariableIntValue("STUB_LOCKED") == 1;
        addLogin(QStringLiteral("Example"), QStringLiteral("alice@example.com"), QStringLiteral("alice-pw"),
                 QStringLiteral("https://example.com"));
    }

    void getLoginStatus()
    {
        later([this]() { Q_EMIT loginStatusFetched(m_locked ? SessionStatusLocked : SessionStatusUnlocked); });
    }
    void unlockVault()
    {
        const int delay = qEnvironmentVariableIsSet("STUB_UNLOCK_DELAY_MS")
                ? qEnvironmentVariableIntValue("STUB_UNLOCK_DELAY_MS") : 10;
        QTimer::singleShot(delay, this, [this]() {
            const bool fail = qEnvironmentVariableIntValue("STUB_UNLOCK_FAIL") == 1;
            if (!fail) {
                m_locked = false;
            }
            Q_EMIT unlockFinished(!fail, fail ? QStringLiteral("stub: unlock failed") : QString());
        });
    }
    void fetchItems()
    {
        later([this]() {
            if (m_locked) {
                Q_EMIT itemResolvingFailed();
            } else {
                Q_EMIT itemsResolved(m_items);
            }
        });
    }
    void fetchItem(const QString &id)
    {
        later([this, id]() {
            for (const auto &value : m_items) {
                if (value.toObject().value("id").toString() == id) {
                    Q_EMIT itemFetchFinished(true, value.toObject());
                    return;
                }
            }
            Q_EMIT itemFetchFinished(false, {});
        });
    }
    void createItem(const QJsonObject &item)
    {
        later([this, item]() {
            if (qEnvironmentVariableIntValue("STUB_WRITE_FAIL") == 1) {
                Q_EMIT itemCreationFinished(false);
                return;
            }
            QJsonObject stored = item;
            stored.insert("id", newId());
            stored.insert("deletedDate", QJsonValue::Null);
            m_items.append(stored);
            Q_EMIT itemCreationFinished(true);
        });
    }
    void updateItem(const QString &id, const QJsonObject &item)
    {
        later([this, id, item]() {
            if (qEnvironmentVariableIntValue("STUB_WRITE_FAIL") == 1) {
                Q_EMIT itemUpdated(false);
                return;
            }
            for (int i = 0; i < m_items.size(); ++i) {
                if (m_items.at(i).toObject().value("id").toString() == id) {
                    QJsonObject stored = item;
                    stored.insert("id", id);
                    m_items.replace(i, stored);
                    Q_EMIT itemUpdated(true);
                    return;
                }
            }
            Q_EMIT itemUpdated(false);
        });
    }
    void generatePassword(bool lowercase, bool uppercase, bool numbers, bool special, bool avoidAmbiguous,
                          int minimumNumbers, int minimumSpecial, int length)
    {
        Q_UNUSED(avoidAmbiguous)
        Q_UNUSED(minimumNumbers)
        Q_UNUSED(minimumSpecial)
        later([=]() {
            QString all;
            if (lowercase) all += QStringLiteral("abcdefghijklmnopqrstuvwxyz");
            if (uppercase) all += QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
            if (numbers) all += QStringLiteral("0123456789");
            if (special) all += QStringLiteral("!@#$%^&*");
            if (all.isEmpty() || length < 1) {
                Q_EMIT passwordGeneratingFinished(false, QString());
                return;
            }
            QString out;
            for (int i = 0; i < length; ++i) {
                out += all.at(int(QRandomGenerator::global()->bounded(quint32(all.size()))));
            }
            Q_EMIT passwordGeneratingFinished(true, out);
        });
    }

    // Test access
    QJsonArray items() const { return m_items; }

Q_SIGNALS:
    void loginStatusFetched(SessionStatus status);
    void unlockFinished(bool success, const QString &error);
    void itemsResolved(const QJsonArray &items);
    void itemResolvingFailed();
    void itemFetchFinished(bool success, const QJsonObject &item);
    void itemCreationFinished(bool success);
    void itemUpdated(bool success);
    void passwordGeneratingFinished(bool success, const QString &password);

private:
    template <typename F>
    void later(F f)
    {
        QTimer::singleShot(10, this, f);
    }
    static QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    void addLogin(const QString &name, const QString &username, const QString &password, const QString &uri)
    {
        m_items.append(QJsonObject{
            {"id", newId()}, {"type", ItemTypeLogin}, {"name", name}, {"deletedDate", QJsonValue::Null},
            {"fields", QJsonArray()},
            {"login", QJsonObject{{"username", username}, {"password", password},
                                  {"uris", QJsonArray{QJsonObject{{"uri", uri}, {"match", -1}}}}}}});
    }

    QJsonArray m_items;
    bool m_locked = false;
};

#endif // BITSAILORCORE_H
