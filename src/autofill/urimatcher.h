// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

#ifndef AUTOFILL_URIMATCHER_H
#define AUTOFILL_URIMATCHER_H

#include <QString>

namespace Autofill {

// Bitwarden URI match types. Same values as the core library.
enum UriMatch {
    MatchDefault = -1,
    MatchDomain = 0,
    MatchHost = 1,
    MatchStartsWith = 2,
    MatchExact = 3,
    MatchRegex = 4,
    MatchNever = 5,
};

// Return the registrable part of a host name. This is an approximation of
// the public suffix list: the last two labels, or the last three labels
// for a two-letter country code under a known second-level name.
QString baseDomain(const QString &host);

// Return true when a vault item URI matches the origin of an autofill
// request.
//  - app://<id> origins match only the identical app:// item URI.
//  - Web origins never match an app:// URI.
//  - An http origin never matches an https item URI. This stops a
//    credential leaving over a plain connection.
bool uriMatches(const QString &origin, const QString &itemUri, int matchType);

}

#endif // AUTOFILL_URIMATCHER_H
