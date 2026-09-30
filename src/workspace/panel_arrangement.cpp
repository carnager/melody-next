// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/panel_arrangement.hpp"

#include <QSettings>

#include <algorithm>
#include <utility>

namespace trackknife::bench {
namespace {

constexpr auto settings_key = "workspace/panel-layout-v1";

} // namespace

PanelArrangement::PanelArrangement(QObject* parent)
    : QObject(parent), layout_(defaultLayout()) {}

ui::PanelLayout PanelArrangement::defaultLayout() {
    std::vector<ui::PanelLayoutNode> children;
    children.push_back(ui::panelLayoutPanel(QString::fromLatin1(sources_panel)));
    children.push_back(ui::panelLayoutPanel(QString::fromLatin1(tracks_panel)));
    return ui::PanelLayout{
        .schema_version = ui::panel_layout_schema_version,
        .root = ui::panelLayoutSplit(Qt::Horizontal, std::move(children), {1, 3}),
    };
}

QString PanelArrangement::load() {
    const auto encoded = QSettings{}.value(QString::fromLatin1(settings_key)).toByteArray();
    protected_ = false;
    if (encoded.isEmpty()) {
        layout_ = defaultLayout();
        emit changed();
        return {};
    }
    QString error;
    auto restored = ui::deserializePanelLayout(
        encoded, {QString::fromLatin1(sources_panel), QString::fromLatin1(tracks_panel)}, &error);
    if (!restored) {
        protected_ = true;
        layout_ = defaultLayout();
        emit changed();
        return error;
    }
    layout_ = std::move(*restored);
    emit changed();
    return {};
}

void PanelArrangement::setEditing(const bool editing) {
    if (editing != editing_) {
        editing_ = editing;
        emit editingChanged();
    }
}

void PanelArrangement::persist() {
    if (protected_) {
        return;
    }
    QSettings{}.setValue(QString::fromLatin1(settings_key), ui::serializePanelLayout(layout_));
}

void PanelArrangement::adopt(ui::PanelLayout layout, const bool by_hand) {
    if (by_hand) {
        protected_ = false;
    }
    layout_ = std::move(layout);
    persist();
}

void PanelArrangement::arrange(const ui::PanelLayoutNodeKind kind,
                               const Qt::Orientation orientation) {
    auto root = layout_.root;
    if (kind == ui::PanelLayoutNodeKind::panel || root.children.empty()) {
        return;
    }
    auto children = std::move(root.children);
    ui::PanelLayoutNode replacement;
    if (kind == ui::PanelLayoutNodeKind::split) {
        auto weights = root.kind == ui::PanelLayoutNodeKind::split
                           ? std::move(root.weights)
                           : std::vector<int>(children.size(), 1);
        replacement = ui::panelLayoutSplit(orientation, std::move(children), std::move(weights));
    } else {
        auto active = root.kind == ui::PanelLayoutNodeKind::tabs ? root.active_child : 0;
        if (root.kind != ui::PanelLayoutNodeKind::tabs) {
            const auto track_lists = std::ranges::find(
                children, QString::fromLatin1(tracks_panel), &ui::PanelLayoutNode::panel_id);
            if (track_lists != children.end()) {
                active = static_cast<int>(std::distance(children.begin(), track_lists));
            }
        }
        replacement = ui::panelLayoutTabs(std::move(children), active);
    }
    protected_ = false;
    layout_ = ui::PanelLayout{.schema_version = ui::panel_layout_schema_version,
                              .root = std::move(replacement)};
    persist();
    emit changed();
}

void PanelArrangement::swap() {
    auto root = layout_.root;
    if (root.children.size() < 2U) {
        return;
    }
    std::ranges::reverse(root.children);
    if (root.kind == ui::PanelLayoutNodeKind::split) {
        std::ranges::reverse(root.weights);
    } else if (root.kind == ui::PanelLayoutNodeKind::tabs) {
        root.active_child = static_cast<int>(root.children.size()) - 1 - root.active_child;
    }
    protected_ = false;
    layout_ = ui::PanelLayout{.schema_version = ui::panel_layout_schema_version,
                              .root = std::move(root)};
    persist();
    emit changed();
}

void PanelArrangement::reset() {
    protected_ = false;
    layout_ = defaultLayout();
    persist();
    emit changed();
}

} // namespace trackknife::bench
