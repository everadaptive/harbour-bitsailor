// SPDX-FileCopyrightText: 2026 Zach Dunton
// SPDX-License-Identifier: MIT

import QtQuick 2.0
import Sailfish.Silica 1.0

// Confirmation of an autofill save. The autofill provider opens this page
// after an app reported a login or a signup. The page shows the site, the
// requesting app, the username, and whether the save creates or updates a
// login. It never shows the password: the provider keeps it. Leaving the page
// without a choice declines the save.
Page {
    property string requestId
    property string origin
    property string appId
    // "create" or "update"
    property string action
    property string username
    property string itemName

    property bool answered: false

    id: page
    allowedOrientations: Orientation.All

    // choice: "save", "decline", or "never"
    function answer(choice) {
        if (answered) {
            return;
        }
        answered = true;
        autofillPicker.answerSave(requestId, choice);
    }

    // Close the page and send the app to the background, so that the
    // requesting app comes back.
    function finish(choice) {
        answer(choice);
        pageStack.pop();
        if (typeof app.deactivate === "function") {
            app.deactivate();
        }
    }

    onStatusChanged: {
        if (status === PageStatus.Deactivating) {
            answer("decline");
        }
    }

    Component.onCompleted: app.savePage = page

    Component.onDestruction: {
        answer("decline");
        if (app.savePage === page) {
            app.savePage = null;
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: column.height

        VerticalScrollDecorator {}

        Column {
            id: column
            width: parent.width
            spacing: Theme.paddingLarge

            PageHeader {
                title: page.action === "update" ? qsTr("Update login?") : qsTr("Save login?")
                description: page.origin
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - Theme.horizontalPageMargin * 2
                visible: page.appId.length > 0
                text: qsTr("Requested by %1").arg(page.appId)
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Label.WordWrap
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - Theme.horizontalPageMargin * 2
                text: page.action === "update"
                      ? qsTr("Change the saved password of this login:")
                      : qsTr("Save a new login:")
                color: Theme.highlightColor
                wrapMode: Label.WordWrap
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - Theme.horizontalPageMargin * 2
                text: page.itemName
                font.bold: true
                color: Theme.primaryColor
                wrapMode: Label.WordWrap
            }

            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - Theme.horizontalPageMargin * 2
                visible: page.username.length > 0
                text: page.username
                color: Theme.secondaryColor
                wrapMode: Label.WordWrap
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: page.action === "update" ? qsTr("Update") : qsTr("Save")
                onClicked: page.finish("save")
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Not now")
                onClicked: page.finish("decline")
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                //: Autofill save prompt. The user never wants to save a login for this site.
                text: qsTr("Never for this site")
                onClicked: page.finish("never")
            }
        }
    }
}
