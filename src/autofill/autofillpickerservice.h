// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

#ifndef AUTOFILLPICKERSERVICE_H
#define AUTOFILLPICKERSERVICE_H

#include <QDBusContext>
#include <QObject>
#include <QVariantMap>

// D-Bus object of the UI app: cz.chrastecky.bitsailor.AutofillPicker. The
// autofill provider asks the UI app to show a login picker for a manual
// fill. The picker page reports the choice with select() or cancel().
// The provider also asks the UI app to show a save prompt (protocol v2).
// The prompt page reports the answer with answerSave(). The request holds
// no password: only the origin, the username, and the action.
class AutofillPickerService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "cz.chrastecky.bitsailor.AutofillPicker")

public:
    explicit AutofillPickerService(QObject *parent = nullptr);

    // QML API. These are not slots, so they are not exported on D-Bus.
    Q_INVOKABLE bool hasPending() const;
    // Return {requestId, origin, appId} and clear the pending request.
    Q_INVOKABLE QVariantMap takePending();
    Q_INVOKABLE void select(const QString &requestId, const QString &itemId);
    Q_INVOKABLE void cancel(const QString &requestId);
    // Write a line to the log. The picker page uses it to report its state.
    Q_INVOKABLE void report(const QString &message) const;

    Q_INVOKABLE bool hasPendingSave() const;
    // Return {requestId, origin, appId, action, username, itemName} and clear
    // the pending request. The action is "create" or "update".
    Q_INVOKABLE QVariantMap takePendingSave();
    // choice is "save", "decline", or "never".
    Q_INVOKABLE void answerSave(const QString &requestId, const QString &choice);

public Q_SLOTS:
    // Exported on D-Bus. Only the provider may call it.
    void ShowPicker(const QString &requestId, const QString &origin, const QString &appId);
    // Exported on D-Bus. Only the provider may call it.
    void ShowSavePrompt(const QString &requestId, const QString &origin, const QString &appId,
                        const QString &action, const QString &username, const QString &itemName);

Q_SIGNALS:
    void pickerRequested();
    void savePromptRequested();

private:
    void callProvider(const QString &method, const QVariantList &arguments);

    bool callerIsProvider();

    QVariantMap m_pending;
    QVariantMap m_pendingSave;
};

#endif // AUTOFILLPICKERSERVICE_H
