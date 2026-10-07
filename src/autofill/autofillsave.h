// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

#ifndef AUTOFILL_AUTOFILLSAVE_H
#define AUTOFILL_AUTOFILLSAVE_H

#include <QJsonObject>
#include <QString>
#include <QVariantMap>

// Helpers for SaveCredentials and GeneratePassword (protocol v2). They need
// only Qt Core, so a host test can run them.

namespace Autofill {

// Item type of a login in the core library (BitwardenItemTypeLogin).
constexpr int LoginItemType = 1;

// Return the origin in the form that a new vault item stores, or an empty
// string when the origin cannot be saved.
//  - A web origin gives scheme://host[:port]. Path, query, and fragment go.
//    Only http and https count. User info makes the origin invalid.
//  - An app origin app://<id> stays as it is. The id has only letters,
//    digits, dots, underscores, and hyphens.
QString saveOrigin(const QString &origin);

// Return the name of a new item for a saved origin: the host without a
// leading "www.", or the app id.
QString itemNameForOrigin(const QString &savedOrigin);

// Return a new login item for the core. An email that differs from the
// username goes into a custom field named "email" (see Resolve).
QJsonObject newLoginItem(const QString &savedOrigin, const QString &username,
                         const QString &password, const QString &email);

// Return a copy of an existing item with new credentials. The password
// revision date changes only when the password changes. A non-empty email
// that differs from the username sets the custom field "email". All other
// parts of the item stay as they are.
QJsonObject withCredentials(const QJsonObject &item, const QString &username,
                            const QString &password, const QString &email);

// Return true when the item holds exactly these credentials.
bool hasCredentials(const QJsonObject &item, const QString &username, const QString &password);

struct PasswordPolicy
{
    bool lowercase = true;
    bool uppercase = true;
    bool digits = true;
    bool symbols = true;
    int length = DefaultLength;
    QString excludeChars;

    static constexpr int DefaultLength = 20;
    static constexpr int MinimumLength = 8;
    static constexpr int MaximumLength = 128;
};

// Read the policy dictionary of GeneratePassword (spec §4.1). Return false
// when no character class is left. The length is clamped to 8..128.
bool parsePasswordPolicy(const QVariantMap &policy, PasswordPolicy *out);

// Return true when the password has one of the characters.
bool containsAny(const QString &password, const QString &chars);

}

#endif // AUTOFILL_AUTOFILLSAVE_H
