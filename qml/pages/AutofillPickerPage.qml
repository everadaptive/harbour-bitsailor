import QtQuick 2.0
import Sailfish.Silica 1.0

import cz.chrastecky.bitsailor 1.0

// Searchable list of all logins. The autofill provider opens this page for a
// manual fill. A tap on a login reports the choice to the provider. Leaving
// the page without a choice cancels the request.
Page {
    property string requestId
    property string origin
    property string appId

    property var allLogins: []
    property var logins: []
    property string searchText: ""
    property string errorText
    property bool loaded: false
    property bool answered: false

    id: page
    allowedOrientations: Orientation.All

    function loginMatches(item, text) {
        const searchable = [item.name, item.login.username];
        for (var i = 0; i < (item.login.uris || []).length; i++) {
            searchable.push(item.login.uris[i].uri);
        }

        for (var j = 0; j < searchable.length; j++) {
            if (String(searchable[j] || "").toLocaleLowerCase().indexOf(text) > -1) {
                return true;
            }
        }
        return false;
    }

    function applyFilter() {
        const text = searchText.toLocaleLowerCase();
        logins = text.length === 0 ? allLogins : allLogins.filter(function(item) {
            return loginMatches(item, text);
        });
    }

    function choose(itemId) {
        if (answered) {
            return;
        }
        answered = true;
        autofillPicker.select(requestId, itemId);
        finish();
    }

    function cancel() {
        if (answered) {
            return;
        }
        answered = true;
        autofillPicker.cancel(requestId);
    }

    // Close the page and send the app to the background, so that the
    // requesting app comes back.
    function finish() {
        pageStack.pop();
        if (typeof app.deactivate === "function") {
            app.deactivate();
        }
    }

    onSearchTextChanged: applyFilter()

    onStatusChanged: {
        if (status === PageStatus.Deactivating) {
            cancel();
        }
    }

    Component.onCompleted: {
        app.pickerPage = page;
        core.fetchItems();
    }

    Component.onDestruction: {
        cancel();
        if (app.pickerPage === page) {
            app.pickerPage = null;
        }
    }

    Connections {
        target: core

        onItemsResolved: {
            const wanted = items.filter(function(item) {
                return item.type === BitSailorCore.ItemTypeLogin && !item.deletedDate && item.login;
            });
            wanted.sort(function(a, b) {
                return String(a.name).toLocaleLowerCase().localeCompare(String(b.name).toLocaleLowerCase());
            });
            allLogins = wanted;
            applyFilter();
            loaded = true;
            autofillPicker.report("page loaded " + wanted.length + " logins");
        }

        onItemResolvingFailed: {
            errorText = qsTr("An error occured while loading items. Please try again.");
            loaded = true;
        }
    }

    BusyLabel {
        text: qsTr("Loading")
        running: !loaded
    }

    SilicaListView {
        id: listView
        anchors.fill: parent
        visible: loaded
        model: logins

        VerticalScrollDecorator {}

        header: Column {
            width: listView.width

            PageHeader {
                title: qsTr("Choose a login")
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
                visible: page.errorText.length > 0
                text: page.errorText
                color: Theme.errorColor
                wrapMode: Label.WordWrap
            }

            SearchField {
                width: parent.width
                placeholderText: qsTr("Search")
                onTextChanged: page.searchText = text
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false
            }
        }

        delegate: BackgroundItem {
            id: loginItem
            width: listView.width
            height: Math.max(Theme.itemSizeSmall, loginText.height + 2 * Theme.paddingSmall)

            Column {
                id: loginText
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter

                Label {
                    width: parent.width
                    text: modelData.name
                    font.bold: true
                    truncationMode: TruncationMode.Fade
                    color: loginItem.highlighted ? Theme.highlightColor : Theme.primaryColor
                }
                Label {
                    width: parent.width
                    visible: text.length > 0
                    text: modelData.login.username || ""
                    font.pixelSize: Theme.fontSizeSmall
                    truncationMode: TruncationMode.Fade
                    color: loginItem.highlighted ? Theme.secondaryHighlightColor : Theme.secondaryColor
                }
            }

            onClicked: page.choose(modelData.id)
        }

        ViewPlaceholder {
            enabled: loaded && logins.length === 0
            text: page.searchText.length > 0 ? qsTr("No logins match") : qsTr("No logins")
        }
    }
}
