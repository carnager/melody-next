// SPDX-License-Identifier: GPL-3.0-only
#include "bench/cover_thumbnail.hpp"
#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QUrl>
namespace trackknife::bench {
CoverThumbnail::CoverThumbnail(QWidget* parent) : QLabel(parent) {
    setObjectName(QStringLiteral("bench-metadata-cover-thumbnail"));
    setFixedSize(112, 112);
    setAlignment(Qt::AlignCenter);
    setFrameShape(QFrame::StyledPanel);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAccessibleName(QStringLiteral("Front cover; drop or paste an image"));
    setToolTip(QStringLiteral("Drop an image file or paste an image to stage the front cover"));
    setCover({}, false);
}
void CoverThumbnail::setCover(const QImage& image, bool mixed) {
    if (mixed || image.isNull()) {
        setText(mixed ? QStringLiteral("Multiple covers") : QStringLiteral("No front cover"));
    } else {
        setPixmap(QPixmap::fromImage(image).scaled(108, 108, Qt::KeepAspectRatio,
                                                   Qt::SmoothTransformation));
    }
}
void CoverThumbnail::dragEnterEvent(QDragEnterEvent* event) {
    if (!property("cover-editable").toBool())
        return;
    if ((event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1 &&
         event->mimeData()->urls().front().isLocalFile()) ||
        event->mimeData()->hasImage())
        event->acceptProposedAction();
}
void CoverThumbnail::dropEvent(QDropEvent* event) {
    if (!property("cover-editable").toBool())
        return;
    const auto* mime = event->mimeData();
    if (mime->hasUrls() && mime->urls().size() == 1 && mime->urls().front().isLocalFile()) {
        emit fileDropped(mime->urls().front().toLocalFile());
        event->acceptProposedAction();
    } else if (mime->hasImage()) {
        emit imagePasted(qvariant_cast<QImage>(mime->imageData()));
        event->acceptProposedAction();
    }
}
void CoverThumbnail::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Paste) && property("cover-editable").toBool()) {
        const auto* mime = QApplication::clipboard()->mimeData();
        if (mime->hasUrls() && mime->urls().size() == 1 && mime->urls().front().isLocalFile())
            emit fileDropped(mime->urls().front().toLocalFile());
        else if (mime->hasImage())
            emit imagePasted(qvariant_cast<QImage>(mime->imageData()));
        event->accept();
    } else
        QLabel::keyPressEvent(event);
}
} // namespace trackknife::bench
