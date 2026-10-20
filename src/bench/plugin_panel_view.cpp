// SPDX-License-Identifier: GPL-3.0-only

#include "bench/plugin_panel_view.hpp"

#include <QAbstractButton>
#include <QDesktopServices>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace trackknife::bench {
namespace {

// A paragraph longer than this is folded, a More beneath it.
constexpr int folded_lines = 6;

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

[[nodiscard]] QColor faded(const QWidget* widget, const int alpha) {
    auto colour = widget->palette().color(QPalette::WindowText);
    colour.setAlpha(alpha);
    return colour;
}

// An image as wide as the panel -- no wider than it is -- its corners
// rounded, a link on it.
class FittedImage final : public QWidget {
  public:
    FittedImage(QImage image, QString url, QWidget* parent)
        : QWidget(parent), image_(std::move(image)), url_(std::move(url)) {
        setObjectName(QStringLiteral("bench-plugin-panel-image"));
        QSizePolicy policy{QSizePolicy::Preferred, QSizePolicy::Preferred};
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setToolTip(url_);
        if (!url_.isEmpty()) {
            setCursor(Qt::PointingHandCursor);
        }
    }
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(const int width) const override {
        const auto shown = std::min(width, image_.width());
        return image_.width() > 0 ? shown * image_.height() / image_.width() : 0;
    }
    [[nodiscard]] QSize sizeHint() const override {
        return {image_.width(), heightForWidth(image_.width())};
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        const auto width = std::min(this->width(), image_.width());
        const QRectF area{0, 0, static_cast<double>(width),
                          static_cast<double>(heightForWidth(width))};
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath rounded;
        rounded.addRoundedRect(area, 6, 6);
        painter.setClipPath(rounded);
        painter.drawImage(area, image_);
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!url_.isEmpty() && event->button() == Qt::LeftButton) {
            QDesktopServices::openUrl(QUrl{url_});
        }
    }

  private:
    QImage image_;
    QString url_;
};

// Its items side by side, a line for as many as fit -- links in a row.
class FlowLayout final : public QLayout {
  public:
    explicit FlowLayout(QWidget* parent) : QLayout(parent) { setContentsMargins(0, 0, 0, 0); }
    FlowLayout(const FlowLayout&) = delete;
    FlowLayout& operator=(const FlowLayout&) = delete;
    ~FlowLayout() override {
        while (auto* item = takeAt(0)) {
            delete item;
        }
    }
    void addItem(QLayoutItem* item) override { items_.push_back(item); }
    [[nodiscard]] int count() const override { return static_cast<int>(items_.size()); }
    [[nodiscard]] QLayoutItem* itemAt(const int index) const override {
        return index >= 0 && index < count() ? items_[static_cast<std::size_t>(index)] : nullptr;
    }
    QLayoutItem* takeAt(const int index) override {
        if (index < 0 || index >= count()) {
            return nullptr;
        }
        auto* item = items_[static_cast<std::size_t>(index)];
        items_.erase(items_.begin() + index);
        return item;
    }
    [[nodiscard]] Qt::Orientations expandingDirections() const override { return {}; }
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(const int width) const override {
        return arrange(QRect{0, 0, width, 0}, false);
    }
    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        static_cast<void>(arrange(rect, true));
    }
    [[nodiscard]] QSize sizeHint() const override { return minimumSize(); }
    [[nodiscard]] QSize minimumSize() const override {
        QSize size;
        for (const auto* item : items_) {
            size = size.expandedTo(item->minimumSize());
        }
        return size;
    }

  private:
    int arrange(const QRect& rect, const bool place) const {
        constexpr int gap = 6;
        int x = rect.x();
        int y = rect.y();
        int line = 0;
        for (auto* item : items_) {
            const auto hint = item->sizeHint();
            if (x > rect.x() && x + hint.width() > rect.right() + 1) {
                x = rect.x();
                y += line + gap;
                line = 0;
            }
            if (place) {
                item->setGeometry(QRect{QPoint{x, y}, hint});
            }
            x += hint.width() + gap;
            line = std::max(line, hint.height());
        }
        return y + line - rect.y();
    }
    std::vector<QLayoutItem*> items_;
};

// ADR-0304: Melody's colours, mixed as its own rows mix them.
[[nodiscard]] QColor mixed(const QColor& from, const QColor& to, const int percent) {
    const auto mix = [percent](const int a, const int b) { return a + (b - a) * percent / 100; };
    return QColor::fromRgb(mix(from.red(), to.red()), mix(from.green(), to.green()),
                           mix(from.blue(), to.blue()));
}

[[nodiscard]] QFont smaller(QFont font) {
    font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1.0));
    return font;
}

[[nodiscard]] QLabel* plainLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    return label;
}

// A small round thing, painted: a status's dot, a person's initial.
class Dot final : public QWidget {
  public:
    Dot(const int extent, QColor colour, QString letter, QWidget* parent)
        : QWidget(parent), colour_(std::move(colour)), letter_(std::move(letter)) {
        setFixedSize(extent, extent);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour_);
        painter.drawEllipse(QRectF{rect()}.adjusted(0.5, 0.5, -0.5, -0.5));
        if (!letter_.isEmpty()) {
            auto font = this->font();
            font.setWeight(QFont::DemiBold);
            font.setPixelSize(std::max(7, height() * 11 / 20));
            painter.setFont(font);
            painter.setPen(Qt::white);
            painter.drawText(rect(), Qt::AlignCenter, letter_);
        }
    }

  private:
    QColor colour_;
    QString letter_;
};

// A person's colour, from the name: the same as the listening room's web
// page gives it (together/web/room.html, colour()).
[[nodiscard]] QColor personColour(const QString& name) {
    std::uint32_t hash = 0;
    for (const auto code : name.toUcs4()) {
        hash = hash * 31U + code;
    }
    return QColor::fromHslF(static_cast<float>(hash % 360U) / 360.0F, 0.55F, 0.48F);
}

// A person, as a chip: a coloured initial and the name.
class PersonChip final : public QWidget {
  public:
    PersonChip(const QString& name, QWidget* parent) : QWidget(parent) {
        setObjectName(QStringLiteral("bench-plugin-panel-person"));
        setToolTip(name);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(3, 3, 10, 3);
        layout->setSpacing(6);
        const auto extent = fontMetrics().height() + 2;
        layout->addWidget(new Dot(extent, personColour(name), name.left(1).toUpper(), this));
        auto* label = plainLabel(name, this);
        label->setFont(smaller(font()));
        layout->addWidget(label);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(mixed(palette().color(QPalette::Window),
                               palette().color(QPalette::WindowText), 8));
        const QRectF area{rect()};
        painter.drawRoundedRect(area, area.height() / 2, area.height() / 2);
    }
};

// ADR-0304: a toggle as a switch -- the accent when it is on.
class PanelSwitch final : public QAbstractButton {
  public:
    explicit PanelSwitch(QWidget* parent) : QAbstractButton(parent) {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
    }
    [[nodiscard]] QSize sizeHint() const override {
        const auto height = fontMetrics().height() + 4;
        return {height * 9 / 5, height};
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        const auto& colours = palette();
        const QRectF track = QRectF{rect()}.adjusted(0.5, 0.5, -0.5, -0.5);
        const auto radius = track.height() / 2;
        const auto off = mixed(colours.color(QPalette::Window),
                               colours.color(QPalette::WindowText), 28);
        auto fill = isChecked() ? colours.color(QPalette::Highlight) : off;
        if (underMouse()) {
            fill = fill.lighter(108);
        }
        if (!isEnabled()) {
            fill.setAlphaF(0.5);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(track, radius, radius);
        const auto knob = track.height() - 4;
        const auto x = isChecked() ? track.right() - 2 - knob : track.left() + 2;
        painter.setBrush(Qt::white);
        painter.drawEllipse(QRectF{x, track.top() + 2, knob, knob});
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen{colours.color(QPalette::Highlight), 1.5});
            painter.drawRoundedRect(track.adjusted(-1, -1, 1, 1), radius + 1, radius + 1);
        }
    }
};

// ADR-0304: a track's row, as Melody's own: a small cover, the title over
// the artist, its buttons at the right -- what plays in the accent, quiet
// buttons only under the pointer.
class TrackRow final : public QWidget {
  public:
    TrackRow(const PluginPanelContent::Part& part, QWidget* parent)
        : QWidget(parent), playing_(part.on), url_(part.url) {
        setObjectName(QStringLiteral("bench-plugin-panel-track"));
        setProperty("playing", playing_);
        setAttribute(Qt::WA_Hover);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 4, 6, 4);
        layout->setSpacing(8);
        const auto extent = fontMetrics().height() * 2 + 2;
        if (!part.cover.isEmpty()) {
            QImageReader reader{part.cover};
            reader.setAutoTransform(true);
            if (const auto size = reader.size(); size.isValid()) {
                reader.setScaledSize(size.scaled(QSize{extent, extent} * devicePixelRatioF(),
                                                 Qt::KeepAspectRatioByExpanding));
            }
            cover_ = reader.read();
            auto* space = new QWidget(this);
            space->setFixedSize(extent, extent);
            space->setAttribute(Qt::WA_TransparentForMouseEvents);
            cover_space_ = space;
            layout->addWidget(space, 0, Qt::AlignVCenter);
        }
        auto* lines = new QVBoxLayout;
        lines->setSpacing(0);
        auto* title = plainLabel(part.text, this);
        title->setObjectName(QStringLiteral("bench-plugin-panel-track-title"));
        title->setWordWrap(true);
        auto font = title->font();
        if (playing_) {
            font.setWeight(QFont::DemiBold);
            auto colours = title->palette();
            colours.setColor(QPalette::WindowText, palette().color(QPalette::Highlight));
            title->setPalette(colours);
        }
        title->setFont(font);
        lines->addWidget(title);
        QStringList under;
        for (const auto& each : {part.artist, part.detail}) {
            if (!each.isEmpty()) {
                under << each;
            }
        }
        if (!under.isEmpty()) {
            auto* below = plainLabel(under.join(QStringLiteral(" · ")), this);
            below->setObjectName(QStringLiteral("bench-plugin-panel-track-artist"));
            below->setWordWrap(true);
            below->setFont(smaller(below->font()));
            below->setForegroundRole(QPalette::PlaceholderText);
            lines->addWidget(below);
        }
        layout->addLayout(lines, 1);
        if (!url_.isEmpty()) {
            setCursor(Qt::PointingHandCursor);
            setToolTip(url_);
        }
    }
    void addButton(QAbstractButton* button, const bool quiet) {
        static_cast<QHBoxLayout*>(layout())->addWidget(button, 0, Qt::AlignVCenter);
        if (quiet) {
            // Kept in its place while hidden: the row does not shift.
            auto policy = button->sizePolicy();
            policy.setRetainSizeWhenHidden(true);
            button->setSizePolicy(policy);
            button->setVisible(false);
            quiet_.push_back(button);
        }
    }

  protected:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave) {
            const bool shown = event->type() == QEvent::HoverEnter;
            for (auto* button : quiet_) {
                button->setVisible(shown);
            }
            update();
        }
        return QWidget::event(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter{this};
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const auto window = palette().color(QPalette::Window);
        const bool hovered = testAttribute(Qt::WA_UnderMouse);
        if (playing_ || hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(playing_ ? mixed(window, palette().color(QPalette::Highlight), 16)
                                      : mixed(window, palette().color(QPalette::WindowText), 6));
            painter.drawRoundedRect(QRectF{rect()}, 6, 6);
        }
        if (cover_space_ != nullptr) {
            const QRectF area{cover_space_->geometry()};
            QPainterPath rounded;
            rounded.addRoundedRect(area, 3, 3);
            if (cover_.isNull()) {
                painter.fillPath(rounded, mixed(window, palette().color(QPalette::WindowText), 10));
            } else {
                painter.setClipPath(rounded);
                // Filled, as a cover is: cut to a square around its middle.
                const auto side = std::min(cover_.width(), cover_.height());
                const QRect middle{(cover_.width() - side) / 2, (cover_.height() - side) / 2, side,
                                   side};
                painter.drawImage(area, cover_, middle);
            }
        }
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!url_.isEmpty() && event->button() == Qt::LeftButton) {
            QDesktopServices::openUrl(QUrl{url_});
        }
    }

  private:
    bool playing_{false};
    QString url_;
    QImage cover_;
    QWidget* cover_space_{nullptr};
    std::vector<QAbstractButton*> quiet_;
};

// An item's or a track's button: a pill -- a vote, its own drawn as chosen
// -- or, quiet, a bare glyph.
[[nodiscard]] QToolButton* rowButton(const PluginPanelContent::Button& each, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setObjectName(QStringLiteral("bench-plugin-panel-item-button"));
    button->setText(each.text);
    button->setToolTip(each.text);
    button->setCursor(Qt::PointingHandCursor);
    button->setProperty("action", each.action);
    if (!each.url.isEmpty()) {
        // ADR-0304: a page it opens, not its plugin run again.
        button->setToolTip(each.url);
        QObject::connect(button, &QToolButton::clicked, button,
                         [url = each.url] { QDesktopServices::openUrl(QUrl{url}); });
    }
    if (each.role == PluginPanelContent::Role::quiet) {
        button->setAutoRaise(true);
        button->setProperty("role", QStringLiteral("quiet"));
        button->setStyleSheet(QStringLiteral(
            "QToolButton { border: none; border-radius: 10px; padding: 2px 6px;"
            " color: palette(placeholder-text); }"
            "QToolButton:hover { background: palette(midlight); color: palette(window-text); }"));
        return button;
    }
    button->setCheckable(true);
    button->setChecked(each.on);
    button->setStyleSheet(QStringLiteral(
        "QToolButton { border: 1px solid palette(mid); border-radius: 11px;"
        " padding: 2px 10px; font-weight: 600; }"
        "QToolButton:checked { background: palette(highlight);"
        " color: palette(highlighted-text); border-color: palette(highlight); }"));
    return button;
}

// A paragraph folded to a few lines when it is long, More and Less beneath.
class FoldingParagraph final : public QWidget {
  public:
    FoldingParagraph(const QString& text, QWidget* parent) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        text_ = partLabel(text.toHtmlEscaped(), this);
        text_->setProperty("part", QStringLiteral("paragraph"));
        layout->addWidget(text_);
        more_ = new QLabel(this);
        more_->setObjectName(QStringLiteral("bench-plugin-panel-more"));
        more_->setTextFormat(Qt::RichText);
        more_->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        connect(more_, &QLabel::linkActivated, this, [this] {
            folded_ = !folded_;
            fold();
        });
        layout->addWidget(more_);
        more_->hide();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        fold();
    }

  private:
    void fold() {
        const auto limit = text_->fontMetrics().lineSpacing() * folded_lines;
        const bool long_one = text_->heightForWidth(width()) > limit + limit / folded_lines;
        more_->setVisible(long_one);
        more_->setText(folded_ ? QStringLiteral("<a href=\"more\">More</a>")
                               : QStringLiteral("<a href=\"less\">Less</a>"));
        text_->setMaximumHeight(long_one && folded_ ? limit : QWIDGETSIZE_MAX);
    }

    QLabel* text_{nullptr};
    QLabel* more_{nullptr};
    bool folded_{true};
};

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

QJsonObject PluginPanelView::inputs() const {
    QJsonObject told;
    for (auto input = inputs_.cbegin(); input != inputs_.cend(); ++input) {
        told.insert(input.key(), input.value()->text());
    }
    return told;
}

void PluginPanelView::clearParts() {
    inputs_.clear();
    while (parts_layout_->count() > 1) {
        auto* item = parts_layout_->takeAt(0);
        delete item->widget();
        delete item;
    }
}

void PluginPanelView::showMessage(const QString& message) {
    clearParts();
    has_content_ = false;
    subject_->show();
    auto* label = partLabel(message.toHtmlEscaped(), parts_);
    label->setObjectName(QStringLiteral("bench-plugin-panel-message"));
    label->setForegroundRole(QPalette::PlaceholderText);
    parts_layout_->insertWidget(0, label);
}

void PluginPanelView::showContent(const PluginPanelContent& content) {
    clearParts();
    has_content_ = true;
    int at = 0;
    const auto add = [this, &at](QWidget* widget) { parts_layout_->insertWidget(at++, widget); };
    // The answer's title as the page's, what it was asked about beneath --
    // not above as well.
    subject_->setVisible(content.title.isEmpty());
    if (!content.title.isEmpty()) {
        auto* title = partLabel(content.title.toHtmlEscaped(), parts_);
        title->setObjectName(QStringLiteral("bench-plugin-panel-title"));
        auto font = title->font();
        font.setWeight(QFont::DemiBold);
        font.setPointSizeF(font.pointSizeF() * 1.45);
        title->setFont(font);
        add(title);
        if (!subject_->text().isEmpty()) {
            auto* asked = new QLabel(subject_->text(), parts_);
            asked->setObjectName(QStringLiteral("bench-plugin-panel-asked"));
            asked->setWordWrap(true);
            asked->setForegroundRole(QPalette::PlaceholderText);
            asked->setContentsMargins(0, 0, 0, 4);
            add(asked);
        }
    }
    using Kind = PluginPanelContent::Part::Kind;
    const auto& parts = content.parts;
    // A row's button runs its plugin again -- one that opens a page opens it.
    const auto run_on_press = [this](QToolButton* button, const PluginPanelContent::Button& each) {
        if (each.url.isEmpty()) {
            connect(button, &QToolButton::clicked, this,
                    [this, action = each.action, value = each.value] {
                        emit actionRequested(action, value);
                    });
        }
    };
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto& part = parts[index];
        switch (part.kind) {
        case Kind::heading: {
            // A section: quieter and smaller than the text, a rule beneath.
            auto* section = new QWidget(parts_);
            auto* section_layout = new QVBoxLayout(section);
            section_layout->setContentsMargins(0, 12, 0, 2);
            section_layout->setSpacing(3);
            // ADR-0304: in small capitals, spaced, as a sidebar's sections.
            auto* heading = partLabel(part.text.toHtmlEscaped(), section);
            auto font = heading->font();
            font.setWeight(QFont::DemiBold);
            font.setPointSizeF(font.pointSizeF() * 0.8);
            font.setCapitalization(QFont::AllUppercase);
            font.setLetterSpacing(QFont::PercentageSpacing, 108);
            heading->setFont(font);
            auto palette = heading->palette();
            palette.setColor(QPalette::WindowText, faded(heading, 150));
            heading->setPalette(palette);
            heading->setProperty("part", QStringLiteral("heading"));
            section_layout->addWidget(heading);
            auto* rule = new QFrame(section);
            rule->setFrameShape(QFrame::HLine);
            rule->setFrameShadow(QFrame::Plain);
            auto rule_palette = rule->palette();
            rule_palette.setColor(QPalette::WindowText, faded(rule, 40));
            rule->setPalette(rule_palette);
            section_layout->addWidget(rule);
            add(section);
            break;
        }
        case Kind::paragraph:
            add(new FoldingParagraph(part.text, parts_));
            break;
        case Kind::input: {
            // ADR-0303: a field, its label above; Enter runs its action.
            if (!part.text.isEmpty()) {
                auto* label = partLabel(part.text.toHtmlEscaped(), parts_);
                label->setForegroundRole(QPalette::PlaceholderText);
                add(label);
            }
            auto* edit = new QLineEdit(parts_);
            edit->setObjectName(QStringLiteral("bench-plugin-panel-input-%1").arg(part.key));
            edit->setPlaceholderText(part.placeholder);
            edit->setEchoMode(part.secret ? QLineEdit::Password : QLineEdit::Normal);
            // What was typed stays, unless the plugin gives a new value.
            const bool changed = given_.value(part.key) != part.value;
            given_.insert(part.key, part.value);
            if (changed) {
                typed_.remove(part.key);
            }
            edit->setText(typed_.contains(part.key) ? typed_.value(part.key) : part.value);
            connect(edit, &QLineEdit::textEdited, this,
                    [this, key = part.key](const QString& text) { typed_.insert(key, text); });
            if (!part.action.isEmpty()) {
                connect(edit, &QLineEdit::returnPressed, this,
                        [this, action = part.action] { emit actionRequested(action, {}); });
            }
            inputs_.insert(part.key, edit);
            add(edit);
            break;
        }
        case Kind::toggle: {
            // ADR-0304: its label, and a switch at the right.
            auto* line = new QWidget(parts_);
            auto* line_layout = new QHBoxLayout(line);
            line_layout->setContentsMargins(0, 2, 0, 2);
            line_layout->setSpacing(8);
            auto* label = plainLabel(part.text, line);
            label->setWordWrap(true);
            if (part.detail.isEmpty()) {
                line_layout->addWidget(label, 1);
            } else {
                // What it does, smaller, beneath its name.
                auto* lines = new QVBoxLayout;
                lines->setSpacing(0);
                lines->addWidget(label);
                auto* detail = plainLabel(part.detail, line);
                detail->setWordWrap(true);
                detail->setFont(smaller(detail->font()));
                detail->setForegroundRole(QPalette::PlaceholderText);
                lines->addWidget(detail);
                line_layout->addLayout(lines, 1);
            }
            auto* toggle = new PanelSwitch(line);
            toggle->setObjectName(QStringLiteral("bench-plugin-panel-toggle-%1").arg(part.key));
            toggle->setAccessibleName(part.text);
            toggle->setToolTip(part.text);
            toggle->setChecked(part.on);
            label->setBuddy(toggle);
            connect(toggle, &QAbstractButton::toggled, this,
                    [this, key = part.key](const bool on) {
                        emit actionRequested(key, on ? QStringLiteral("on") : QStringLiteral("off"));
                    });
            line_layout->addWidget(toggle, 0, Qt::AlignVCenter);
            add(line);
            break;
        }
        case Kind::status: {
            // ADR-0304: a dot -- the accent while it is live -- and a line.
            auto* line = new QWidget(parts_);
            line->setObjectName(QStringLiteral("bench-plugin-panel-status"));
            auto* line_layout = new QHBoxLayout(line);
            line_layout->setContentsMargins(0, 2, 0, 2);
            line_layout->setSpacing(7);
            const auto dot_colour = part.on ? palette().color(QPalette::Highlight)
                                            : faded(this, 90);
            line_layout->addWidget(new Dot(8, dot_colour, {}, line), 0, Qt::AlignVCenter);
            auto* text = plainLabel(part.text, line);
            auto font = text->font();
            font.setWeight(QFont::DemiBold);
            text->setFont(font);
            line_layout->addWidget(text);
            if (!part.detail.isEmpty()) {
                auto* detail = plainLabel(QStringLiteral("· %1").arg(part.detail), line);
                detail->setForegroundRole(QPalette::PlaceholderText);
                line_layout->addWidget(detail);
            }
            line_layout->addStretch(1);
            // Its buttons at the right, quiet ones too: there is no row to
            // hover here.
            for (const auto& each : part.buttons) {
                auto* button = rowButton(each, line);
                run_on_press(button, each);
                line_layout->addWidget(button, 0, Qt::AlignVCenter);
            }
            add(line);
            break;
        }
        case Kind::filters: {
            // Badges in a row, those on drawn as chosen.
            auto* row = new QWidget(parts_);
            row->setObjectName(QStringLiteral("bench-plugin-panel-filters"));
            auto* flow = new FlowLayout(row);
            for (const auto& each : part.buttons) {
                auto* button = rowButton(each, row);
                button->setObjectName(QStringLiteral("bench-plugin-panel-filter"));
                run_on_press(button, each);
                flow->addWidget(button);
            }
            add(row);
            break;
        }
        case Kind::people: {
            auto* row = new QWidget(parts_);
            row->setObjectName(QStringLiteral("bench-plugin-panel-people"));
            auto* flow = new FlowLayout(row);
            for (const auto& name : part.people) {
                flow->addWidget(new PersonChip(name, row));
            }
            add(row);
            break;
        }
        case Kind::track: {
            auto* row = new TrackRow(part, parts_);
            for (const auto& each : part.buttons) {
                auto* button = rowButton(each, row);
                run_on_press(button, each);
                row->addButton(button, each.role == PluginPanelContent::Role::quiet);
            }
            add(row);
            break;
        }
        case Kind::button: {
            // Buttons side by side, as links are; the primary one stands out.
            auto* row = new QWidget(parts_);
            auto* flow = new FlowLayout(row);
            for (; index < parts.size() && parts[index].kind == Kind::button; ++index) {
                const auto& pressed = parts[index];
                auto* button = new QPushButton(pressed.text, row);
                button->setObjectName(QStringLiteral("bench-plugin-panel-button"));
                // ADR-0304: the primary in the accent; a quiet one flat.
                button->setDefault(pressed.role == PluginPanelContent::Role::primary);
                button->setFlat(pressed.role == PluginPanelContent::Role::quiet);
                button->setProperty("role", pressed.role == PluginPanelContent::Role::quiet
                                                ? QStringLiteral("quiet")
                                                : QString{});
                button->setProperty("action", pressed.action);
                connect(button, &QPushButton::clicked, this,
                        [this, action = pressed.action, value = pressed.value] {
                            emit actionRequested(action, value);
                        });
                flow->addWidget(button);
            }
            --index;
            add(row);
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
            if (part.buttons.empty()) {
                add(item);
                break;
            }
            // ADR-0303: its buttons at its right -- a vote, say -- one's own
            // drawn as chosen.
            auto* line = new QWidget(parts_);
            auto* line_layout = new QHBoxLayout(line);
            line_layout->setContentsMargins(0, 0, 0, 0);
            line_layout->setSpacing(6);
            line_layout->addWidget(item, 1);
            for (const auto& each : part.buttons) {
                auto* button = rowButton(each, line);
                run_on_press(button, each);
                line_layout->addWidget(button, 0, Qt::AlignVCenter);
            }
            add(line);
            break;
        }
        case Kind::link: {
            // Links side by side: a row of small buttons, wrapping.
            auto* row = new QWidget(parts_);
            row->setObjectName(QStringLiteral("bench-plugin-panel-links"));
            auto* flow = new FlowLayout(row);
            row->setStyleSheet(
                QStringLiteral("QToolButton { border: 1px solid palette(mid); border-radius: 10px;"
                               " padding: 2px 9px; }"
                               "QToolButton:hover { background: palette(midlight); }"));
            for (; index < parts.size() && parts[index].kind == Kind::link; ++index) {
                const auto& link = parts[index];
                auto* button = new QToolButton(row);
                button->setObjectName(QStringLiteral("bench-plugin-panel-link"));
                button->setText(link.text);
                button->setCursor(Qt::PointingHandCursor);
                button->setToolTip(link.detail.isEmpty()
                                       ? link.url
                                       : QStringLiteral("%1 · %2").arg(link.detail, link.url));
                connect(button, &QToolButton::clicked, button,
                        [url = link.url] { QDesktopServices::openUrl(QUrl{url}); });
                flow->addWidget(button);
            }
            --index;
            add(row);
            break;
        }
        case Kind::image: {
            QImageReader reader{part.image};
            reader.setAutoTransform(true);
            const auto image = reader.read();
            if (image.isNull()) {
                break;
            }
            add(new FittedImage(image, part.url, parts_));
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
