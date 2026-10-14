// SPDX-License-Identifier: GPL-3.0-only

#include "bench/list_view_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

namespace trackknife::bench {
namespace {

enum Column : int {
    shown_column,
    title_column,
    display_column,
    sort_column,
    style_column,
    align_column,
    expands_column,
    column_count
};

// The built-in columns as the header names them.
[[nodiscard]] QString builtin_title(const std::string& id) {
    if (id == "artwork") {
        return QStringLiteral("Artwork");
    }
    if (id == "track-number") {
        return QStringLiteral("#");
    }
    if (id == "play-count") {
        return QStringLiteral("Play count");
    }
    if (id == "last-played") {
        return QStringLiteral("Last played");
    }
    auto title = QString::fromStdString(id);
    title[0] = title[0].toUpper();
    return title;
}

[[nodiscard]] QString text(const std::string& value) { return QString::fromStdString(value); }

} // namespace

ListViewDialog::ListViewDialog(lists::ListView view, std::vector<lists::OwnedViewRow> rows,
                               const bool removable, QWidget* parent)
    : QDialog(parent), view_(std::move(view)), rows_(std::move(rows)) {
    setObjectName(QStringLiteral("bench-list-view-dialog"));
    setWindowTitle(QStringLiteral("List view"));
    resize(900, 640);

    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("bench-list-view-name"));
    artwork_ = new QComboBox(this);
    artwork_->setObjectName(QStringLiteral("bench-list-view-artwork"));
    artwork_->addItems({QStringLiteral("Beside the rows"), QStringLiteral("In the group header"),
                        QStringLiteral("None")});
    grouping_ = new QComboBox(this);
    grouping_->setObjectName(QStringLiteral("bench-list-view-grouping"));
    grouping_->addItems({QStringLiteral("None"), QStringLiteral("Album, discs within"),
                         QStringLiteral("By an expression")});

    const auto expression_field = [this](const char* name, const QString& placeholder) {
        auto* field = new QLineEdit(this);
        field->setObjectName(QString::fromLatin1(name));
        field->setPlaceholderText(placeholder);
        connect(field, &QLineEdit::textEdited, this, &ListViewDialog::edited);
        return field;
    };
    group_key_ = expression_field("bench-list-view-group-key", QStringLiteral("%label%"));
    group_title_ = expression_field("bench-list-view-group-title", QStringLiteral("%label%"));
    group_details_ = expression_field("bench-list-view-group-details",
                                      QStringLiteral("%group_tracks% tracks · %group_length%"));
    subgroup_ = new QCheckBox(QStringLiteral("Subgroups within, where there are several"), this);
    subgroup_->setObjectName(QStringLiteral("bench-list-view-subgroup"));
    subgroup_key_ =
        expression_field("bench-list-view-subgroup-key", QStringLiteral("%discnumber%"));
    subgroup_label_ =
        expression_field("bench-list-view-subgroup-label", QStringLiteral("Disc %discnumber%"));
    expression_grouping_ = new QWidget(this);
    auto* grouping_form = new QFormLayout(expression_grouping_);
    grouping_form->setContentsMargins(0, 0, 0, 0);
    grouping_form->addRow(QStringLiteral("Group by"), group_key_);
    grouping_form->addRow(QStringLiteral("Header"), group_title_);
    grouping_form->addRow(QStringLiteral("Details"), group_details_);
    grouping_form->addRow(subgroup_);
    grouping_form->addRow(QStringLiteral("Subgroup by"), subgroup_key_);
    grouping_form->addRow(QStringLiteral("Subgroup label"), subgroup_label_);

    // ADR-0283: style words a row is drawn by.
    row_style_ =
        expression_field("bench-list-view-row-style", QStringLiteral("$if(%playcount%,,quiet)"));
    row_style_->setToolTip(
        QStringLiteral("Answers style words: quiet, accent, warning, bold, italic"));
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("Name"), name_);
    form->addRow(QStringLiteral("Artwork"), artwork_);
    form->addRow(QStringLiteral("Row style"), row_style_);
    form->addRow(QStringLiteral("Grouping"), grouping_);
    form->addRow(expression_grouping_);

    columns_ = new QTableWidget(0, column_count, this);
    columns_->setObjectName(QStringLiteral("bench-list-view-columns"));
    columns_->setHorizontalHeaderLabels({QStringLiteral("Shown"), QStringLiteral("Title"),
                                         QStringLiteral("Shows"), QStringLiteral("Sorts by"),
                                         QStringLiteral("Style"), QStringLiteral("Align"),
                                         QStringLiteral("Expands")});
    columns_->verticalHeader()->hide();
    columns_->setSelectionBehavior(QAbstractItemView::SelectRows);
    columns_->setSelectionMode(QAbstractItemView::SingleSelection);
    columns_->horizontalHeader()->setSectionResizeMode(display_column, QHeaderView::Stretch);
    columns_->horizontalHeader()->setSectionResizeMode(sort_column, QHeaderView::Stretch);
    columns_->horizontalHeader()->setSectionResizeMode(style_column, QHeaderView::Stretch);
    connect(columns_, &QTableWidget::itemChanged, this, &ListViewDialog::edited);
    auto* add = new QPushButton(QStringLiteral("Add column"), this);
    add->setObjectName(QStringLiteral("bench-list-view-add-column"));
    connect(add, &QPushButton::clicked, this, &ListViewDialog::addColumn);
    remove_column_ = new QPushButton(QStringLiteral("Remove column"), this);
    remove_column_->setObjectName(QStringLiteral("bench-list-view-remove-column"));
    connect(remove_column_, &QPushButton::clicked, this, &ListViewDialog::removeColumn);
    auto* up = new QPushButton(QStringLiteral("Up"), this);
    connect(up, &QPushButton::clicked, this, [this] { moveColumn(-1); });
    auto* down = new QPushButton(QStringLiteral("Down"), this);
    connect(down, &QPushButton::clicked, this, [this] { moveColumn(1); });
    connect(columns_, &QTableWidget::currentCellChanged, this, [this](const int row) {
        remove_column_->setEnabled(row >= 0 && row < static_cast<int>(view_.columns.size()) &&
                                   !view_.columns[static_cast<std::size_t>(row)].builtin());
    });
    auto* column_buttons = new QHBoxLayout;
    column_buttons->addWidget(add);
    column_buttons->addWidget(remove_column_);
    column_buttons->addStretch();
    column_buttons->addWidget(up);
    column_buttons->addWidget(down);

    problems_ = new QLabel(this);
    problems_->setObjectName(QStringLiteral("bench-list-view-problems"));
    problems_->setWordWrap(true);
    problems_->setTextFormat(Qt::PlainText);

    preview_ = new QTreeWidget(this);
    preview_->setObjectName(QStringLiteral("bench-list-view-preview"));
    preview_->setRootIsDecorated(false);
    preview_->setUniformRowHeights(true);
    preview_timer_.setSingleShot(true);
    preview_timer_.setInterval(150);
    connect(&preview_timer_, &QTimer::timeout, this, &ListViewDialog::preview);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    if (removable) {
        auto* remove =
            buttons_->addButton(QStringLiteral("Remove view"), QDialogButtonBox::DestructiveRole);
        remove->setObjectName(QStringLiteral("bench-list-view-remove"));
        connect(remove, &QPushButton::clicked, this, [this] {
            emit removed(text(view_.id));
            reject();
        });
    }
    connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        readEdits();
        if (lists::validate_list_view(view_).empty()) {
            emit saved(view_);
            accept();
        }
    });
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* editor = new QWidget(this);
    auto* editor_layout = new QVBoxLayout(editor);
    editor_layout->setContentsMargins(0, 0, 0, 0);
    editor_layout->addLayout(form);
    editor_layout->addWidget(columns_, 1);
    editor_layout->addLayout(column_buttons);
    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(editor);
    splitter->addWidget(preview_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(splitter, 1);
    layout->addWidget(problems_);
    layout->addWidget(buttons_);

    connect(name_, &QLineEdit::textEdited, this, &ListViewDialog::edited);
    connect(artwork_, &QComboBox::currentIndexChanged, this, &ListViewDialog::edited);
    connect(grouping_, &QComboBox::currentIndexChanged, this, &ListViewDialog::edited);
    connect(subgroup_, &QCheckBox::toggled, this, &ListViewDialog::edited);
    fill();
    check();
    preview();
}

void ListViewDialog::fill() {
    filling_ = true;
    name_->setText(text(view_.name));
    artwork_->setCurrentIndex(view_.artwork == lists::ViewArtwork::beside   ? 0
                              : view_.artwork == lists::ViewArtwork::header ? 1
                                                                            : 2);
    grouping_->setCurrentIndex(view_.grouping == lists::ViewGrouping::none    ? 0
                               : view_.grouping == lists::ViewGrouping::album ? 1
                                                                              : 2);
    row_style_->setText(text(view_.row_style));
    group_key_->setText(text(view_.group_key));
    group_title_->setText(text(view_.group_title));
    group_details_->setText(text(view_.group_details));
    subgroup_->setChecked(view_.subgroup.has_value());
    subgroup_key_->setText(view_.subgroup ? text(view_.subgroup->key) : QString{});
    subgroup_label_->setText(view_.subgroup ? text(view_.subgroup->label) : QString{});
    fillColumns();
    filling_ = false;
}

void ListViewDialog::fillColumns() {
    const bool was = filling_;
    filling_ = true;
    columns_->setRowCount(static_cast<int>(view_.columns.size()));
    for (int row = 0; row < static_cast<int>(view_.columns.size()); ++row) {
        const auto& column = view_.columns[static_cast<std::size_t>(row)];
        auto* shown = new QTableWidgetItem;
        shown->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        shown->setCheckState(column.visible ? Qt::Checked : Qt::Unchecked);
        columns_->setItem(row, shown_column, shown);
        const auto editable = Qt::ItemIsEnabled | Qt::ItemIsEditable | Qt::ItemIsSelectable;
        const auto fixed = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        auto* title =
            new QTableWidgetItem(column.builtin() ? builtin_title(column.id) : text(column.title));
        title->setFlags(column.builtin() ? fixed : editable);
        columns_->setItem(row, title_column, title);
        auto* display = new QTableWidgetItem(column.builtin() ? QStringLiteral("built in")
                                                              : text(column.display));
        display->setFlags(column.builtin() ? fixed : editable);
        columns_->setItem(row, display_column, display);
        auto* sort = new QTableWidgetItem(column.builtin() ? QString{} : text(column.sort));
        sort->setFlags(column.builtin() ? fixed : editable);
        sort->setToolTip(QStringLiteral("Empty: sorted by what it shows"));
        columns_->setItem(row, sort_column, sort);
        auto* style = new QTableWidgetItem(column.builtin() ? QString{} : text(column.style));
        style->setFlags(column.builtin() ? fixed : editable);
        style->setToolTip(QStringLiteral("Empty: as its row"));
        columns_->setItem(row, style_column, style);
        auto* align = new QComboBox(columns_);
        align->addItems(
            {QStringLiteral("Left"), QStringLiteral("Right"), QStringLiteral("Centre")});
        align->setCurrentIndex(column.alignment == lists::ColumnAlignment::right    ? 1
                               : column.alignment == lists::ColumnAlignment::center ? 2
                                                                                    : 0);
        align->setEnabled(!column.builtin());
        connect(align, &QComboBox::currentIndexChanged, this, &ListViewDialog::edited);
        columns_->setCellWidget(row, align_column, align);
        auto* expands = new QTableWidgetItem;
        expands->setFlags(column.builtin()
                              ? fixed
                              : Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        if (!column.builtin()) {
            expands->setCheckState(column.expands ? Qt::Checked : Qt::Unchecked);
        }
        columns_->setItem(row, expands_column, expands);
    }
    filling_ = was;
}

void ListViewDialog::readEdits() {
    view_.name = name_->text().trimmed().toStdString();
    view_.artwork = artwork_->currentIndex() == 0   ? lists::ViewArtwork::beside
                    : artwork_->currentIndex() == 1 ? lists::ViewArtwork::header
                                                    : lists::ViewArtwork::none;
    view_.grouping = grouping_->currentIndex() == 0   ? lists::ViewGrouping::none
                     : grouping_->currentIndex() == 1 ? lists::ViewGrouping::album
                                                      : lists::ViewGrouping::expression;
    view_.row_style = row_style_->text().toStdString();
    view_.group_key = group_key_->text().toStdString();
    view_.group_title = group_title_->text().toStdString();
    view_.group_details = group_details_->text().toStdString();
    view_.subgroup =
        subgroup_->isChecked()
            ? std::optional{lists::ViewSubgroup{.key = subgroup_key_->text().toStdString(),
                                                .label = subgroup_label_->text().toStdString()}}
            : std::nullopt;
    for (int row = 0; row < columns_->rowCount(); ++row) {
        auto& column = view_.columns[static_cast<std::size_t>(row)];
        column.visible = columns_->item(row, shown_column)->checkState() == Qt::Checked;
        if (column.builtin()) {
            continue;
        }
        column.title = columns_->item(row, title_column)->text().toStdString();
        column.display = columns_->item(row, display_column)->text().toStdString();
        column.sort = columns_->item(row, sort_column)->text().toStdString();
        column.style = columns_->item(row, style_column)->text().toStdString();
        const auto* align = qobject_cast<QComboBox*>(columns_->cellWidget(row, align_column));
        column.alignment = align == nullptr || align->currentIndex() == 0
                               ? lists::ColumnAlignment::left
                           : align->currentIndex() == 1 ? lists::ColumnAlignment::right
                                                        : lists::ColumnAlignment::center;
        column.expands = columns_->item(row, expands_column)->checkState() == Qt::Checked;
    }
}

void ListViewDialog::edited() {
    if (filling_) {
        return;
    }
    readEdits();
    check();
    preview_timer_.start();
}

void ListViewDialog::check() {
    const auto by_expression = grouping_->currentIndex() == 2;
    expression_grouping_->setVisible(by_expression);
    subgroup_key_->setEnabled(subgroup_->isChecked());
    subgroup_label_->setEnabled(subgroup_->isChecked());
    const auto problems = lists::validate_list_view(view_);
    // Each expression's error where it is.
    const auto mark = [&problems](QLineEdit* field, const std::string& where) {
        const auto found = std::ranges::find(problems, where, &lists::ViewProblem::where);
        field->setToolTip(found == problems.end() ? QString{} : text(found->message));
        field->setStyleSheet(found == problems.end() ? QString{}
                                                     : QStringLiteral("color: palette(link)"));
    };
    mark(row_style_, "view:row_style");
    mark(group_key_, "group:key");
    mark(group_title_, "group:title");
    mark(group_details_, "group:details");
    mark(subgroup_key_, "subgroup:key");
    mark(subgroup_label_, "subgroup:label");
    const bool was = filling_;
    filling_ = true;
    for (int row = 0; row < columns_->rowCount(); ++row) {
        const auto& column = view_.columns[static_cast<std::size_t>(row)];
        for (const auto& [cell, part] :
             {std::pair{title_column, ":title"}, std::pair{display_column, ":display"},
              std::pair{sort_column, ":sort"}, std::pair{style_column, ":style"}}) {
            const auto where = "column:" + column.id + part;
            const auto found = std::ranges::find(problems, where, &lists::ViewProblem::where);
            auto* item = columns_->item(row, cell);
            item->setToolTip(found == problems.end() ? QString{} : text(found->message));
            item->setForeground(found == problems.end() ? palette().text()
                                                        : palette().brush(QPalette::Link));
        }
    }
    filling_ = was;
    QStringList messages;
    for (const auto& problem : problems) {
        messages << text(problem.message);
    }
    messages.removeDuplicates();
    problems_->setText(messages.join(QStringLiteral(" · ")));
    problems_->setVisible(!messages.isEmpty());
    buttons_->button(QDialogButtonBox::Save)->setEnabled(problems.empty());
}

void ListViewDialog::preview() {
    preview_->clear();
    auto compiled = lists::CompiledListView::compile(view_);
    if (!compiled) {
        return;
    }
    // The shown expression columns, as the list would show them.
    QStringList headers;
    std::vector<std::size_t> shown;
    std::size_t expression = 0U;
    for (const auto& column : view_.columns) {
        if (column.builtin()) {
            continue;
        }
        if (column.visible) {
            headers << text(column.title);
            shown.push_back(expression);
        }
        ++expression;
    }
    headers.prepend(QStringLiteral("Title"));
    preview_->setColumnCount(static_cast<int>(headers.size()));
    preview_->setHeaderLabels(headers);
    std::vector<lists::ViewRow> rows;
    rows.reserve(rows_.size());
    for (const auto& row : rows_) {
        rows.push_back(row.view());
    }
    const auto evaluated = lists::evaluate_list_rows(*compiled, rows);
    if (!evaluated) {
        return;
    }
    std::vector<lists::EvaluatedGroup> groups;
    if (compiled->groups_by_expression()) {
        if (auto made = lists::evaluate_list_groups(*compiled, rows, *evaluated)) {
            groups = std::move(*made);
        }
    }
    std::map<std::size_t, QString> subgroups;
    for (const auto& made : groups) {
        for (const auto& [start, label] : made.subgroups) {
            subgroups.emplace(start, text(label));
        }
    }
    auto group = groups.begin();
    for (std::size_t at = 0U; at < rows.size(); ++at) {
        if (group != groups.end() && group->first == at) {
            auto* header = new QTreeWidgetItem(preview_);
            header->setText(0, text(group->title) + QStringLiteral("    ") + text(group->details));
            header->setFirstColumnSpanned(true);
            auto font = header->font(0);
            font.setBold(true);
            header->setFont(0, font);
            ++group;
        }
        auto* item = new QTreeWidgetItem(preview_);
        auto title = rows_[at].title.empty() ? QString::fromStdString(rows_[at].raw_path)
                                             : text(rows_[at].title);
        if (const auto found = subgroups.find(at); found != subgroups.end()) {
            title = found->second + QStringLiteral(" · ") + title;
        }
        item->setText(0, title);
        for (std::size_t column = 0U; column < shown.size(); ++column) {
            item->setText(static_cast<int>(column + 1U),
                          text((*evaluated)[at].cells[shown[column]]));
        }
    }
}

void ListViewDialog::addColumn() {
    readEdits();
    int number = 1;
    while (std::ranges::contains(view_.columns, "expression-" + std::to_string(number),
                                 &lists::ViewColumn::id)) {
        ++number;
    }
    view_.columns.push_back({.id = "expression-" + std::to_string(number),
                             .visible = true,
                             .title = "Column " + std::to_string(number),
                             .display = "%genre%",
                             .sort = {},
                             .alignment = lists::ColumnAlignment::left,
                             .expands = false,
                             .style = {}});
    fillColumns();
    columns_->setCurrentCell(columns_->rowCount() - 1, title_column);
    edited();
}

void ListViewDialog::removeColumn() {
    const auto row = columns_->currentRow();
    readEdits();
    if (row < 0 || row >= static_cast<int>(view_.columns.size()) ||
        view_.columns[static_cast<std::size_t>(row)].builtin()) {
        return;
    }
    view_.columns.erase(view_.columns.begin() + row);
    fillColumns();
    edited();
}

void ListViewDialog::moveColumn(const int by) {
    const auto row = columns_->currentRow();
    const auto to = row + by;
    readEdits();
    if (row < 0 || to < 0 || to >= static_cast<int>(view_.columns.size())) {
        return;
    }
    std::swap(view_.columns[static_cast<std::size_t>(row)],
              view_.columns[static_cast<std::size_t>(to)]);
    fillColumns();
    columns_->setCurrentCell(to, title_column);
    edited();
}

} // namespace trackknife::bench
