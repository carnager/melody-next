// SPDX-License-Identifier: GPL-3.0-only

#include "bench/about_dialog.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>
#include <QVBoxLayout>

namespace trackknife::bench {
namespace {

// The mascot drawn sharp at the screen's own scale.
[[nodiscard]] QPixmap mascot(const int side, const qreal ratio) {
    QSvgRenderer renderer{QStringLiteral(":/melody/melody-mascot.svg")};
    QPixmap pixmap{QSize{side, side} * ratio};
    pixmap.fill(Qt::transparent);
    QPainter painter{&pixmap};
    renderer.render(&painter);
    painter.end();
    pixmap.setDevicePixelRatio(ratio);
    return pixmap;
}

} // namespace

AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("bench-about"));
    setWindowTitle(QStringLiteral("About Melody"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 16);
    layout->setSpacing(10);

    auto* picture = new QLabel(this);
    picture->setObjectName(QStringLiteral("bench-about-mascot"));
    picture->setPixmap(mascot(180, devicePixelRatioF()));
    picture->setAlignment(Qt::AlignHCenter);
    layout->addWidget(picture);

    auto* name = new QLabel(QStringLiteral("Melody"), this);
    auto font = name->font();
    font.setPointSizeF(font.pointSizeF() * 1.8);
    font.setWeight(QFont::DemiBold);
    name->setFont(font);
    name->setAlignment(Qt::AlignHCenter);
    layout->addWidget(name);

    const QString commit = QStringLiteral(MELODY_COMMIT);
    auto* version = new QLabel(
        commit.isEmpty()
            ? QStringLiteral("Version %1").arg(QStringLiteral(MELODY_RELEASE))
            : QStringLiteral("Version %1 · %2").arg(QStringLiteral(MELODY_RELEASE), commit),
        this);
    version->setObjectName(QStringLiteral("bench-about-version"));
    version->setAlignment(Qt::AlignHCenter);
    version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(version);

    auto* text = new QLabel(
        QStringLiteral(
            "<p>Your music library: play, tag, convert and share it.</p>"
            "<p>Free software under the <a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">"
            "GNU General Public License, version 3</a>.</p>"
            "<p><a href=\"https://github.com/carnager/melody-next\">Project</a> · "
            "<a href=\"https://github.com/carnager/melody-next/tree/main/docs\">"
            "Documentation</a></p>"),
        this);
    text->setAlignment(Qt::AlignHCenter);
    text->setWordWrap(true);
    text->setOpenExternalLinks(true);
    layout->addWidget(text);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    setFixedWidth(sizeHint().width() < 360 ? 360 : sizeHint().width());
}

} // namespace trackknife::bench
