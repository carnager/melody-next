// SPDX-License-Identifier: GPL-3.0-only

#include "bench/list_view_dialog.hpp"

#include "bench/list_view_apply.hpp"
#include "uicommon/queue_table_view.hpp"
#include "workspace/workspace.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

namespace trackknife::bench {
namespace {

using titleformat::FormatContextKind;

// How many of the list's rows the preview draws.
constexpr std::size_t preview_rows = 40U;

// The built-in columns as the header names them.
[[nodiscard]] QString builtin_title(const std::string& id) {
    if (id == "artwork") {
        return QStringLiteral("Cover");
    }
    if (id == "track-number") {
        return QStringLiteral("Track number");
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

[[nodiscard]] QString column_name(const lists::ViewColumn& column) {
    return column.builtin() ? builtin_title(column.id) : text(column.title);
}

// A quiet line of help.
[[nodiscard]] QLabel* hint(const QString& words, QWidget* parent) {
    auto* label = new QLabel(words, parent);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

} // namespace

ListViewDialog::ListViewDialog(lists::ListView view, std::vector<LocalTrackRow> rows,
                               const QHash<QString, QImage>& covers, const bool removable,
                               QWidget* parent)
    : QDialog(parent), view_(std::move(view)) {
    setObjectName(QStringLiteral("bench-list-view-dialog"));
    setWindowTitle(QStringLiteral("List view"));
    resize(980, 780);

    name_ = new QLineEdit(this);
    name_->setObjectName(QStringLiteral("bench-list-view-name"));
    connect(name_, &QLineEdit::textEdited, this, &ListViewDialog::edited);
    auto* name_row = new QHBoxLayout;
    name_row->addWidget(new QLabel(QStringLiteral("Name"), this));
    name_row->addWidget(name_, 1);

    auto* pages = new QTabWidget(this);
    pages->setObjectName(QStringLiteral("bench-list-view-pages"));
    pages->addTab(columnsPage(), QStringLiteral("Columns"));
    pages->addTab(rowsPage(), QStringLiteral("Rows"));
    pages->addTab(groupingPage(), QStringLiteral("Grouping"));

    // The preview: the list's first rows, drawn as the list draws them.
    if (rows.size() > preview_rows) {
        rows.resize(preview_rows);
    }
    preview_model_ = new LocalListModel(this);
    preview_model_->replaceRows(std::move(rows));
    for (auto cover = covers.cbegin(); cover != covers.cend(); ++cover) {
        preview_model_->setArtwork(cover.key(), cover.value());
    }
    if (auto copies = preview_model_->viewRows(); !copies.empty()) {
        example_tracks_ = copies.size();
        example_ = std::move(copies.front());
    }
    preview_ = new ui::QueueTableView(this);
    preview_->setObjectName(QStringLiteral("bench-list-view-preview"));
    preview_->setModel(preview_model_);
    preview_->setAlternatingRowColors(true);
    preview_->setShowGrid(false);
    preview_->setSelectionBehavior(QAbstractItemView::SelectRows);
    preview_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    preview_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    preview_->setWordWrap(false);
    preview_->setTextElideMode(Qt::ElideRight);
    preview_->verticalHeader()->hide();
    preview_->setEmptyMessage(QStringLiteral("Nothing to preview"),
                              QStringLiteral("Open the editor on a list with tracks in it"));
    // Expression groups come once their rows are evaluated.
    connect(preview_model_, &LocalListModel::expressionGroupsChanged, this,
            [this] { preview_timer_.start(); });
    preview_timer_.setSingleShot(true);
    preview_timer_.setInterval(150);
    connect(&preview_timer_, &QTimer::timeout, this, &ListViewDialog::preview);
    auto* preview_box = new QWidget(this);
    auto* preview_layout = new QVBoxLayout(preview_box);
    preview_layout->setContentsMargins(0, 0, 0, 0);
    preview_layout->addWidget(
        hint(QStringLiteral("Preview: this list's first tracks, as the view draws them"), this));
    preview_layout->addWidget(preview_, 1);

    auto* splitter = new QSplitter(Qt::Vertical, this);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(pages);
    splitter->addWidget(preview_box);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    problems_ = new QLabel(this);
    problems_->setObjectName(QStringLiteral("bench-list-view-problems"));
    problems_->setWordWrap(true);
    problems_->setTextFormat(Qt::PlainText);

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

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(name_row);
    layout->addWidget(splitter, 1);
    layout->addWidget(problems_);
    layout->addWidget(buttons_);

    fill();
    check();
    preview();
}

QWidget* ListViewDialog::expressionField(Field& field, const char* name, const QString& placeholder,
                                         const FormatContextKind context) {
    auto* box = new QWidget(this);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    field.edit = new QLineEdit(box);
    field.edit->setObjectName(QString::fromLatin1(name));
    field.edit->setPlaceholderText(placeholder);
    field.note = new QLabel(box);
    field.note->setObjectName(QString::fromLatin1(name) + QStringLiteral("-note"));
    field.note->setTextFormat(Qt::PlainText);
    field.note->setWordWrap(true);
    field.context = context;
    layout->addWidget(field.edit);
    layout->addWidget(field.note);
    connect(field.edit, &QLineEdit::textEdited, this, &ListViewDialog::edited);
    return box;
}

QWidget* ListViewDialog::columnsPage() {
    auto* page = new QWidget(this);
    // The columns in their order, ticked where shown.
    columns_ = new QListWidget(page);
    columns_->setObjectName(QStringLiteral("bench-list-view-columns"));
    connect(columns_, &QListWidget::currentRowChanged, this, &ListViewDialog::showColumn);
    connect(columns_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
        if (filling_) {
            return;
        }
        view_.columns[static_cast<std::size_t>(columns_->row(item))].visible =
            item->checkState() == Qt::Checked;
        edited();
    });
    auto* add = new QPushButton(QStringLiteral("Add column"), page);
    add->setObjectName(QStringLiteral("bench-list-view-add-column"));
    connect(add, &QPushButton::clicked, this, &ListViewDialog::addColumn);
    remove_column_ = new QPushButton(QStringLiteral("Remove"), page);
    remove_column_->setObjectName(QStringLiteral("bench-list-view-remove-column"));
    connect(remove_column_, &QPushButton::clicked, this, &ListViewDialog::removeColumn);
    auto* up = new QPushButton(QStringLiteral("Up"), page);
    up->setObjectName(QStringLiteral("bench-list-view-column-up"));
    connect(up, &QPushButton::clicked, this, [this] { moveColumn(-1); });
    auto* down = new QPushButton(QStringLiteral("Down"), page);
    down->setObjectName(QStringLiteral("bench-list-view-column-down"));
    connect(down, &QPushButton::clicked, this, [this] { moveColumn(1); });
    auto* list_buttons = new QHBoxLayout;
    list_buttons->addWidget(add);
    list_buttons->addWidget(remove_column_);
    list_buttons->addStretch();
    list_buttons->addWidget(up);
    list_buttons->addWidget(down);
    auto* list_box = new QVBoxLayout;
    list_box->addWidget(hint(QStringLiteral("Ticked columns are shown, left to right in this "
                                            "order. Choose one to edit it."),
                             page));
    list_box->addWidget(columns_, 1);
    list_box->addLayout(list_buttons);

    // A built-in column: Trackknife draws and sorts it; what it says beneath
    // is the view's.
    auto* builtin = new QWidget(page);
    auto* builtin_form = new QFormLayout(builtin);
    builtin_note_ = hint({}, builtin);
    builtin_form->addRow(builtin_note_);
    builtin_form->addRow(QStringLiteral("Second line"),
                         expressionField(builtin_second_, "bench-list-view-builtin-second",
                                         QStringLiteral("e.g. %artist% · %album%"),
                                         FormatContextKind::list_item));
    builtin_form->addRow(hint(QStringLiteral("Shown smaller beneath it, when rows have more "
                                             "than one line (Rows page)."),
                              builtin));

    // A column of the view's own.
    auto* own = new QWidget(page);
    auto* own_form = new QFormLayout(own);
    column_title_ = new QLineEdit(own);
    column_title_->setObjectName(QStringLiteral("bench-list-view-column-title"));
    connect(column_title_, &QLineEdit::textEdited, this, &ListViewDialog::edited);
    own_form->addRow(QStringLiteral("Header"), column_title_);
    own_form->addRow(QStringLiteral("Shows"),
                     expressionField(column_display_, "bench-list-view-column-display",
                                     QStringLiteral("e.g. %genre%"), FormatContextKind::list_item));
    own_form->addRow(QStringLiteral("Second line"),
                     expressionField(column_second_, "bench-list-view-column-second",
                                     QStringLiteral("beneath it, when rows have more lines"),
                                     FormatContextKind::list_item));
    own_form->addRow(QStringLiteral("Sorts by"),
                     expressionField(column_sort_, "bench-list-view-column-sort",
                                     QStringLiteral("empty: by what it shows"),
                                     FormatContextKind::sort));
    own_form->addRow(QStringLiteral("Style"),
                     expressionField(column_style_, "bench-list-view-column-style",
                                     QStringLiteral("empty: as its row"),
                                     FormatContextKind::list_item));
    column_align_ = new QComboBox(own);
    column_align_->setObjectName(QStringLiteral("bench-list-view-column-align"));
    column_align_->addItems(
        {QStringLiteral("Left"), QStringLiteral("Right"), QStringLiteral("Centre")});
    connect(column_align_, &QComboBox::currentIndexChanged, this, &ListViewDialog::edited);
    own_form->addRow(QStringLiteral("Aligned"), column_align_);
    column_expands_ = new QCheckBox(QStringLiteral("Takes up spare room"), own);
    column_expands_->setObjectName(QStringLiteral("bench-list-view-column-expands"));
    connect(column_expands_, &QCheckBox::toggled, this, &ListViewDialog::edited);
    own_form->addRow(column_expands_);
    own_form->addRow(hint(QStringLiteral("Expressions are tkfmt-1: %tag%, $if(…), "
                                         "$info(codec). Beneath each, what it gives for this "
                                         "list's first track."),
                          own));

    column_page_ = new QStackedWidget(page);
    column_page_->addWidget(builtin);
    column_page_->addWidget(own);

    auto* layout = new QHBoxLayout(page);
    layout->addLayout(list_box, 2);
    layout->addWidget(column_page_, 3);
    return page;
}

QWidget* ListViewDialog::rowsPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    row_lines_ = new QComboBox(page);
    row_lines_->setObjectName(QStringLiteral("bench-list-view-row-lines"));
    row_lines_->addItems(
        {QStringLiteral("One line"), QStringLiteral("Two lines"), QStringLiteral("Three lines")});
    connect(row_lines_, &QComboBox::currentIndexChanged, this, &ListViewDialog::edited);
    form->addRow(QStringLiteral("Each row has"), row_lines_);
    form->addRow(hint(QStringLiteral("With more than one line, each column shows its Second "
                                     "line beneath (Columns page)."),
                      page));
    artwork_ = new QComboBox(page);
    artwork_->setObjectName(QStringLiteral("bench-list-view-artwork"));
    artwork_->addItems({QStringLiteral("Beside the rows"), QStringLiteral("In the group header"),
                        QStringLiteral("None")});
    connect(artwork_, &QComboBox::currentIndexChanged, this, &ListViewDialog::edited);
    form->addRow(QStringLiteral("Covers"), artwork_);
    form->addRow(hint(QStringLiteral("Grouped, a group has one cover; not grouped, every row "
                                     "shows its own."),
                      page));
    form->addRow(QStringLiteral("Row style"),
                 expressionField(row_style_, "bench-list-view-row-style",
                                 QStringLiteral("e.g. $if(%playcount%,,quiet)"),
                                 FormatContextKind::list_item));
    row_style_.where = "view:row_style";
    form->addRow(hint(QStringLiteral("Gives words a row is drawn by: quiet, accent, warning, "
                                     "bold, italic. A column's own Style goes over it."),
                      page));
    return page;
}

QWidget* ListViewDialog::groupingPage() {
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    grouping_ = new QComboBox(page);
    grouping_->setObjectName(QStringLiteral("bench-list-view-grouping"));
    grouping_->addItems({QStringLiteral("Not grouped"), QStringLiteral("By album, discs within"),
                         QStringLiteral("By an expression")});
    connect(grouping_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        // Grouping by an expression starts from one that works.
        if (!filling_ && index == 2 && group_key_.edit->text().isEmpty() &&
            group_title_.edit->text().isEmpty()) {
            group_key_.edit->setText(QStringLiteral("%genre%"));
            group_title_.edit->setText(QStringLiteral("$if2(%genre%,No genre)"));
            group_details_.edit->setText(QStringLiteral("%group_tracks% tracks · %group_length%"));
        }
        edited();
    });
    form->addRow(QStringLiteral("Rows are"), grouping_);
    expression_grouping_ = new QWidget(page);
    auto* grouped = new QFormLayout(expression_grouping_);
    grouped->setContentsMargins(0, 0, 0, 0);
    grouped->addRow(hint(QStringLiteral("Neighbouring rows with the same value make a group, "
                                        "in the list's order: sort the list by it first."),
                         expression_grouping_));
    grouped->addRow(QStringLiteral("Group by"),
                    expressionField(group_key_, "bench-list-view-group-key",
                                    QStringLiteral("e.g. %label%"), FormatContextKind::grouping));
    grouped->addRow(QStringLiteral("Header"),
                    expressionField(group_title_, "bench-list-view-group-title",
                                    QStringLiteral("e.g. $if2(%label%,No label)"),
                                    FormatContextKind::list_item));
    grouped->addRow(QStringLiteral("Beside it"),
                    expressionField(group_details_, "bench-list-view-group-details",
                                    QStringLiteral("e.g. %group_tracks% tracks · %group_length%"),
                                    FormatContextKind::list_item));
    group_key_.where = "group:key";
    group_title_.where = "group:title";
    group_details_.where = "group:details";
    group_title_.header = true;
    group_details_.header = true;
    subgroup_ = new QCheckBox(QStringLiteral("Subgroups within a group, where it has several"),
                              expression_grouping_);
    subgroup_->setObjectName(QStringLiteral("bench-list-view-subgroup"));
    connect(subgroup_, &QCheckBox::toggled, this, &ListViewDialog::edited);
    grouped->addRow(subgroup_);
    grouped->addRow(QStringLiteral("Subgroup by"),
                    expressionField(subgroup_key_, "bench-list-view-subgroup-key",
                                    QStringLiteral("e.g. %discnumber%"),
                                    FormatContextKind::grouping));
    grouped->addRow(QStringLiteral("Its label"),
                    expressionField(subgroup_label_, "bench-list-view-subgroup-label",
                                    QStringLiteral("e.g. Disc %discnumber%"),
                                    FormatContextKind::list_item));
    subgroup_key_.where = "subgroup:key";
    subgroup_label_.where = "subgroup:label";
    form->addRow(expression_grouping_);
    return page;
}

void ListViewDialog::fill() {
    filling_ = true;
    name_->setText(text(view_.name));
    row_lines_->setCurrentIndex(std::clamp(view_.row_lines, 1, 3) - 1);
    artwork_->setCurrentIndex(view_.artwork == lists::ViewArtwork::beside   ? 0
                              : view_.artwork == lists::ViewArtwork::header ? 1
                                                                            : 2);
    row_style_.edit->setText(text(view_.row_style));
    grouping_->setCurrentIndex(view_.grouping == lists::ViewGrouping::none    ? 0
                               : view_.grouping == lists::ViewGrouping::album ? 1
                                                                              : 2);
    group_key_.edit->setText(text(view_.group_key));
    group_title_.edit->setText(text(view_.group_title));
    group_details_.edit->setText(text(view_.group_details));
    subgroup_->setChecked(view_.subgroup.has_value());
    subgroup_key_.edit->setText(view_.subgroup ? text(view_.subgroup->key) : QString{});
    subgroup_label_.edit->setText(view_.subgroup ? text(view_.subgroup->label) : QString{});
    fillColumnList();
    filling_ = false;
    // The title first: the column saying most.
    const auto title =
        std::ranges::find(view_.columns, std::string{"title"}, &lists::ViewColumn::id);
    columns_->setCurrentRow(
        title == view_.columns.end() ? 0 : static_cast<int>(title - view_.columns.begin()));
}

void ListViewDialog::fillColumnList() {
    const bool was = filling_;
    filling_ = true;
    columns_->clear();
    for (const auto& column : view_.columns) {
        auto* item = new QListWidgetItem(column_name(column), columns_);
        if (!column.builtin()) {
            item->setToolTip(text(column.display));
            auto font = item->font();
            font.setItalic(true);
            item->setFont(font);
        }
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setCheckState(column.visible ? Qt::Checked : Qt::Unchecked);
        // The cover is the view's Covers (Rows page), not a column to tick.
        item->setHidden(column.id == "artwork");
    }
    filling_ = was;
}

lists::ViewColumn* ListViewDialog::currentColumn() {
    return shown_column_ >= 0 && shown_column_ < static_cast<int>(view_.columns.size())
               ? &view_.columns[static_cast<std::size_t>(shown_column_)]
               : nullptr;
}

void ListViewDialog::showColumn(const int row) {
    if (!filling_) {
        readColumn();
    }
    shown_column_ = row;
    const auto* column = currentColumn();
    remove_column_->setEnabled(column != nullptr && !column->builtin());
    if (column == nullptr) {
        return;
    }
    const bool was = filling_;
    filling_ = true;
    const auto where = "column:" + column->id + ":";
    if (column->builtin()) {
        column_page_->setCurrentIndex(0);
        builtin_note_->setText(QStringLiteral("%1 is a built-in column: Trackknife draws it and "
                                              "sorts by it. What it shows beneath is the view's.")
                                   .arg(builtin_title(column->id)));
        builtin_second_.edit->setText(text(column->second_line));
        builtin_second_.where = where + "second_line";
    } else {
        column_page_->setCurrentIndex(1);
        column_title_->setText(text(column->title));
        column_display_.edit->setText(text(column->display));
        column_second_.edit->setText(text(column->second_line));
        column_sort_.edit->setText(text(column->sort));
        column_style_.edit->setText(text(column->style));
        column_align_->setCurrentIndex(column->alignment == lists::ColumnAlignment::right    ? 1
                                       : column->alignment == lists::ColumnAlignment::center ? 2
                                                                                             : 0);
        column_expands_->setChecked(column->expands);
        column_display_.where = where + "display";
        column_second_.where = where + "second_line";
        column_sort_.where = where + "sort";
        column_style_.where = where + "style";
    }
    filling_ = was;
    check();
}

void ListViewDialog::readColumn() {
    auto* column = currentColumn();
    if (column == nullptr) {
        return;
    }
    if (column->builtin()) {
        column->second_line = builtin_second_.edit->text().toStdString();
        return;
    }
    column->title = column_title_->text().trimmed().toStdString();
    column->display = column_display_.edit->text().toStdString();
    column->second_line = column_second_.edit->text().toStdString();
    column->sort = column_sort_.edit->text().toStdString();
    column->style = column_style_.edit->text().toStdString();
    column->alignment = column_align_->currentIndex() == 1   ? lists::ColumnAlignment::right
                        : column_align_->currentIndex() == 2 ? lists::ColumnAlignment::center
                                                             : lists::ColumnAlignment::left;
    column->expands = column_expands_->isChecked();
}

void ListViewDialog::readEdits() {
    view_.name = name_->text().trimmed().toStdString();
    view_.row_lines = row_lines_->currentIndex() + 1;
    view_.artwork = artwork_->currentIndex() == 0   ? lists::ViewArtwork::beside
                    : artwork_->currentIndex() == 1 ? lists::ViewArtwork::header
                                                    : lists::ViewArtwork::none;
    view_.row_style = row_style_.edit->text().toStdString();
    view_.grouping = grouping_->currentIndex() == 0   ? lists::ViewGrouping::none
                     : grouping_->currentIndex() == 1 ? lists::ViewGrouping::album
                                                      : lists::ViewGrouping::expression;
    view_.group_key = group_key_.edit->text().toStdString();
    view_.group_title = group_title_.edit->text().toStdString();
    view_.group_details = group_details_.edit->text().toStdString();
    view_.subgroup = subgroup_->isChecked()
                         ? std::optional{lists::ViewSubgroup{
                               .key = subgroup_key_.edit->text().toStdString(),
                               .label = subgroup_label_.edit->text().toStdString()}}
                         : std::nullopt;
    readColumn();
}

void ListViewDialog::edited() {
    if (filling_) {
        return;
    }
    readEdits();
    // The list names a column as its header does.
    if (const auto* column = currentColumn(); column != nullptr && !column->builtin()) {
        if (auto* item = columns_->item(shown_column_)) {
            const bool was = filling_;
            filling_ = true;
            item->setText(column_name(*column));
            item->setToolTip(text(column->display));
            filling_ = was;
        }
    }
    check();
    preview_timer_.start();
}

void ListViewDialog::note(Field& field, const std::vector<lists::ViewProblem>& problems) const {
    const auto problem = std::ranges::find(problems, field.where, &lists::ViewProblem::where);
    const auto source = field.edit->text().toStdString();
    auto palette = field.note->palette();
    if (problem != problems.end()) {
        field.note->setVisible(true);
        palette.setColor(QPalette::WindowText, QColor{0xd0, 0x6a, 0x2a});
        field.note->setPalette(palette);
        field.note->setText(text(problem->message));
        return;
    }
    palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
    field.note->setPalette(palette);
    field.note->setVisible(!source.empty());
    if (source.empty()) {
        field.note->clear();
        return;
    }
    if (const auto error = lists::list_view_expression_error(source, field.context);
        !error.empty()) {
        // A field the view does not use just now, its error all the same.
        field.note->setText(text(error));
        return;
    }
    if (!example_) {
        field.note->setText(QStringLiteral("→ (no track in this list to try it on)"));
        return;
    }
    const auto tracks = field.header ? example_tracks_ : 1U;
    const auto length = field.header ? example_->duration_ms.value_or(0) *
                                           static_cast<std::int64_t>(example_tracks_)
                                     : 0;
    const auto value =
        lists::evaluate_view_expression(source, field.context, example_->view(), tracks, length);
    if (!value) {
        field.note->setText(text(value.error().message));
        return;
    }
    field.note->setText(value->empty() ? QStringLiteral("→ nothing, for the first track")
                                       : QStringLiteral("→ ") + text(*value));
}

void ListViewDialog::check() {
    expression_grouping_->setVisible(grouping_->currentIndex() == 2);
    subgroup_key_.edit->setEnabled(subgroup_->isChecked());
    subgroup_label_.edit->setEnabled(subgroup_->isChecked());
    const auto problems = lists::validate_list_view(view_);
    for (auto* field : {&builtin_second_, &column_display_, &column_second_, &column_sort_,
                        &column_style_, &row_style_, &group_key_, &group_title_, &group_details_,
                        &subgroup_key_, &subgroup_label_}) {
        note(*field, problems);
    }
    // Every problem, said where it is: the Save button waits on them.
    const auto place = [this](const std::string& where) -> QString {
        static const std::map<std::string, QString> places{
            {"view:row_style", QStringLiteral("Rows, row style")},
            {"group:key", QStringLiteral("Grouping, group by")},
            {"group:title", QStringLiteral("Grouping, header")},
            {"group:details", QStringLiteral("Grouping, beside it")},
            {"subgroup:key", QStringLiteral("Grouping, subgroup by")},
            {"subgroup:label", QStringLiteral("Grouping, its label")}};
        if (const auto found = places.find(where); found != places.end()) {
            return found->second;
        }
        if (!where.starts_with("column:")) {
            return {};
        }
        const auto part_at = where.rfind(':');
        const auto id = where.substr(7U, part_at - 7U);
        const auto part = where.substr(part_at + 1U);
        const auto column = std::ranges::find(view_.columns, id, &lists::ViewColumn::id);
        const auto name = column == view_.columns.end() ? text(id) : column_name(*column);
        static const std::map<std::string, QString> parts{
            {"title", QStringLiteral("header")},
            {"display", QStringLiteral("shows")},
            {"sort", QStringLiteral("sorts by")},
            {"style", QStringLiteral("style")},
            {"second_line", QStringLiteral("second line")}};
        const auto named = parts.find(part);
        return QStringLiteral("Column “%1”, %2")
            .arg(name, named == parts.end() ? text(part) : named->second);
    };
    QStringList messages;
    for (const auto& problem : problems) {
        const auto where = place(problem.where);
        messages << (where.isEmpty() ? text(problem.message)
                                     : QStringLiteral("%1: %2").arg(where, text(problem.message)));
    }
    messages.removeDuplicates();
    problems_->setText(messages.join(QLatin1Char('\n')));
    problems_->setVisible(!messages.isEmpty());
    buttons_->button(QDialogButtonBox::Save)->setEnabled(problems.empty());
}

void ListViewDialog::preview() {
    // A view that does not compile keeps the last one drawn.
    auto compiled = lists::CompiledListView::compile(view_);
    if (!compiled) {
        return;
    }
    static_cast<void>(drawListView(
        preview_, view_, std::make_shared<const lists::CompiledListView>(std::move(*compiled)),
        Workspace::trackViewLayoutFor(view_)));
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
                             .title = "Genre",
                             .display = "%genre%",
                             .sort = {},
                             .alignment = lists::ColumnAlignment::left,
                             .expands = false,
                             .style = {},
                             .second_line = {},
                             .width = 0});
    shown_column_ = -1;
    fillColumnList();
    columns_->setCurrentRow(columns_->count() - 1);
    column_title_->setFocus();
    column_title_->selectAll();
    edited();
}

void ListViewDialog::removeColumn() {
    const auto row = shown_column_;
    if (const auto* column = currentColumn(); column == nullptr || column->builtin()) {
        return;
    }
    view_.columns.erase(view_.columns.begin() + row);
    shown_column_ = -1;
    fillColumnList();
    auto next = std::min(row, columns_->count() - 1);
    if (next >= 0 && columns_->item(next)->isHidden()) {
        next = next > 0 ? next - 1 : next + 1;
    }
    columns_->setCurrentRow(next);
    edited();
}

void ListViewDialog::moveColumn(const int by) {
    readColumn();
    const auto row = shown_column_;
    auto to = row + by;
    // Past the cover, which the list does not show.
    while (to >= 0 && to < static_cast<int>(view_.columns.size()) &&
           view_.columns[static_cast<std::size_t>(to)].id == "artwork") {
        to += by;
    }
    if (row < 0 || to < 0 || to >= static_cast<int>(view_.columns.size())) {
        return;
    }
    std::swap(view_.columns[static_cast<std::size_t>(row)],
              view_.columns[static_cast<std::size_t>(to)]);
    shown_column_ = -1;
    fillColumnList();
    columns_->setCurrentRow(to);
    edited();
}

} // namespace trackknife::bench
