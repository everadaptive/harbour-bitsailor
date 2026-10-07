// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT
//
// A stand-in for the UI app of BitSailor. It runs the real
// AutofillPickerService and answers a save prompt like a user. It reads the
// answer ("save", "decline", or "never") from the file UI_ANSWER_FILE at the
// time of the prompt, and it appends one line for each prompt to UI_LOG.
// The line has the request fields. It never has a password: the provider
// sends none.

#include <QCoreApplication>
#include <QDBusConnection>
#include <QFile>
#include <QTextStream>
#include <QTimer>

#include "autofillpickerservice.h"

static QString readFirstLine(const QString &path, const QString &fallback)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fallback;
    }
    const QString line = QString::fromUtf8(file.readLine()).trimmed();
    return line.isEmpty() ? fallback : line;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QString answerFile = qEnvironmentVariable("UI_ANSWER_FILE");
    const QString logFile = qEnvironmentVariable("UI_LOG");

    auto picker = new AutofillPickerService(&app);
    QObject::connect(picker, &AutofillPickerService::savePromptRequested, picker, [picker, answerFile, logFile]() {
        const QVariantMap request = picker->takePendingSave();
        QFile log(logFile);
        if (log.open(QIODevice::Append)) {
            QTextStream out(&log);
            out << request.value("action").toString() << '|' << request.value("origin").toString() << '|'
                << request.value("appId").toString() << '|' << request.value("username").toString() << '|'
                << request.value("itemName").toString() << '\n';
        }
        const QString answer = readFirstLine(answerFile, QStringLiteral("save"));
        const QString requestId = request.value("requestId").toString();
        QTimer::singleShot(20, picker, [picker, requestId, answer]() { picker->answerSave(requestId, answer); });
    });

    QDBusConnection::sessionBus().registerObject(QStringLiteral("/autofill"), picker, QDBusConnection::ExportAllSlots);
    if (!QDBusConnection::sessionBus().registerService(QStringLiteral("cz.chrastecky.bitsailor"))) {
        return 1;
    }
    return app.exec();
}
