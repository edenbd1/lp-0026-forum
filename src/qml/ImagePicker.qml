import QtQuick
import QtQuick.Dialogs

// The platform's file dialog (Qt's own where there is none). It returns
// file:// URLs; the backend reads only regular local files from them.
FileDialog {
    signal picked(var urls)
    title: "Attach images"
    fileMode: FileDialog.OpenFiles
    nameFilters: ["Images (*.png *.jpg *.jpeg *.webp)"]
    onAccepted: {
        var out = []
        for (var i = 0; i < selectedFiles.length; i++) out.push(selectedFiles[i].toString())
        picked(out)
    }
}
