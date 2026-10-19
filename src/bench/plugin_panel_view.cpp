// SPDX-License-Identifier: GPL-3.0-only

#include "bench/plugin_panel_view.hpp"

#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace trackknife::bench {
namespace {

constexpr int image_width = 240;

[[nodiscard]] QLabel* partLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(parent);
    // Always rich: every text is escaped, or a link. Left to guess, a
    // paragraph without a tag was shown as plain -- its &quot; as typed.
    label->setTextFormat(Qt::RichText);
    label->setText(text);
    label->setObjectName(QStringLiteral("bench-plugin-panel-part"));
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setOpenExternalLinks(true);
    return label;
}

[[nodiscard]] QString linked(const QString& text, const QString& url) {
    return url.isEmpty() ? text.toHtmlEscaped()
                         : QStringLiteral("<a href=\"%1\">%2</a>")
                               .arg(url.toHtmlEscaped(), text.toHtmlEscaped());
}

} // namespace

PluginPanelView::PluginPanelView(const QString& label, QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* heading = new QHBoxLayout;
    heading->setContentsMargins(12, 10, 6, 8);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(1);
    auto* title = new QLabel(label, this);
    title->setObjectName(QStringLiteral("bench-plugin-panel-name"));
    auto font = title->font();
    font.setWeight(QFont::DemiBold);
    font.setPointSizeF(font.pointSizeF() * 1.08);
    title->setFont(font);
    titles->addWidget(title);
    subject_ = new QLabel(this);
    subject_->setObjectName(QStringLiteral("bench-plugin-panel-subject"));
    subject_->setForegroundRole(QPalette::PlaceholderText);
    titles->addWidget(subject_);
    heading->addLayout(titles, 1);
    auto* refresh = new QToolButton(this);
    refresh->setObjectName(QStringLiteral("bench-plugin-panel-refresh"));
    refresh->setAutoRaise(true);
    refresh->setIcon(QIcon::fromTheme(QStringLiteral("view-refresh")));
    refresh->setToolTip(QStringLiteral("Ask again"));
    refresh->setAccessibleName(refresh->toolTip());
    connect(refresh, &QToolButton::clicked, this, &PluginPanelView::refreshRequested);
    heading->addWidget(refresh);
    auto* close = new QToolButton(this);
    close->setObjectName(QStringLiteral("bench-plugin-panel-close"));
    close->setAutoRaise(true);
    close->setIcon(QIcon::fromTheme(QStringLiteral("window-close")));
    close->setToolTip(QStringLiteral("Close %1").arg(label));
    close->setAccessibleName(close->toolTip());
    connect(close, &QToolButton::clicked, this, &PluginPanelView::closeRequested);
    heading->addWidget(close);
    layout->addLayout(heading);

    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    parts_ = new QWidget(scroll);
    parts_->setObjectName(QStringLiteral("bench-plugin-panel-parts"));
    parts_layout_ = new QVBoxLayout(parts_);
    parts_layout_->setContentsMargins(12, 0, 12, 12);
    parts_layout_->setSpacing(6);
    parts_layout_->addStretch(1);
    scroll->setWidget(parts_);
    layout->addWidget(scroll, 1);
}

void PluginPanelView::setSubject(const QString& subject) { subject_->setText(subject); }

void PluginPanelView::clearParts() {
    while (parts_layout_->count() > 1) {
        auto* item = parts_layout_->takeAt(0);
        delete item->widget();
        delete item;
    }
}

void PluginPanelView::showMessage(const QString& message) {
    clearParts();
    auto* label = partLabel(message.toHtmlEscaped(), parts_);
    label->setObjectName(QStringLiteral("bench-plugin-panel-message"));
    label->setForegroundRole(QPalette::PlaceholderText);
    parts_layout_->insertWidget(0, label);
}

void PluginPanelView::showContent(const PluginPanelContent& content) {
    clearParts();
    int at = 0;
    const auto add = [this, &at](QWidget* widget) { parts_layout_->insertWidget(at++, widget); };
    if (!content.title.isEmpty()) {
        auto* title = partLabel(content.title.toHtmlEscaped(), parts_);
        title->setObjectName(QStringLiteral("bench-plugin-panel-title"));
        auto font = title->font();
        font.setWeight(QFont::DemiBold);
        title->setFont(font);
        add(title);
    }
    using Kind = PluginPanelContent::Part::Kind;
    for (const auto& part : content.parts) {
        switch (part.kind) {
        case Kind::heading: {
            auto* heading = partLabel(part.text.toHtmlEscaped(), parts_);
            auto font = heading->font();
            font.setWeight(QFont::DemiBold);
            heading->setFont(font);
            heading->setContentsMargins(0, 6, 0, 0);
            heading->setProperty("part", QStringLiteral("heading"));
            add(heading);
            break;
        }
        case Kind::paragraph: {
            auto* paragraph = partLabel(part.text.toHtmlEscaped(), parts_);
            paragraph->setProperty("part", QStringLiteral("paragraph"));
            add(paragraph);
            break;
        }
        case Kind::item: {
            auto* item = new QWidget(parts_);
            auto* item_layout = new QVBoxLayout(item);
            item_layout->setContentsMargins(0, 0, 0, 0);
            item_layout->setSpacing(0);
            auto* text = partLabel(linked(part.text, part.url), item);
            text->setProperty("part", QStringLiteral("item"));
            text->setToolTip(part.url);
            item_layout->addWidget(text);
            if (!part.detail.isEmpty()) {
                auto* detail = partLabel(part.detail.toHtmlEscaped(), item);
                detail->setObjectName(QStringLiteral("bench-plugin-panel-detail"));
                detail->setForegroundRole(QPalette::PlaceholderText);
                auto font = detail->font();
                font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1.0));
                detail->setFont(font);
                item_layout->addWidget(detail);
            }
            add(item);
            break;
        }
        case Kind::image: {
            QImageReader reader{part.image};
            reader.setAutoTransform(true);
            if (const auto size = reader.size(); size.isValid() && size.width() > image_width) {
                reader.setScaledSize(
                    size.scaled(image_width, image_width * 4, Qt::KeepAspectRatio));
            }
            const auto image = reader.read();
            if (image.isNull()) {
                break;
            }
            auto* picture = new QLabel(parts_);
            picture->setObjectName(QStringLiteral("bench-plugin-panel-image"));
            picture->setPixmap(QPixmap::fromImage(image));
            picture->setToolTip(part.url);
            if (!part.url.isEmpty()) {
                picture->setCursor(Qt::PointingHandCursor);
                picture->setProperty("url", part.url);
                picture->installEventFilter(this);
            }
            add(picture);
            if (!part.text.isEmpty()) {
                auto* caption = partLabel(part.text.toHtmlEscaped(), parts_);
                caption->setForegroundRole(QPalette::PlaceholderText);
                add(caption);
            }
            break;
        }
        }
    }
    if (content.parts.empty()) {
        auto* nothing = partLabel(QStringLiteral("Nothing"), parts_);
        nothing->setForegroundRole(QPalette::PlaceholderText);
        add(nothing);
    }
}

bool PluginPanelView::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonRelease) {
        if (const auto url = watched->property("url").toString(); !url.isEmpty()) {
            QDesktopServices::openUrl(QUrl{url});
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace trackknife::bench
