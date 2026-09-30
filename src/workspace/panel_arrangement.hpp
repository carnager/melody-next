// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "uicommon/panel_layout.hpp"

#include <QObject>
#include <QString>

namespace trackknife::bench {

// How the window's two panels -- the sources and the lists with their
// tracks -- are arranged: side by side, one above the other, or as tabs;
// in which order, and how much of the room each has. Changed while editing
// the layout, and kept. Both windows arrange their panels by it.
class PanelArrangement final : public QObject {
    Q_OBJECT

  public:
    static constexpr auto sources_panel = "folders";
    static constexpr auto tracks_panel = "track-lists";

    explicit PanelArrangement(QObject* parent = nullptr);

    // The kept arrangement, or the default; what kept it from being read,
    // if something did -- then it is left as it was until changed here.
    [[nodiscard]] QString load();
    [[nodiscard]] const ui::PanelLayout& layout() const { return layout_; }
    [[nodiscard]] static ui::PanelLayout defaultLayout();

    [[nodiscard]] bool editing() const { return editing_; }
    void setEditing(bool editing);

    // The arrangement as the window has it now -- a divider moved
    // (`by_hand`), a tab chosen -- kept.
    void adopt(ui::PanelLayout layout, bool by_hand);
    void arrange(ui::PanelLayoutNodeKind kind, Qt::Orientation orientation);
    void swap();
    void reset();

  signals:
    void changed();
    void editingChanged();

  private:
    void persist();

    ui::PanelLayout layout_;
    bool protected_{false};
    bool editing_{false};
};

} // namespace trackknife::bench
