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
}
