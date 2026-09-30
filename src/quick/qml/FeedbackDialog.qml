// SPDX-License-Identifier: GPL-3.0-only
import QtQuick
import QtQuick.Controls

// What went wrong with files: Apply blocked, saved with problems, a
// ReplayGain scan's failures. After a partial save it offers to retry the
// unfinished files with the reviewed changes.
TableDialog {
    id: feedback
    objectName: "bench-preparation-feedback"

    signal retry()
    // Closed, not to retry.
    signal done()
    property bool retrying: false

    function show(title, summary, rows, retryOffered, applyCommitted) {
        const table = [];
        for (const row of rows)
            table.push([row.file, row.detail]);
        feedback.retrying = false;
        feedback.extraButton = retryOffered ? qsTr("Retry failed / stopped files") : "";
        feedback.title = title;
        feedback.note = summary;
        feedback.headers = [qsTr("File"), qsTr("Problem")];
        feedback.rows = table;
        feedback.review = false;
        open();
        // With files already saved, closing this closes the editor too.
        const close = feedback.standardButton(Dialog.Close);
        if (close)
            close.text = retryOffered && applyCommitted ? qsTr("Close editor") : qsTr("Close");
    }

    onExtra: {
        retrying = true;
        close();
        retry();
    }
    onClosed: {
        if (!retrying)
            done();
    }
}
