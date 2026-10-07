// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT
//
// Unit test of the helpers in src/autofill/autofillsave.cpp.

#include <QCoreApplication>
#include <QJsonArray>
#include <iostream>

#include "autofillsave.h"

static int failures = 0;
static void check(bool ok, const char *what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << std::endl;
    if (!ok) ++failures;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace Autofill;

    // saveOrigin
    check(saveOrigin(QStringLiteral("https://Example.COM/login?x=1#top")) == QLatin1String("https://example.com"),
          "web origin: path, query, fragment go; host is lower case");
    check(saveOrigin(QStringLiteral("https://example.com:8443/a")) == QLatin1String("https://example.com:8443"),
          "web origin keeps an explicit port");
    check(saveOrigin(QStringLiteral("http://example.com")) == QLatin1String("http://example.com"), "http origin is valid");
    check(saveOrigin(QStringLiteral("app://org.example.app")) == QLatin1String("app://org.example.app"), "app origin stays");
    check(saveOrigin(QStringLiteral("ftp://example.com")).isEmpty(), "ftp origin is invalid");
    check(saveOrigin(QStringLiteral("https://")).isEmpty(), "origin without host is invalid");
    check(saveOrigin(QStringLiteral("example.com")).isEmpty(), "origin without scheme is invalid");
    check(saveOrigin(QStringLiteral("https://user:pw@example.com")).isEmpty(), "origin with user info is invalid");
    check(saveOrigin(QStringLiteral("app://")).isEmpty(), "app origin without id is invalid");
    check(saveOrigin(QStringLiteral("app://bad id")).isEmpty(), "app id with a space is invalid");
    check(saveOrigin(QString()).isEmpty(), "empty origin is invalid");

    // itemNameForOrigin
    check(itemNameForOrigin(QStringLiteral("https://www.example.com")) == QLatin1String("example.com"), "name drops www.");
    check(itemNameForOrigin(QStringLiteral("https://login.example.com:8443")) == QLatin1String("login.example.com"), "name is the host");
    check(itemNameForOrigin(QStringLiteral("app://org.example.app")) == QLatin1String("org.example.app"), "name of an app is its id");

    // newLoginItem
    const QJsonObject created = newLoginItem(QStringLiteral("https://example.com"), QStringLiteral("alice"),
                                             QStringLiteral("pw"), QString());
    const QJsonObject login = created.value("login").toObject();
    check(created.value("type").toInt() == LoginItemType && created.value("name").toString() == QLatin1String("example.com"),
          "new item: login type and name");
    check(login.value("username").toString() == QLatin1String("alice") && login.value("password").toString() == QLatin1String("pw"),
          "new item: username and password");
    const QJsonObject uri = login.value("uris").toArray().at(0).toObject();
    check(uri.value("uri").toString() == QLatin1String("https://example.com") && uri.value("match").isNull(),
          "new item: one URI with default match detection");
    check(created.value("fields").toArray().isEmpty(), "new item: no email field when no email");
    check(created.value("folderId").isNull() && created.value("favorite").toBool() == false, "new item: no folder, no favorite");

    const QJsonObject withMail = newLoginItem(QStringLiteral("https://example.com"), QStringLiteral("alice"),
                                              QStringLiteral("pw"), QStringLiteral("alice@example.net"));
    const QJsonObject mailField = withMail.value("fields").toArray().at(0).toObject();
    check(mailField.value("name").toString() == QLatin1String("email") && mailField.value("value").toString() == QLatin1String("alice@example.net")
          && mailField.value("type").toInt() == 0, "new item: email in a custom text field named email");
    check(newLoginItem(QStringLiteral("https://example.com"), QStringLiteral("a@example.com"), QStringLiteral("pw"),
                       QStringLiteral("a@example.com")).value("fields").toArray().isEmpty(),
          "new item: no email field when email equals username");
    check(newLoginItem(QStringLiteral("https://example.com"), QString(), QStringLiteral("pw"), QString())
          .value("login").toObject().value("username").isNull(), "new item: empty username is null");

    // withCredentials
    QJsonObject existing{{"id", "abc"}, {"name", "Example"}, {"favorite", true}, {"folderId", "folder-1"},
                         {"fields", QJsonArray{QJsonObject{{"type", 1}, {"name", "pin"}, {"value", "1234"}}}},
                         {"login", QJsonObject{{"username", "alice"}, {"password", "old"}, {"totp", "otpauth://x"},
                                               {"uris", QJsonArray{QJsonObject{{"uri", "https://example.com"}, {"match", 1}}}}}}};
    const QJsonObject changed = withCredentials(existing, QStringLiteral("alice"), QStringLiteral("new"), QString());
    const QJsonObject changedLogin = changed.value("login").toObject();
    check(changedLogin.value("password").toString() == QLatin1String("new"), "update: new password");
    check(!changedLogin.value("passwordRevisionDate").toString().isEmpty(), "update: password revision date set");
    check(changedLogin.value("totp").toString() == QLatin1String("otpauth://x")
          && changedLogin.value("uris").toArray().at(0).toObject().value("match").toInt() == 1
          && changed.value("favorite").toBool() && changed.value("folderId").toString() == QLatin1String("folder-1")
          && changed.value("fields").toArray().size() == 1,
          "update: totp, URIs, folder, favorite, and other fields stay");
    const QJsonObject same = withCredentials(existing, QStringLiteral("alice2"), QStringLiteral("old"), QString());
    check(same.value("login").toObject().value("username").toString() == QLatin1String("alice2")
          && !same.value("login").toObject().contains("passwordRevisionDate"),
          "update: a new username alone leaves the revision date");
    const QJsonObject mailed = withCredentials(existing, QStringLiteral("alice"), QStringLiteral("old"), QStringLiteral("a@example.net"));
    check(mailed.value("fields").toArray().size() == 2 && mailed.value("fields").toArray().at(1).toObject().value("name").toString() == QLatin1String("email"),
          "update: email adds a field");
    const QJsonObject mailedAgain = withCredentials(mailed, QStringLiteral("alice"), QStringLiteral("old"), QStringLiteral("b@example.net"));
    check(mailedAgain.value("fields").toArray().size() == 2 && mailedAgain.value("fields").toArray().at(1).toObject().value("value").toString() == QLatin1String("b@example.net"),
          "update: email changes the existing email field");
    check(hasCredentials(existing, QStringLiteral("alice"), QStringLiteral("old")), "hasCredentials: equal");
    check(!hasCredentials(existing, QStringLiteral("alice"), QStringLiteral("other")), "hasCredentials: other password");

    // parsePasswordPolicy
    PasswordPolicy policy;
    check(parsePasswordPolicy({}, &policy) && policy.lowercase && policy.uppercase && policy.digits && policy.symbols
          && policy.length == PasswordPolicy::DefaultLength, "policy: defaults");
    check(parsePasswordPolicy({{"length", 24u}, {"symbols", false}}, &policy) && policy.length == 24 && !policy.symbols, "policy: length and a class off");
    check(parsePasswordPolicy({{"length", 3u}}, &policy) && policy.length == 8, "policy: short length is raised to 8");
    check(parsePasswordPolicy({{"length", 5000u}}, &policy) && policy.length == 128, "policy: long length is cut to 128");
    check(parsePasswordPolicy({{"length", 0u}}, &policy) && policy.length == PasswordPolicy::DefaultLength, "policy: length 0 gives the default");
    check(parsePasswordPolicy({{"exclude-chars", "il1"}}, &policy) && policy.excludeChars == QLatin1String("il1"), "policy: exclude-chars");
    check(parsePasswordPolicy({{"exclude-chars", "abcdefghijklmnopqrstuvwxyz"}}, &policy) && !policy.lowercase
          && policy.uppercase && policy.digits && policy.symbols,
          "policy: an exclude list that covers a class switches the class off");
    check(parsePasswordPolicy({{"exclude-chars", "abcdefghijklmnopqrstuvwxy"}}, &policy) && policy.lowercase,
          "policy: a class that the exclude list covers in part stays on");
    check(!parsePasswordPolicy({{"exclude-chars", "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*"}}, &policy),
          "policy: an exclude list that covers all classes is refused");
    check(!parsePasswordPolicy({{"lowercase", false}, {"uppercase", false}, {"digits", false}, {"symbols", false}}, &policy),
          "policy: no class left is refused");

    // containsAny
    check(containsAny(QStringLiteral("abc"), QStringLiteral("xc")) && !containsAny(QStringLiteral("abc"), QStringLiteral("xyz"))
          && !containsAny(QStringLiteral("abc"), QString()), "containsAny");

    std::cout << (failures ? "\nsome checks failed" : "\nall checks passed") << std::endl;
    return failures ? 1 : 0;
}
