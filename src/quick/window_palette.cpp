// SPDX-License-Identifier: GPL-3.0-only
#include "quick/window_palette.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QPalette>
#include <QQuickWindow>

#include <array>
#include <utility>

namespace trackknife::quick {
namespace {

constexpr std::array roles{
    std::pair{QPalette::Window, "window"},
    std::pair{QPalette::WindowText, "windowText"},
    std::pair{QPalette::Base, "base"},
    std::pair{QPalette::AlternateBase, "alternateBase"},
    std::pair{QPalette::Text, "text"},
    std::pair{QPalette::Button, "button"},
    std::pair{QPalette::ButtonText, "buttonText"},
    std::pair{QPalette::BrightText, "brightText"},
    std::pair{QPalette::Highlight, "highlight"},
    std::pair{QPalette::HighlightedText, "highlightedText"},
    std::pair{QPalette::Link, "link"},
    std::pair{QPalette::LinkVisited, "linkVisited"},
    std::pair{QPalette::PlaceholderText, "placeholderText"},
    std::pair{QPalette::ToolTipBase, "toolTipBase"},
    std::pair{QPalette::ToolTipText, "toolTipText"},
    std::pair{QPalette::Light, "light"},
    std::pair{QPalette::Midlight, "midlight"},
    std::pair{QPalette::Mid, "mid"},
    std::pair{QPalette::Dark, "dark"},
    std::pair{QPalette::Shadow, "shadow"},
    std::pair{QPalette::Accent, "accent"},
};

void recolor(QObject* window) {
    auto* palette = window->property("palette").value<QObject*>();
    if (palette == nullptr) {
        return;
    }
    const auto application = QGuiApplication::palette();
    for (const auto& [role, name] : roles) {
        palette->setProperty(name, application.color(QPalette::Active, role));
    }
    // Disabled the same: the Trackknife style fades a disabled control
    // itself, and a palette's disabled greys under that fade read as nothing.
    if (auto* disabled = palette->property("disabled").value<QObject*>(); disabled != nullptr) {
        for (const auto& [role, name] : roles) {
            disabled->setProperty(name, application.color(QPalette::Active, role));
        }
    }
}

} // namespace

WindowPalettes::WindowPalettes(QObject* parent) : QObject(parent) {
    QGuiApplication::instance()->installEventFilter(this);
}

void WindowPalettes::recolorAll() {
    for (auto* window : QGuiApplication::topLevelWindows()) {
        if (qobject_cast<QQuickWindow*>(window) != nullptr) {
            recolor(window);
        }
    }
}

bool WindowPalettes::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::ApplicationPaletteChange && watched == QGuiApplication::instance()) {
        recolorAll();
    } else if (event->type() == QEvent::Show && qobject_cast<QQuickWindow*>(watched) != nullptr) {
        recolor(watched);
    }
    return QObject::eventFilter(watched, event);
}

} // namespace trackknife::quick
