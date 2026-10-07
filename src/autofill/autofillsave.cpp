// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

#include "autofillsave.h"

#include <QDateTime>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUrl>

namespace Autofill {

namespace {

constexpr auto appScheme = "app://";

}

QString saveOrigin(const QString &origin)
{
    const QString trimmed = origin.trimmed();
    if (trimmed.startsWith(QLatin1String(appScheme))) {
        const QString id = trimmed.mid(QLatin1String(appScheme).size());
        static const QRegularExpression validId(QStringLiteral("^[A-Za-z0-9._-]+$"));
        return validId.match(id).hasMatch() ? trimmed : QString();
    }

    const QUrl url(trimmed, QUrl::StrictMode);
    const QString scheme = url.scheme();
    if (!url.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
            || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        return QString();
    }

    QString result = scheme + QStringLiteral("://") + url.host().toLower();
    if (url.port() != -1) {
        result += QLatin1Char(':') + QString::number(url.port());
    }
    return result;
}

QString itemNameForOrigin(const QString &savedOrigin)
{
    if (savedOrigin.startsWith(QLatin1String(appScheme))) {
        return savedOrigin.mid(QLatin1String(appScheme).size());
    }
    QString host = QUrl(savedOrigin).host();
    if (host.startsWith(QLatin1String("www."))) {
        host = host.mid(4);
    }
    return host.isEmpty() ? savedOrigin : host;
}

namespace {

// A custom field of the item. Same keys as the fields of the item editor.
QJsonObject textField(const QString &name, const QString &value)
{
    return QJsonObject{{QStringLiteral("type"), 0},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("value"), value},
                       {QStringLiteral("linkedId"), QJsonValue::Null}};
}

bool isEmailField(const QJsonObject &field)
{
    return field.value(QStringLiteral("name")).toString().trimmed().toLower() == QLatin1String("email");
}

QJsonValue stringOrNull(const QString &value)
{
    return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
}

}

QJsonObject newLoginItem(const QString &savedOrigin, const QString &username,
                         const QString &password, const QString &email)
{
    const QJsonObject uri{{QStringLiteral("uri"), savedOrigin},
                          {QStringLiteral("match"), QJsonValue::Null}}; // default match detection
    const QJsonObject login{{QStringLiteral("username"), stringOrNull(username)},
                            {QStringLiteral("password"), password},
                            {QStringLiteral("totp"), QJsonValue::Null},
                            {QStringLiteral("uris"), QJsonArray{uri}}};

    QJsonArray fields;
    if (!email.isEmpty() && email != username) {
        fields.append(textField(QStringLiteral("email"), email));
    }

    return QJsonObject{{QStringLiteral("type"), LoginItemType},
                       {QStringLiteral("name"), itemNameForOrigin(savedOrigin)},
                       {QStringLiteral("notes"), QJsonValue::Null},
                       {QStringLiteral("favorite"), false},
                       {QStringLiteral("folderId"), QJsonValue::Null},
                       {QStringLiteral("collectionIds"), QJsonValue::Null},
                       {QStringLiteral("reprompt"), 0},
                       {QStringLiteral("fields"), fields},
                       {QStringLiteral("login"), login}};
}

QJsonObject withCredentials(const QJsonObject &item, const QString &username,
                            const QString &password, const QString &email)
{
    QJsonObject result = item;
    QJsonObject login = result.value(QStringLiteral("login")).toObject();

    if (login.value(QStringLiteral("password")).toString() != password) {
        login.insert(QStringLiteral("password"), password);
        login.insert(QStringLiteral("passwordRevisionDate"),
                     QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    }
    login.insert(QStringLiteral("username"), stringOrNull(username));
    result.insert(QStringLiteral("login"), login);

    if (!email.isEmpty() && email != username) {
        QJsonArray fields = result.value(QStringLiteral("fields")).toArray();
        bool found = false;
        for (int i = 0; i < fields.size(); ++i) {
            QJsonObject field = fields.at(i).toObject();
            if (isEmailField(field)) {
                field.insert(QStringLiteral("value"), email);
                fields.replace(i, field);
                found = true;
                break;
            }
        }
        if (!found) {
            fields.append(textField(QStringLiteral("email"), email));
        }
        result.insert(QStringLiteral("fields"), fields);
    }
    return result;
}

bool hasCredentials(const QJsonObject &item, const QString &username, const QString &password)
{
    const QJsonObject login = item.value(QStringLiteral("login")).toObject();
    return login.value(QStringLiteral("username")).toString() == username
            && login.value(QStringLiteral("password")).toString() == password;
}

namespace {

// The character sets of the core generator (core/generate_password.go).
const QString lowercaseChars = QStringLiteral("abcdefghijklmnopqrstuvwxyz");
const QString uppercaseChars = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ");
const QString digitChars = QStringLiteral("0123456789");
const QString symbolChars = QStringLiteral("!@#$%^&*");

// True when the exclude list holds every character of the set.
bool coversAll(const QString &exclude, const QString &set)
{
    for (const QChar c : set) {
        if (!exclude.contains(c)) {
            return false;
        }
    }
    return true;
}

}

bool parsePasswordPolicy(const QVariantMap &policy, PasswordPolicy *out)
{
    const auto flag = [&policy](const char *key) {
        const QString name = QLatin1String(key);
        return policy.contains(name) ? policy.value(name).toBool() : true;
    };

    PasswordPolicy result;
    result.excludeChars = policy.value(QStringLiteral("exclude-chars")).toString();
    // The core puts one character of every enabled class into the password.
    // A class that the exclude list covers fully cannot appear. Switch it off.
    result.lowercase = flag("lowercase") && !coversAll(result.excludeChars, lowercaseChars);
    result.uppercase = flag("uppercase") && !coversAll(result.excludeChars, uppercaseChars);
    result.digits = flag("digits") && !coversAll(result.excludeChars, digitChars);
    result.symbols = flag("symbols") && !coversAll(result.excludeChars, symbolChars);
    const int classes = int(result.lowercase) + int(result.uppercase) + int(result.digits) + int(result.symbols);
    if (classes == 0) {
        return false;
    }

    int length = PasswordPolicy::DefaultLength;
    const QVariant requested = policy.value(QStringLiteral("length"));
    if (requested.isValid() && requested.toUInt() > 0) {
        length = int(qMin(requested.toUInt(), uint(PasswordPolicy::MaximumLength)));
    }
    result.length = qBound(qMax(PasswordPolicy::MinimumLength, classes), length, int(PasswordPolicy::MaximumLength));
    *out = result;
    return true;
}

bool containsAny(const QString &password, const QString &chars)
{
    for (const QChar c : chars) {
        if (password.contains(c)) {
            return true;
        }
    }
    return false;
}

}
