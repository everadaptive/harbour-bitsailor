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

public Q_SLOTS:
    // Exported on D-Bus. Only the provider may call it.
    void ShowPicker(const QString &requestId, const QString &origin, const QString &appId);

Q_SIGNALS:
    void pickerRequested();

private:
    void callProvider(const QString &method, const QVariantList &arguments);

    QVariantMap m_pending;
};

#endif // AUTOFILLPICKERSERVICE_H
