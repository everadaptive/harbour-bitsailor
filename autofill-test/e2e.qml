// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

import QtQuick 2.6
import tech.everadaptive.autofill 1.0

Item {
    width: 100
    height: 100
    property string origin: "app://autofill-demo"

    AutofillSessionController {
        id: c
        applicationId: "autofill-demo"
        onSessionCreated: {
            console.log("E2E session created, provider:", c.providerName)
            c.getCandidates(origin, ["username"])
        }
        onCandidatesChanged: {
            console.log("E2E candidates:", JSON.stringify(c.candidates))
            if (c.candidates.length > 0) {
                c.resolve(c.candidates[0].token)
            }
        }
        onResolved: {
            console.log("E2E resolved:", username, password, email)
            c.closeSession()
            Qt.quit()
        }
        onVaultLocked: { console.log("E2E vault locked"); Qt.quit() }
        onRequestInvalid: { console.log("E2E invalid:", reason) }
    }
    Component.onCompleted: c.ensureSession()
    Timer {
        interval: 10000
        running: true
        onTriggered: { console.log("E2E timeout"); Qt.quit() }
    }
}
