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
            width: parent.width / 2
            height: parent.height

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
                            color: "white"
                            font.pixelSize: 10
                            text: "  " + modelData.time + "  ·  " + Math.round(modelData.ratio * 100) + "%"
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
        }

        // Channel "test2" pane -- identical structure, independent
        // ServerStatusModel and independent CaptureWorker/VideoDisplayItem.
        Item {
            width: parent.width / 2
            height: parent.height

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
                            color: "white"
                            font.pixelSize: 10
                            text: "  " + modelData.time + "  ·  " + Math.round(modelData.ratio * 100) + "%"
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
        }
    }
}
