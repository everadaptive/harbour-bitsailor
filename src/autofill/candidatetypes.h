/*
    SPDX-FileCopyrightText: 2026 Zach Dunton
    SPDX-License-Identifier: MIT
*/

#pragma once

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QMetaType>
#include <QVariantMap>

// D-Bus struct (s, a{sv}): (token, metadata)
struct AutofillCandidate
{
    QString token;
    QVariantMap metadata;
};

typedef QList<AutofillCandidate> AutofillCandidateList;

// D-Bus struct (a(sa{sv}), a{sv}): GetCandidates reply.
// response MAY carry "manual-fill-ticket" (spec §4).
struct AutofillCandidatesReply
{
    AutofillCandidateList candidates;
    QVariantMap response;
};

inline QDBusArgument &operator<<(QDBusArgument &arg, const AutofillCandidate &candidate)
{
    arg.beginStructure();
    arg << candidate.token << candidate.metadata;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, AutofillCandidate &candidate)
{
    arg.beginStructure();
    arg >> candidate.token >> candidate.metadata;
    arg.endStructure();
    return arg;
}

inline QDBusArgument &operator<<(QDBusArgument &arg, const AutofillCandidatesReply &reply)
{
    arg.beginStructure();
    arg << reply.candidates << reply.response;
    arg.endStructure();
    return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg, AutofillCandidatesReply &reply)
{
    arg.beginStructure();
    arg >> reply.candidates >> reply.response;
    arg.endStructure();
    return arg;
}

inline void registerAutofillTypes()
{
    qDBusRegisterMetaType<AutofillCandidate>();
    qDBusRegisterMetaType<AutofillCandidateList>();
    qDBusRegisterMetaType<AutofillCandidatesReply>();
}

// Qt5 needs these macros. Qt6 registers the types automatically.
Q_DECLARE_METATYPE(AutofillCandidate)
Q_DECLARE_METATYPE(AutofillCandidateList)
Q_DECLARE_METATYPE(AutofillCandidatesReply)
