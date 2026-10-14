// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_list_model.hpp"
#include "trackknife/lists/list_view.hpp"

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QTimer>

#include <optional>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;

namespace trackknife::ui {
class QueueTableView;
}

namespace trackknife::bench {

// ADR-0282, ADR-0284: edits one list view. Its columns are a list, each
// chosen to edit what it shows and what beneath; its rows and its grouping
// are pages of their own. Beneath every expression, what it gives for the
// list's first track or what is wrong with it; below it all, the list's first
// rows drawn by the view as edited. A view with an error is not saved.
class ListViewDialog final : public QDialog {
    Q_OBJECT

  public:
    // `rows`, `covers`: a few of the list's own rows and their albums'
    // covers, to preview. `removable`: a view of the user's own already saved.
    ListViewDialog(lists::ListView view, std::vector<LocalTrackRow> rows,
                   const QHash<QString, QImage>& covers, bool removable, QWidget* parent);

    [[nodiscard]] const lists::ListView& view() const noexcept { return view_; }
    // The preview's rows: their listening history, where the view reads it,
    // is asked of the list's engine by whoever opened the editor.
    [[nodiscard]] LocalListModel* previewModel() const noexcept { return preview_model_; }

  signals:
    // The view as edited, to be saved.
    void saved(const lists::ListView& view);
    void removed(const QString& id);

  private:
    // An expression's field and the line beneath it saying what it gives.
    struct Field {
        QLineEdit* edit{nullptr};
        QLabel* note{nullptr};
        titleformat::FormatContextKind context{titleformat::FormatContextKind::list_item};
        // A group header's, read with group_tracks and group_length.
        bool header{false};
        // Where a problem with it is, as validate_list_view says.
        std::string where;
    };
    [[nodiscard]] QWidget* expressionField(Field& field, const char* name,
                                           const QString& placeholder,
                                           titleformat::FormatContextKind context);
    [[nodiscard]] QWidget* columnsPage();
    [[nodiscard]] QWidget* rowsPage();
    [[nodiscard]] QWidget* groupingPage();
    void fill();
    void fillColumnList();
    void showColumn(int row);
    void readColumn();
    void readEdits();
    void edited();
    void check();
    // Says beneath the field what it gives, or its problem.
    void note(Field& field, const std::vector<lists::ViewProblem>& problems) const;
    void preview();
    // The first track again, once more is known of it (its history).
    void refreshExample();
    void addColumn();
    void removeColumn();
    void moveColumn(int by);
    [[nodiscard]] lists::ViewColumn* currentColumn();

    lists::ListView view_;
    bool filling_{false};
    int shown_column_{-1};
    QTimer preview_timer_;
    // The list's first track, which the notes are about.
    std::optional<lists::OwnedViewRow> example_;
    std::size_t example_tracks_{0U};

    QLineEdit* name_{nullptr};
    // Columns.
    QListWidget* columns_{nullptr};
    QPushButton* remove_column_{nullptr};
    QStackedWidget* column_page_{nullptr};
    QLabel* builtin_note_{nullptr};
    QLineEdit* column_title_{nullptr};
    Field column_display_;
    Field column_second_;
    Field builtin_second_;
    Field column_sort_;
    Field column_style_;
    QComboBox* column_align_{nullptr};
    QCheckBox* column_expands_{nullptr};
    // Rows.
    QComboBox* row_lines_{nullptr};
    QComboBox* artwork_{nullptr};
    Field row_style_;
    // Grouping.
    QComboBox* grouping_{nullptr};
    QWidget* expression_grouping_{nullptr};
    Field group_key_;
    Field group_title_;
    Field group_details_;
    QCheckBox* subgroup_{nullptr};
    Field subgroup_key_;
    Field subgroup_label_;
    // The rest.
    QLabel* problems_{nullptr};
    LocalListModel* preview_model_{nullptr};
    ui::QueueTableView* preview_{nullptr};
    QDialogButtonBox* buttons_{nullptr};
};

} // namespace trackknife::bench
