// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/lists/list_view.hpp"

#include <QDialog>
#include <QTimer>

#include <vector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTreeWidget;

namespace trackknife::bench {

// ADR-0282: edits one list view -- its name, artwork, grouping and columns --
// with each expression's error beside it and the current list's first rows
// previewed as it would show them. A view with an error is not saved.
class ListViewDialog final : public QDialog {
    Q_OBJECT

  public:
    // `rows`: what to preview, a few of the list's own. `removable`: a view of
    // the user's own already saved, which can be removed.
    ListViewDialog(lists::ListView view, std::vector<lists::OwnedViewRow> rows, bool removable,
                   QWidget* parent);

    [[nodiscard]] const lists::ListView& view() const noexcept { return view_; }

  signals:
    // The view as edited, to be saved.
    void saved(const lists::ListView& view);
    void removed(const QString& id);

  private:
    void fill();
    void fillColumns();
    void readEdits();
    void edited();
    void check();
    void preview();
    void addColumn();
    void removeColumn();
    void moveColumn(int by);

    lists::ListView view_;
    std::vector<lists::OwnedViewRow> rows_;
    bool filling_{false};
    QTimer preview_timer_;
    QLineEdit* name_{nullptr};
    QComboBox* artwork_{nullptr};
    QComboBox* grouping_{nullptr};
    QLineEdit* row_style_{nullptr};
    QComboBox* row_lines_{nullptr};
    QLineEdit* group_key_{nullptr};
    QLineEdit* group_title_{nullptr};
    QLineEdit* group_details_{nullptr};
    QCheckBox* subgroup_{nullptr};
    QLineEdit* subgroup_key_{nullptr};
    QLineEdit* subgroup_label_{nullptr};
    QWidget* expression_grouping_{nullptr};
    QTableWidget* columns_{nullptr};
    QPushButton* remove_column_{nullptr};
    QLabel* problems_{nullptr};
    QTreeWidget* preview_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
};

} // namespace trackknife::bench
