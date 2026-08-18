import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import Decenza

// [barista-fork] In-app camera for "add a bean from a photo" (Phase 2B). A full-screen viewfinder + shutter;
// the captured JPEG's file URL is emitted via captured(), which the overlay hands to
// AIConversation::followUpWithImage — reusing the whole Phase 2C pipeline (off-thread decode → vision turn →
// add_bag). Camera is OPTIONAL: on a denied permission or a camera error, the user falls back to the gallery
// (galleryRequested). Reuses the QtMultimedia module already used by ScreensaverPage.
Rectangle {
    id: cam
    anchors.fill: parent
    color: "#000000"
    visible: false
    z: 100000

    // The captured photo, as a file URL (→ AIConversation.followUpWithImage).
    signal captured(url imageUrl)
    signal galleryRequested()   // "choose from files instead"
    signal closed()

    property string _status: ""

    function open() {
        cam._status = ""
        cam.visible = true
        if (typeof Barista !== "undefined" && Barista.requestCameraPermission)
            Barista.requestCameraPermission()   // camera.active flips true in onCameraPermissionResult
        else
            cam._status = TranslationManager.translate("barista.camera.unavailable",
                "Camera isn't available here — choose a photo from your files instead.")
    }
    function _close() {
        camera.active = false
        cam.visible = false
        cam.closed()
    }

    Connections {
        target: (typeof Barista !== "undefined") ? Barista : null
        function onCameraPermissionResult(granted) {
            if (!cam.visible) return
            if (granted) { camera.active = true; cam._status = "" }
            else cam._status = TranslationManager.translate("barista.camera.denied",
                "Camera permission is off. Choose a photo from your files instead, or enable the camera in Settings.")
        }
    }

    CaptureSession {
        id: session
        camera: Camera { id: camera; active: false }
        videoOutput: viewfinder
        imageCapture: ImageCapture {
            id: shot
            onImageSaved: function(requestId, path) {
                cam.captured(Qt.url("file://" + path))
                cam._close()
            }
            onErrorOccurred: function(requestId, error, message) {
                cam._status = TranslationManager.translate("barista.camera.error",
                    "Couldn't take the photo — try choosing one from your files instead.")
            }
        }
    }

    VideoOutput {
        id: viewfinder
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectCrop
        visible: camera.active
    }

    // Shown when there's no live viewfinder (permission denied / camera unavailable / error).
    Text {
        anchors.centerIn: parent
        width: parent.width * 0.8
        visible: !camera.active && cam._status.length > 0
        text: cam._status
        color: Theme.textColor
        font: Theme.bodyFont
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
    }

    // Top bar: close (×).
    AccessibleButton {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.scaled(12)
        subtle: true
        text: "×"
        icon.color: "#FFFFFF"
        accessibleName: TranslationManager.translate("common.accessibility.dismissDialog", "Dismiss")
        onClicked: cam._close()
    }

    // Bottom controls: gallery fallback · shutter · (spacer).
    RowLayout {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: Theme.scaled(24)
        spacing: Theme.scaled(28)

        AccessibleButton {
            subtle: true
            icon.source: "qrc:/icons/coffeebeans.svg"
            icon.color: "#FFFFFF"
            accessibleName: TranslationManager.translate("barista.camera.gallery", "Choose from files instead")
            onClicked: { cam._close(); cam.galleryRequested() }
        }

        // Shutter — capture the current frame to a file (→ imageSaved → captured()).
        AccessibleButton {
            enabled: camera.active
            text: TranslationManager.translate("barista.camera.shutter", "Take photo")
            accessibleName: TranslationManager.translate("barista.camera.shutter", "Take photo")
            onClicked: if (camera.active) shot.captureToFile()
        }
    }
}
