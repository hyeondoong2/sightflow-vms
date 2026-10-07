import QtQuick
import QtQuick.Window
import SightFlowVMS

Window {
    width: 640
    height: 480
    visible: true
    title: "SightFlow VMS"

    Rectangle {
        // Letterbox background: VideoDisplayItem::paint() draws the frame
        // with KeepAspectRatio, so this fills the bars on the sides/top-bottom.
        anchors.fill: parent
        color: "black"
    }

    VideoDisplayItem {
        id: videoDisplay
        objectName: "videoDisplay"
        anchors.fill: parent
    }

    // Server status strip: polls sightflow-server.exe over HTTP
    // (ServerStatusModel, docs/DECISIONS.md D18) -- entirely separate from
    // the RTSP video above. "MediaMTX 송출" is MediaMTX's own view of
    // whether a source is publishing (GET /channels/test); "서버 디코딩"
    // is sightflow-server.exe's own DecodeWorker state and frame count
    // (GET /channels/test/metrics) -- not frames this window has drawn.
    ServerStatusModel {
        id: serverStatus
    }

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: statusText.implicitHeight + 8
        color: "#99000000"

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

                mediaMtxPart + "  |  " + decodePart
            }
        }
    }
}
