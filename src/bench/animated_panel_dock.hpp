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
        QTimer::singleShot(0, this, [this] {
            if (wanted_ && isVisible() && animation_.state() != QAbstractAnimation::Running &&
                width() >= 260) {
                savedWidth_ = width();
                QSettings{}.setValue(key_ + "/width", savedWidth_);
            }
        });
    }

  private:
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
