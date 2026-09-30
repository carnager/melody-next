// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "workspace/workspace.hpp"

#include <QAbstractListModel>

#include <vector>

namespace trackknife::quick {

// The open lists in the order their tabs show them, as the tab bar draws
// them: the chrome is the workspace's (Workspace::tabChrome), so it reads
// as the widgets window's does.
class ListTabsModel final : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Role : int {
        text_role = Qt::UserRole + 1,
        tooltip_role,
        playing_role,
        remote_role,
        pinned_role,
        dirty_role,
        name_role,
        document_role,
        model_role,
    };

    explicit ListTabsModel(bench::Workspace& workspace, QObject* parent);

    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] const std::vector<bench::Workspace::ListTab*>& tabs() const { return tabs_; }
    [[nodiscard]] bench::Workspace::ListTab* at(int row) const;
    [[nodiscard]] int indexOf(const bench::Workspace::ListTab* tab) const;
    // Added where its engine's tabs end, so tabs stay grouped by engine in
    // engine order, this computer's first (ADR-0234).
    int insert(bench::Workspace::ListTab& tab);
    void remove(int row);
    void move(int from, int to);
    void refresh(const bench::Workspace::ListTab& tab);

  private:
    [[nodiscard]] std::size_t rankOf(const bench::Workspace::ListTab& tab) const;

    bench::Workspace& workspace_;
    std::vector<bench::Workspace::ListTab*> tabs_;
};

} // namespace trackknife::quick
