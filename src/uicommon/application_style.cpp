// SPDX-License-Identifier: GPL-3.0-only
#include "uicommon/application_style.hpp"

#include <QApplication>
#include <QStyle>
#include <QStyleFactory>

#include <utility>

namespace trackknife::ui {
namespace {

std::function<QStyle*()>& factory() {
    static std::function<QStyle*()> made;
    return made;
}

} // namespace

QStyle* createApplicationStyle() {
    if (factory()) {
        return factory()();
    }
    const auto name = QApplication::style()->name();
    return QStyleFactory::create(name.isEmpty() ? QStringLiteral("Fusion") : name);
}

void setApplicationStyleFactory(std::function<QStyle*()> made) { factory() = std::move(made); }

} // namespace trackknife::ui
