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
