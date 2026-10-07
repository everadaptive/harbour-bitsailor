// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

import QtQuick 2.6
import tech.everadaptive.autofill 1.0

// Test client for the manual fill flow. The client side is the real
// AutofillSessionController. The UI side is the dev picker mode of
// harbour-bitsailor (see run-e2e-manual.sh).
Item {
    width: 100
    height: 100
    property bool requested: false

    AutofillSessionController {
        id: c
        applicationId: "autofill-demo"
        onSessionCreated: c.getCandidates("https://nothing.test", ["username"])
        onManualFillTicketChanged: {
            console.log("E2E ticket present:", c.manualFillTicket !== "", "candidates:", c.candidates.length)
            if (c.manualFillTicket !== "" && !requested) {
                requested = true
                c.requestManualFill()
            }
        }
        onManualFillCompleted: console.log("E2E manual fill completed")
        onManualFillCancelled: { console.log("E2E manual fill cancelled"); Qt.quit() }
        onResolved: {
            console.log("E2E resolved:", username, password)
            c.closeSession()
            Qt.quit()
        }
        onRequestInvalid: console.log("E2E invalid:", reason)
    }

    Component.onCompleted: c.ensureSession()
    Timer {
        interval: 20000
        running: true
        onTriggered: { console.log("E2E timeout"); Qt.quit() }
    }
}
