// SPDX-License-Identifier: GPL-3.0-only

#include "quick/image_providers.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "quick/quick_workspace.hpp"

#include <QApplication>
#include <QIcon>
#include <QMetaEnum>
#include <QPainter>
#include <QStyle>
#include <QUrl>

namespace trackknife::quick {

CoverProvider::CoverProvider(QuickWorkspace& workspace)
    : QQuickImageProvider(QQuickImageProvider::Image), workspace_(workspace) {}

QImage CoverProvider::requestImage(const QString& id, QSize* size, const QSize& requested) {
    // The key, then "#<revision>" so a cover that arrives is asked for again.
    const auto key = QUrl::fromPercentEncoding(id.section(QLatin1Char('#'), 0, 0).toUtf8());
    auto image = workspace_.cover(key);
    if (image.isNull()) {
        // Not there, or not yet: a single clear pixel, which the page takes
        // for no cover and draws its placeholder for.
        image = QImage{1, 1, QImage::Format_ARGB32};
        image.fill(Qt::transparent);
    }
    if (size != nullptr) {
        *size = image.size();
    }
    if (image.width() > 1 && requested.isValid() && requested.width() > 0 && requested.height() > 0) {
        image = image.scaled(requested, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

IconProvider::IconProvider() : QQuickImageProvider(QQuickImageProvider::Pixmap) {}

QPixmap IconProvider::requestPixmap(const QString& encoded, QSize* size, const QSize& requested) {
    const auto id = QUrl::fromPercentEncoding(encoded.toUtf8());
    const auto disabled = id.contains(QStringLiteral("?disabled"));
    const auto one_shot = id.contains(QStringLiteral("?oneshot"));
    const auto names = QString{id}
                           .remove(QStringLiteral("?disabled"))
                           .remove(QStringLiteral("?oneshot"))
                           .split(QLatin1Char('|'));
    QIcon icon;
    for (const auto& name : names) {
        if (name == QStringLiteral("tk:album-shuffle")) {
            icon = bench::albumShuffleIcon(QApplication::palette());
        } else if (name.startsWith(QStringLiteral("sp:"))) {
            const auto meta = QMetaEnum::fromType<QStyle::StandardPixmap>();
            bool known = false;
            const auto value = meta.keyToValue(name.mid(3).toLatin1().constData(), &known);
            if (known) {
                icon = QApplication::style()->standardIcon(static_cast<QStyle::StandardPixmap>(value));
            }
        } else {
            icon = QIcon::fromTheme(name);
        }
        if (!icon.isNull()) {
            break;
        }
    }
    const auto extent = requested.isValid() && requested.width() > 0 ? requested : QSize{16, 16};
    auto pixmap = icon.pixmap(extent, disabled ? QIcon::Disabled : QIcon::Normal);
    if (one_shot && !pixmap.isNull()) {
        // One-shot is a dot in the accent on the mode's icon.
        QPainter painter{&pixmap};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QApplication::palette().color(QPalette::Highlight));
        const auto side = static_cast<qreal>(extent.width());
        const auto dot = side * 0.38;
        painter.drawEllipse(QRectF{side - dot, side - dot, dot, dot});
    }
    if (size != nullptr) {
        *size = pixmap.size();
    }
    return pixmap;
}

} // namespace trackknife::quick
