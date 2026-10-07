import QtQuick
import QtQuick.Window
import SightFlowVMS

Window {
    width: 1280
    height: 480
    visible: true
    title: "SightFlow VMS"

    // Server status strip: polls sightflow-server.exe over HTTP
    // (ServerStatusModel, docs/DECISIONS.md D18) -- entirely separate from
    // both RTSP video panes below. This server only ever runs a DecodeWorker
    // for the "test" channel (D16/D20), so everything in this strip is
    // specifically about "test" -- it says nothing about "test2"'s state.
    ServerStatusModel {
        id: serverStatus
    }

    Rectangle {
        id: topBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: statusText.implicitHeight + 8
        color: "#99000000"
        z: 10

        Text {
            id: statusText
            anchors.fill: parent
            anchors.margins: 4
            color: "white"
            font.pixelSize: 12
            text: {
                const mediaMtxPart = !serverStatus.channelStatusReachable
                    ? qsTr("MediaMTX 송출: 서버 연결 안 됨")
                    : !serverStatus.mediaMtxReachable
                        ? qsTr("MediaMTX 송출: MediaMTX 응답 없음")
                        : qsTr("MediaMTX 송출: ") + (serverStatus.mediaMtxLive ? qsTr("있음") : qsTr("없음"))

                const decodePart = !serverStatus.decodeMetricsReachable
                    ? qsTr("서버 디코딩: 서버 연결 안 됨")
                    : qsTr("서버 디코딩: ") + serverStatus.decodeState
                        + qsTr(" (서버 측 디코딩 프레임 ") + serverStatus.framesDecoded + qsTr("개)")

                qsTr("[서버 상태 - test 채널 전용] ") + mediaMtxPart + "  |  " + decodePart
            }
        }
    }

    // Two fixed channel panes, side by side, each exactly half the window's
    // width -- proportional (not fixed-pixel), so the split stays 50/50 on
    // resize. Each VideoDisplayItem independently preserves its own frame's
    // aspect ratio within its pane via paint()'s existing KeepAspectRatio
    // logic (unchanged) -- nothing new was needed for that part.
    Row {
        anchors.top: topBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

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

            Text {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: 4
                color: "white"
                font.pixelSize: 12
                text: "test"
            }

            // This channel's OWN client-side connection state (CaptureWorker,
            // D19/D20) -- not the server status strip above, and not the
            // other pane's state.
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

        // Channel "test2" pane -- identical structure, independent state.
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

            Text {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: 4
                color: "white"
                font.pixelSize: 12
                text: "test2"
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
