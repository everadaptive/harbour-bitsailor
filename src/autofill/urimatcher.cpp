// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

#include "urimatcher.h"

#include <QRegularExpression>
#include <QSet>
#include <QUrl>

namespace Autofill {

namespace {

constexpr auto appScheme = "app://";

int portOf(const QUrl &url)
{
    if (url.port() != -1) {
        return url.port();
    }
    return url.scheme() == QLatin1String("http") ? 80 : 443;
}

// A vault URI can lack a scheme, for example "example.com/login".
QUrl parseItemUri(const QString &uri)
{
    const QString trimmed = uri.trimmed();
    if (trimmed.contains(QLatin1String("://"))) {
        return QUrl(trimmed);
    }
    return QUrl(QStringLiteral("https://") + trimmed);
}

// Compare URLs by scheme, host, port, path and query. An empty path and
// "/" are the same, and a trailing slash does not count.
QString normalized(const QUrl &url)
{
    QString path = url.path();
    while (path.size() > 1 && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    if (path.isEmpty()) {
        path = QStringLiteral("/");
    }
    return QStringLiteral("%1://%2:%3%4?%5")
            .arg(url.scheme(), url.host().toLower())
            .arg(portOf(url))
            .arg(path, url.query());
}

}

QString baseDomain(const QString &host)
{
    const QString lower = host.toLower();
    // An IPv4 or IPv6 literal has no registrable domain.
    static const QRegularExpression ipv4(QStringLiteral("^\\d{1,3}(\\.\\d{1,3}){3}$"));
    if (ipv4.match(lower).hasMatch() || lower.contains(QLatin1Char(':'))) {
        return lower;
    }

    const QStringList labels = lower.split(QLatin1Char('.'), QString::SkipEmptyParts);
    if (labels.size() <= 2) {
        return lower;
    }

    static const QSet<QString> secondLevel = {
        QStringLiteral("co"), QStringLiteral("com"), QStringLiteral("org"), QStringLiteral("net"),
        QStringLiteral("gov"), QStringLiteral("edu"), QStringLiteral("ac"),
    };
    const QString &tld = labels.at(labels.size() - 1);
    const QString &second = labels.at(labels.size() - 2);
    const int keep = (tld.size() == 2 && secondLevel.contains(second)) ? 3 : 2;
    return labels.mid(labels.size() - keep).join(QLatin1Char('.'));
}

bool uriMatches(const QString &origin, const QString &itemUri, int matchType)
{
    if (origin.isEmpty() || itemUri.trimmed().isEmpty() || matchType == MatchNever) {
        return false;
    }

    const bool originIsApp = origin.startsWith(QLatin1String(appScheme));
    const bool itemIsApp = itemUri.trimmed().startsWith(QLatin1String(appScheme));
    if (originIsApp || itemIsApp) {
        return originIsApp && itemIsApp && origin == itemUri.trimmed();
    }

    const QUrl originUrl(origin);
    const QString originScheme = originUrl.scheme();
    if ((originScheme != QLatin1String("http") && originScheme != QLatin1String("https"))
            || originUrl.host().isEmpty()) {
        return false;
    }

    // A regular expression is not a URL. Test it before the URL checks.
    if (matchType == MatchRegex) {
        const QRegularExpression expression(itemUri.trimmed());
        return expression.isValid() && expression.match(origin).hasMatch();
    }

    const QUrl item = parseItemUri(itemUri);
    if (item.host().isEmpty()) {
        return false;
    }
    if (originScheme == QLatin1String("http") && item.scheme() == QLatin1String("https")) {
        return false;
    }

    switch (matchType) {
    case MatchHost:
        return originUrl.host().toLower() == item.host().toLower() && portOf(originUrl) == portOf(item);
    case MatchStartsWith:
        return origin.startsWith(itemUri.trimmed());
    case MatchExact:
        return normalized(originUrl) == normalized(item);
    case MatchDomain:
    default:
        return baseDomain(originUrl.host()) == baseDomain(item.host());
    }
}

}
