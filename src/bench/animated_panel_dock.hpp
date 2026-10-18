// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QAction>
#include <QDockWidget>
#include <QMainWindow>
#include <QResizeEvent>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QVariantAnimation>

#include <algorithm>

namespace trackknife::bench {

// A clipping viewport keeps expensive track layouts steady during a reveal.
class PanelViewport final : public QWidget {
  public:
    using QWidget::QWidget;
    QWidget* content{nullptr};
    int revealWidth{0};
    QSize sizeHint() const override { return {420, 400}; }
    QSize minimumSizeHint() const override { return {0, 0}; }
    void arrange() {
        if (content)
            content->setGeometry(0, 0, revealWidth > 0 ? revealWidth : width(), height());
    }

  protected:
    void resizeEvent(QResizeEvent*) override { arrange(); }
};

class AnimatedPanelDock final : public QDockWidget {
  public:
    AnimatedPanelDock(const QString& title, const QString& settingsKey, QMainWindow* parent)
        : QDockWidget(title, parent), key_(settingsKey), viewport_(new PanelViewport(this)),
          animation_(this) {
        toggle_ = new QAction(title, this);
        toggle_->setCheckable(true);
        connect(toggle_, &QAction::triggered, this, [this](bool checked) { setVisible(checked); });
        setFeatures(QDockWidget::NoDockWidgetFeatures);
        auto* titleBar = new QWidget(this);
        titleBar->setFixedHeight(0);
        setTitleBarWidget(titleBar);
        QDockWidget::setWidget(viewport_);
        savedWidth_ = qBound(260, QSettings{}.value(key_ + "/width", 300).toInt(), 1200);
        animation_.setDuration(180);
        animation_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&animation_, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& value) { setFixedWidth(value.toInt()); });
        connect(&animation_, &QVariantAnimation::finished, this, [this] { finish(); });
    }

    QAction* panelToggleAction() const { return toggle_; }

    // The width it is on its way to, sliding; else the one it has.
    [[nodiscard]] int headingWidth() const {
        return animation_.state() == QAbstractAnimation::Running ? animation_.endValue().toInt()
                                                                 : width();
    }

    void setPanelContent(QWidget* content) {
        content->setParent(viewport_);
        viewport_->content = content;
        content->show();
        viewport_->arrange();
    }

    void setVisible(bool visible) override {
        if (visible && wanted_ && isVisible() && animation_.state() != QAbstractAnimation::Running)
            return;
        if (visible == wanted_ && animation_.state() == QAbstractAnimation::Running)
            return;
        // ADR-0292: opened beside another side panel, it joins it as a tab;
        // tabbed with another open, it is a tab coming or going -- the room
        // the others have stays as it is.
        const bool joined = visible && !isVisible() && joinAnOpenPanel();
        if (joined || sharesItsTabs()) {
            animation_.stop();
            wanted_ = visible;
            QSettings{}.setValue(key_ + "/visible", visible);
            // Laid out at the width its tabs are on their way to, so what it
            // shows does not reflow while they slide in; let go once there.
            if (visible) {
                viewport_->revealWidth = std::max(260, groupHeading());
                viewport_->arrange();
                QTimer::singleShot(animation_.duration() + 80, this, [this] {
                    viewport_->revealWidth = 0;
                    viewport_->arrange();
                });
            }
            QDockWidget::setVisible(visible);
            // In front, once the tab bar it joined is there.
            if (visible)
                QTimer::singleShot(0, this, [this] { raise(); });
            const QSignalBlocker blocker(panelToggleAction());
            panelToggleAction()->setChecked(visible);
            return;
        }
        if (!visible && isVisible() && animation_.state() != QAbstractAnimation::Running)
            savedWidth_ = width();
        const auto currentWidth = isVisible() ? width() : 1;
        animation_.stop();
        wanted_ = visible;
        QSettings{}.setValue(key_ + "/visible", visible);
        QSettings{}.setValue(key_ + "/width", savedWidth_);
        if ((!visible && isHidden()) || !parentWidget()->isVisible() ||
            !QSettings{}.value(QStringLiteral("appearance/panel-animations"), true).toBool()) {
            finish();
            return;
        }
        if (visible && isVisible() && currentWidth == savedWidth_) {
            finish();
            return;
        }
        savedWidth_ = qBound(260, savedWidth_, qMax(260, parentWidget()->width() / 2));
        viewport_->revealWidth = savedWidth_;
        viewport_->arrange();
        setFixedWidth(currentWidth);
        QDockWidget::setVisible(true);
        {
            const QSignalBlocker blocker(panelToggleAction());
            panelToggleAction()->setChecked(visible);
        }
        animation_.setStartValue(currentWidth);
        animation_.setEndValue(visible ? savedWidth_ : 1);
        animation_.start();
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QDockWidget::resizeEvent(event);
        if (animation_.state() == QAbstractAnimation::Running)
            return;
        // Joined while the others slid in, and now as wide as they went.
        if (viewport_->revealWidth > 0 && width() >= viewport_->revealWidth) {
            viewport_->revealWidth = 0;
            viewport_->arrange();
        }
        QTimer::singleShot(0, this, [this] {
            if (wanted_ && isVisible() && animation_.state() != QAbstractAnimation::Running &&
                width() >= 260) {
                savedWidth_ = width();
                QSettings{}.setValue(key_ + "/width", savedWidth_);
            }
        });
    }

  private:
    // Where the side panels it is tabbed with are heading.
    [[nodiscard]] int groupHeading() const {
        const auto* window = qobject_cast<const QMainWindow*>(parentWidget());
        int heading = savedWidth_;
        if (window == nullptr)
            return heading;
        for (auto* other : window->tabifiedDockWidgets(const_cast<AnimatedPanelDock*>(this)))
            if (const auto* panel = dynamic_cast<const AnimatedPanelDock*>(other);
                panel != nullptr && panel->isVisible())
                heading = panel->headingWidth();
        return heading;
    }
    bool joinAnOpenPanel() {
        auto* window = qobject_cast<QMainWindow*>(parentWidget());
        if (window == nullptr)
            return false;
        for (auto* other : window->findChildren<QDockWidget*>()) {
            if (other != this && dynamic_cast<AnimatedPanelDock*>(other) != nullptr &&
                other->isVisible() &&
                window->dockWidgetArea(other) == window->dockWidgetArea(this)) {
                window->tabifyDockWidget(other, this);
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool sharesItsTabs() const {
        const auto* window = qobject_cast<const QMainWindow*>(parentWidget());
        if (window == nullptr)
            return false;
        for (const auto* other : window->tabifiedDockWidgets(const_cast<AnimatedPanelDock*>(this)))
            if (other != this && other->isVisible())
                return true;
        return false;
    }
    void finish() {
        if (!wanted_)
            QDockWidget::setVisible(false);
        setMinimumWidth(260);
        setMaximumWidth(QWIDGETSIZE_MAX);
        viewport_->revealWidth = 0;
        if (wanted_) {
            QDockWidget::setVisible(true);
            static_cast<QMainWindow*>(parentWidget())
                ->resizeDocks({this}, {savedWidth_}, Qt::Horizontal);
        }
        viewport_->arrange();
        const QSignalBlocker blocker(panelToggleAction());
        panelToggleAction()->setChecked(wanted_);
    }
    QAction* toggle_{nullptr};
    QString key_;
    PanelViewport* viewport_;
    QVariantAnimation animation_;
    int savedWidth_{300};
    bool wanted_{false};
};
} // namespace trackknife::bench
