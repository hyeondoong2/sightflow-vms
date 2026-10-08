import QtQuick
import QtQuick.Window
import SightFlowVMS

Window {
    width: 1280
    height: 480
    visible: true
    title: "SightFlow VMS"

    // Two fixed channel panes, side by side, each exactly half the window's
    // width -- proportional (not fixed-pixel), so the split stays 50/50 on
    // resize. Each VideoDisplayItem independently preserves its own frame's
    // aspect ratio within its pane via paint()'s existing KeepAspectRatio
    // logic (unchanged) -- nothing new was needed for that part.
    Row {
        anchors.fill: parent

        // Channel "test" pane.
        Item {
            id: paneTest
            width: parent.width / 2
            height: parent.height

            // Which of this channel's recent "화면 변화 감지" events (if
            // any) the user has selected to view a snapshot of -- -1 means
            // none selected. Cleared (see the Connections block below) the
            // moment the event drops out of recentChangeEvents or the
            // server becomes unreachable, so a stale snapshot is never left
            // looking current (docs/DECISIONS.md D23).
            property int selectedEventIdTest: -1

            Rectangle {
                // Letterbox background: VideoDisplayItem::paint() draws the
                // frame with KeepAspectRatio, so this fills the bars.
                anchors.fill: parent
                color: "black"
            }

            VideoDisplayItem {
                id: videoDisplayTest
                objectName: "videoDisplayTest"
                anchors.fill: parent
            }

            // This channel's SERVER-side status, queried over HTTP from
            // sightflow-server.exe (ServerStatusModel, D21) -- MediaMTX's
            // publish state and this server's own DecodeWorker state for
            // "test" specifically. Entirely separate from
            // videoDisplayTest.connectionState below, which is this
            // window's own RTSP connection to the camera -- the two must
            // never be read as the same signal.
            ServerStatusModel {
                id: serverStatusTest
                channelName: "test"
            }

            Rectangle {
                id: serverStatusBarTest
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: serverStatusTextTest.implicitHeight + 8
                color: "#99000000"

                Text {
                    id: serverStatusTextTest
                    anchors.fill: parent
                    anchors.margins: 4
                    color: "white"
                    font.pixelSize: 11
                    wrapMode: Text.NoWrap
                    text: {
                        const mediaMtxPart = !serverStatusTest.channelStatusReachable
                            ? qsTr("서버 연결 안 됨")
                            : !serverStatusTest.mediaMtxReachable
                                ? qsTr("MediaMTX 응답 없음")
                                : qsTr("MediaMTX 송출 ") + (serverStatusTest.mediaMtxLive ? qsTr("있음") : qsTr("없음"))

                        const decodePart = !serverStatusTest.decodeMetricsReachable
                            ? qsTr("서버 디코딩: 연결 안 됨")
                            : qsTr("서버 디코딩 ") + serverStatusTest.decodeState
                                + qsTr(" (") + serverStatusTest.framesDecoded + qsTr("프레임)")

                        qsTr("test  |  ") + mediaMtxPart + "  " + decodePart
                    }
                }
            }

            // "화면 변화 감지" (screen change detection) for THIS channel --
            // sightflow-server.exe's ChangeDetector measuring how much of a
            // small downscaled thumbnail changed between samples (D22). This
            // is NOT a person/object/motion recognition result -- it only
            // reports that the picture changed by a meaningful amount, so the
            // wording stays neutral on purpose. Shows up to 5 individual
            // recent events (ServerStatusModel.recentChangeEvents), not just
            // a count -- kept compact (small font, tight spacing) so it
            // never takes much of the video area.
            Rectangle {
                anchors.top: serverStatusBarTest.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: changeEventColumnTest.implicitHeight + 8
                color: "#99000000"

                Column {
                    id: changeEventColumnTest
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 4
                    spacing: 1

                    Text {
                        width: parent.width
                        color: "white"
                        font.pixelSize: 11
                        text: !serverStatusTest.changeEventsReachable
                            ? qsTr("화면 변화 감지: 서버 연결 안 됨")
                            : serverStatusTest.recentChangeEvents.length === 0
                                ? qsTr("화면 변화 감지: 감지된 변화 없음")
                                : qsTr("화면 변화 감지 (최근 ") + serverStatusTest.recentChangeEvents.length + qsTr("건)")
                    }

                    Repeater {
                        model: serverStatusTest.changeEventsReachable ? serverStatusTest.recentChangeEvents : []
                        delegate: Text {
                            width: changeEventColumnTest.width
                            color: paneTest.selectedEventIdTest === modelData.id ? "yellow" : "white"
                            font.pixelSize: 10
                            text: "  " + modelData.time + "  ·  " + Math.round(modelData.ratio * 100) + "%"
                                + (modelData.snapshotAvailable ? qsTr("  [스냅샷]") : "")

                            // Clicking a recent event with a captured
                            // snapshot selects it (toggles off if already
                            // selected) -- an event with no snapshot can't
                            // be selected at all, since there is nothing to
                            // show (docs/DECISIONS.md D23).
                            MouseArea {
                                anchors.fill: parent
                                enabled: !!modelData.snapshotAvailable
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: {
                                    paneTest.selectedEventIdTest = (paneTest.selectedEventIdTest === modelData.id ? -1 : modelData.id)
                                }
                            }
                        }
                    }
                }
            }

            // This channel's OWN client-side connection state (CaptureWorker,
            // D19/D20) -- the window's own RTSP connection, not the server
            // status above and not the other pane's state.
            Text {
                anchors.centerIn: parent
                visible: videoDisplayTest.connectionState !== "running"
                color: "white"
                font.pixelSize: 16
                text: videoDisplayTest.connectionState === "retrying"
                    ? qsTr("재연결 중...")
                    : videoDisplayTest.connectionState === "stopped"
                        ? qsTr("연결 중지됨")
                        : qsTr("연결 중...")
            }

            // Clears the selected event the moment it is no longer one of
            // this channel's recent events, or the server/its change-event
            // query becomes unreachable -- an old snapshot must never be
            // left on screen looking like the current scene
            // (docs/DECISIONS.md D23). Does not depend on
            // videoDisplayTest.connectionState: that is this window's own
            // RTSP link to the camera, a separate concern from the
            // server-side event history this selection is about.
            Connections {
                target: serverStatusTest
                function onStatusChanged() {
                    if (paneTest.selectedEventIdTest === -1) {
                        return;
                    }
                    if (!serverStatusTest.changeEventsReachable) {
                        paneTest.selectedEventIdTest = -1;
                        return;
                    }
                    const events = serverStatusTest.recentChangeEvents;
                    let stillPresent = false;
                    for (let i = 0; i < events.length; i++) {
                        if (events[i].id === paneTest.selectedEventIdTest) {
                            stillPresent = true;
                            break;
                        }
                    }
                    if (!stillPresent) {
                        paneTest.selectedEventIdTest = -1;
                    }
                }
            }

            // Selected event's snapshot overlay -- shown in THIS pane only,
            // on top of the live video, without changing the pane's size or
            // the window's fixed 2-channel 50/50 layout (docs/DECISIONS.md
            // D23). SnapshotDisplayItem fetches/decodes an actual decoded
            // frame captured at detection time (SnapshotDecoder.h) -- never
            // a fabricated or placeholder image.
            Rectangle {
                anchors.fill: parent
                visible: paneTest.selectedEventIdTest !== -1
                color: "#cc000000"

                SnapshotDisplayItem {
                    id: snapshotViewTest
                    anchors.fill: parent
                    anchors.margins: 28
                    snapshotUrl: paneTest.selectedEventIdTest !== -1
                        ? "http://127.0.0.1:8080/channels/test/events/" + paneTest.selectedEventIdTest + "/snapshot"
                        : ""
                }

                Text {
                    anchors.centerIn: parent
                    visible: !snapshotViewTest.hasImage
                    color: "white"
                    font.pixelSize: 14
                    text: snapshotViewTest.loading ? qsTr("불러오는 중...") : snapshotViewTest.errorText
                }

                Text {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 6
                    color: "white"
                    font.pixelSize: 18
                    text: "✕"

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: paneTest.selectedEventIdTest = -1
                    }
                }
            }
        }

        // Channel "test2" pane -- identical structure, independent
        // ServerStatusModel and independent CaptureWorker/VideoDisplayItem.
        Item {
            id: paneTest2
            width: parent.width / 2
            height: parent.height

            property int selectedEventIdTest2: -1

            Rectangle {
                anchors.fill: parent
                color: "black"
            }

            VideoDisplayItem {
                id: videoDisplayTest2
                objectName: "videoDisplayTest2"
                anchors.fill: parent
            }

            ServerStatusModel {
                id: serverStatusTest2
                channelName: "test2"
            }

            Rectangle {
                id: serverStatusBarTest2
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: serverStatusTextTest2.implicitHeight + 8
                color: "#99000000"

                Text {
                    id: serverStatusTextTest2
                    anchors.fill: parent
                    anchors.margins: 4
                    color: "white"
                    font.pixelSize: 11
                    wrapMode: Text.NoWrap
                    text: {
                        const mediaMtxPart = !serverStatusTest2.channelStatusReachable
                            ? qsTr("서버 연결 안 됨")
                            : !serverStatusTest2.mediaMtxReachable
                                ? qsTr("MediaMTX 응답 없음")
                                : qsTr("MediaMTX 송출 ") + (serverStatusTest2.mediaMtxLive ? qsTr("있음") : qsTr("없음"))

                        const decodePart = !serverStatusTest2.decodeMetricsReachable
                            ? qsTr("서버 디코딩: 연결 안 됨")
                            : qsTr("서버 디코딩 ") + serverStatusTest2.decodeState
                                + qsTr(" (") + serverStatusTest2.framesDecoded + qsTr("프레임)")

                        qsTr("test2  |  ") + mediaMtxPart + "  " + decodePart
                    }
                }
            }

            Rectangle {
                anchors.top: serverStatusBarTest2.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: changeEventColumnTest2.implicitHeight + 8
                color: "#99000000"

                Column {
                    id: changeEventColumnTest2
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 4
                    spacing: 1

                    Text {
                        width: parent.width
                        color: "white"
                        font.pixelSize: 11
                        text: !serverStatusTest2.changeEventsReachable
                            ? qsTr("화면 변화 감지: 서버 연결 안 됨")
                            : serverStatusTest2.recentChangeEvents.length === 0
                                ? qsTr("화면 변화 감지: 감지된 변화 없음")
                                : qsTr("화면 변화 감지 (최근 ") + serverStatusTest2.recentChangeEvents.length + qsTr("건)")
                    }

                    Repeater {
                        model: serverStatusTest2.changeEventsReachable ? serverStatusTest2.recentChangeEvents : []
                        delegate: Text {
                            width: changeEventColumnTest2.width
                            color: paneTest2.selectedEventIdTest2 === modelData.id ? "yellow" : "white"
                            font.pixelSize: 10
                            text: "  " + modelData.time + "  ·  " + Math.round(modelData.ratio * 100) + "%"
                                + (modelData.snapshotAvailable ? qsTr("  [스냅샷]") : "")

                            MouseArea {
                                anchors.fill: parent
                                enabled: !!modelData.snapshotAvailable
                                cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                onClicked: {
                                    paneTest2.selectedEventIdTest2 = (paneTest2.selectedEventIdTest2 === modelData.id ? -1 : modelData.id)
                                }
                            }
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: videoDisplayTest2.connectionState !== "running"
                color: "white"
                font.pixelSize: 16
                text: videoDisplayTest2.connectionState === "retrying"
                    ? qsTr("재연결 중...")
                    : videoDisplayTest2.connectionState === "stopped"
                        ? qsTr("연결 중지됨")
                        : qsTr("연결 중...")
            }

            Connections {
                target: serverStatusTest2
                function onStatusChanged() {
                    if (paneTest2.selectedEventIdTest2 === -1) {
                        return;
                    }
                    if (!serverStatusTest2.changeEventsReachable) {
                        paneTest2.selectedEventIdTest2 = -1;
                        return;
                    }
                    const events = serverStatusTest2.recentChangeEvents;
                    let stillPresent = false;
                    for (let i = 0; i < events.length; i++) {
                        if (events[i].id === paneTest2.selectedEventIdTest2) {
                            stillPresent = true;
                            break;
                        }
                    }
                    if (!stillPresent) {
                        paneTest2.selectedEventIdTest2 = -1;
                    }
                }
            }

            Rectangle {
                anchors.fill: parent
                visible: paneTest2.selectedEventIdTest2 !== -1
                color: "#cc000000"

                SnapshotDisplayItem {
                    id: snapshotViewTest2
                    anchors.fill: parent
                    anchors.margins: 28
                    snapshotUrl: paneTest2.selectedEventIdTest2 !== -1
                        ? "http://127.0.0.1:8080/channels/test2/events/" + paneTest2.selectedEventIdTest2 + "/snapshot"
                        : ""
                }

                Text {
                    anchors.centerIn: parent
                    visible: !snapshotViewTest2.hasImage
                    color: "white"
                    font.pixelSize: 14
                    text: snapshotViewTest2.loading ? qsTr("불러오는 중...") : snapshotViewTest2.errorText
                }

                Text {
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.margins: 6
                    color: "white"
                    font.pixelSize: 18
                    text: "✕"

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: paneTest2.selectedEventIdTest2 = -1
                    }
                }
            }
        }
    }
}
