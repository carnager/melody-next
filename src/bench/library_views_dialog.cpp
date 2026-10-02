// SPDX-License-Identifier: GPL-3.0-only

#include "bench/library_views_dialog.hpp"

#include "bench/themed_icon.hpp"
#include "workspace/library_browser.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTableWidget>
#include <QToolButton>
#include <QTreeView>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace trackknife::bench {
namespace {

enum Column { format_column, sort_column, descending_column };

[[nodiscard]] QString text(const std::string& value) { return QString::fromStdString(value); }

} // namespace

LibraryViewsDialog::LibraryViewsDialog(const CatalogueSource& catalogues, EngineKey engine,
                                       QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("bench-library-views"));
    setWindowTitle(tr("Library views"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(980, 640);
    auto* layout = new QVBoxLayout(this);
    auto* explanation = new QLabel(
        tr("A view groups the library by levels, each a tkfmt-1 expression evaluated for every "
           "track. $each(genre) lists a track under each of its genres. Tracks always come "
           "last. The shipped views can be copied, not changed: copy Artist › Album and make "
           "its album level $if(%date%,$left(%date%,4) – ,)%album% to see each album's year."),
        this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    auto* splitter = new QSplitter(this);
    layout->addWidget(splitter, 1);

    auto* views_side = new QWidget(splitter);
    auto* views_layout = new QVBoxLayout(views_side);
    views_layout->setContentsMargins(0, 0, 0, 0);
    list_ = new QListWidget(views_side);
    list_->setObjectName(QStringLiteral("library-views-list"));
    list_->setAccessibleName(tr("Library views"));
    views_layout->addWidget(list_, 1);
    auto* view_buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("New"), views_side);
    add->setObjectName(QStringLiteral("library-views-new"));
    copy_ = new QPushButton(tr("Copy"), views_side);
    copy_->setObjectName(QStringLiteral("library-views-copy"));
    remove_ = new QPushButton(tr("Remove"), views_side);
    remove_->setObjectName(QStringLiteral("library-views-remove"));
    view_buttons->addWidget(add);
    view_buttons->addWidget(copy_);
    view_buttons->addWidget(remove_);
    views_layout->addLayout(view_buttons);

    auto* editor = new QWidget(splitter);
    auto* editor_layout = new QVBoxLayout(editor);
    editor_layout->setContentsMargins(0, 0, 0, 0);
    auto* name_row = new QHBoxLayout;
    name_row->addWidget(new QLabel(tr("Name"), editor));
    name_ = new QLineEdit(editor);
    name_->setObjectName(QStringLiteral("library-view-name"));
    name_->setAccessibleName(tr("View name"));
    name_row->addWidget(name_, 1);
    editor_layout->addLayout(name_row);

    levels_ = new QTableWidget(0, 3, editor);
    levels_->setObjectName(QStringLiteral("library-view-levels"));
    levels_->setAccessibleName(tr("Levels"));
    levels_->setHorizontalHeaderLabels({tr("Group by"), tr("Sort by (optional)"), tr("Reverse")});
    levels_->horizontalHeader()->setSectionResizeMode(format_column, QHeaderView::Stretch);
    levels_->horizontalHeader()->setSectionResizeMode(sort_column, QHeaderView::Stretch);
    levels_->horizontalHeader()->setSectionResizeMode(descending_column,
                                                      QHeaderView::ResizeToContents);
    levels_->verticalHeader()->setVisible(true);
    levels_->setSelectionBehavior(QAbstractItemView::SelectRows);
    levels_->setSelectionMode(QAbstractItemView::SingleSelection);
    levels_->setMaximumHeight(170);
    editor_layout->addWidget(levels_);

    const auto tool = [editor](const QString& name, const QString& icon, const QString& label) {
        auto* button = new QToolButton(editor);
        button->setObjectName(name);
        button->setText(label);
        button->setToolTip(label);
        button->setAccessibleName(label);
        button->setIcon(themedIcon(icon));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button->setAutoRaise(true);
        return button;
    };
    auto* level_buttons = new QHBoxLayout;
    add_level_ = tool(QStringLiteral("library-view-add-level"),
                      QStringLiteral("list-add|sp:SP_FileDialogNewFolder"), tr("Add level"));
    remove_level_ = tool(QStringLiteral("library-view-remove-level"),
                         QStringLiteral("list-remove|sp:SP_TrashIcon"), tr("Remove level"));
    level_up_ = tool(QStringLiteral("library-view-level-up"),
                     QStringLiteral("go-up|sp:SP_ArrowUp"), tr("Up"));
    level_down_ = tool(QStringLiteral("library-view-level-down"),
                       QStringLiteral("go-down|sp:SP_ArrowDown"), tr("Down"));
    for (auto* button : {add_level_, remove_level_, level_up_, level_down_}) {
        level_buttons->addWidget(button);
    }
    level_buttons->addStretch(1);
    editor_layout->addLayout(level_buttons);

    note_ = new QLabel(editor);
    note_->setObjectName(QStringLiteral("library-view-note"));
    note_->setWordWrap(true);
    note_->setForegroundRole(QPalette::PlaceholderText);
    note_->hide();
    editor_layout->addWidget(note_);
    error_ = new QLabel(editor);
    error_->setObjectName(QStringLiteral("library-view-error"));
    error_->setWordWrap(true);
    error_->setForegroundRole(QPalette::BrightText);
    error_->hide();
    editor_layout->addWidget(error_);

    auto* preview_label = new QLabel(tr("Preview"), editor);
    auto heading = preview_label->font();
    heading.setBold(true);
    preview_label->setFont(heading);
    editor_layout->addWidget(preview_label);
    preview_ = new LibraryBrowser(catalogues, std::move(engine), this);
    preview_view_ = new QTreeView(editor);
    preview_view_->setObjectName(QStringLiteral("library-view-preview"));
    preview_view_->setAccessibleName(tr("Preview of the view"));
    preview_view_->setHeaderHidden(true);
    preview_view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    preview_view_->setModel(preview_->model());
    editor_layout->addWidget(preview_view_, 1);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);

    auto* buttons = new QDialogButtonBox(this);
    save_ = buttons->addButton(tr("Save views"), QDialogButtonBox::AcceptRole);
    save_->setObjectName(QStringLiteral("library-views-save"));
    save_->setEnabled(false);
    buttons->addButton(QDialogButtonBox::Close);
    layout->addWidget(buttons);

    preview_timer_.setSingleShot(true);
    preview_timer_.setInterval(400);
    connect(&preview_timer_, &QTimer::timeout, this, &LibraryViewsDialog::preview);
    connect(list_, &QListWidget::currentRowChanged, this, &LibraryViewsDialog::showView);
    connect(add, &QPushButton::clicked, this, [this] { addView(false); });
    connect(copy_, &QPushButton::clicked, this, [this] { addView(true); });
    connect(remove_, &QPushButton::clicked, this, &LibraryViewsDialog::removeView);
    connect(name_, &QLineEdit::textEdited, this, &LibraryViewsDialog::edited);
    connect(levels_, &QTableWidget::itemChanged, this, &LibraryViewsDialog::edited);
    connect(levels_, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto row = levels_->currentRow();
        const auto* view = current();
        const bool editable = view != nullptr && !view->builtin;
        remove_level_->setEnabled(editable && row >= 0 && levels_->rowCount() > 1);
        level_up_->setEnabled(editable && row > 0);
        level_down_->setEnabled(editable && row >= 0 && row + 1 < levels_->rowCount());
    });
    connect(add_level_, &QToolButton::clicked, this, &LibraryViewsDialog::addLevel);
    connect(remove_level_, &QToolButton::clicked, this, &LibraryViewsDialog::removeLevel);
    connect(level_up_, &QToolButton::clicked, this, [this] { moveLevel(-1); });
    connect(level_down_, &QToolButton::clicked, this, [this] { moveLevel(1); });
    connect(save_, &QPushButton::clicked, this, &LibraryViewsDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    loadViews();
}

void LibraryViewsDialog::loadViews() {
    views_.clear();
    views_ = libraryViews();
    const QSignalBlocker blocker{list_};
    list_->clear();
    for (const auto& view : views_) {
        auto* item = new QListWidgetItem(view.name, list_);
        if (view.builtin) {
            item->setToolTip(tr("Shipped with Trackknife: copy it to change it"));
            item->setForeground(palette().color(QPalette::PlaceholderText));
        }
    }
    shown_ = -1;
}

void LibraryViewsDialog::selectView(const QString& id) {
    const auto found = std::ranges::find(views_, id, &LibraryViewDefinition::id);
    list_->setCurrentRow(found == views_.end() ? 0 : static_cast<int>(found - views_.begin()));
    if (list_->currentRow() == shown_ && shown_ >= 0) {
        return;
    }
    showView(list_->currentRow());
}

LibraryViewDefinition* LibraryViewsDialog::current() {
    if (shown_ < 0 || static_cast<std::size_t>(shown_) >= views_.size()) {
        return nullptr;
    }
    return &views_[static_cast<std::size_t>(shown_)];
}

void LibraryViewsDialog::showView(const int row) {
    shown_ = row;
    const auto* view = current();
    const bool editable = view != nullptr && !view->builtin;
    filling_ = true;
    name_->setText(view != nullptr ? view->name : QString{});
    name_->setReadOnly(!editable);
    showLevels();
    filling_ = false;
    // Folders has no levels to start a copy from.
    copy_->setEnabled(view != nullptr && !view->levels.empty());
    note_->setText(view == nullptr || !view->own_tree ? QString{}
                   : view->levels.empty()
                       ? tr("Folders browses the engine's folders, as indexed, from the "
                            "library's roots down. No levels describe it, so it cannot be copied.")
                       : tr("Shown with the library's own tree. These levels group the same way: "
                            "copy the view to change them."));
    note_->setVisible(!note_->text().isEmpty());
    remove_->setEnabled(editable);
    add_level_->setEnabled(editable);
    emit levels_->itemSelectionChanged();
    edited();
}

void LibraryViewsDialog::showLevels() {
    const auto* view = current();
    const bool editable = view != nullptr && !view->builtin;
    const QSignalBlocker blocker{levels_};
    levels_->setRowCount(0);
    if (view == nullptr) {
        return;
    }
    for (const auto& level : view->levels) {
        const auto row = levels_->rowCount();
        levels_->insertRow(row);
        auto* format = new QTableWidgetItem(text(level.format));
        auto* sort = new QTableWidgetItem(text(level.sort));
        auto* descending = new QTableWidgetItem;
        descending->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsSelectable |
                             (editable ? Qt::ItemIsEnabled : Qt::NoItemFlags));
        descending->setCheckState(level.descending ? Qt::Checked : Qt::Unchecked);
        for (auto* item : {format, sort}) {
            item->setFlags(editable ? item->flags() | Qt::ItemIsEditable
                                    : item->flags() & ~Qt::ItemIsEditable);
        }
        levels_->setItem(row, format_column, format);
        levels_->setItem(row, sort_column, sort);
        levels_->setItem(row, descending_column, descending);
    }
}

// The table and name, read back into the view being edited.
void LibraryViewsDialog::readEdits() {
    auto* view = current();
    if (view == nullptr || view->builtin) {
        return;
    }
    view->name = name_->text();
    view->levels.clear();
    for (int row = 0; row < levels_->rowCount(); ++row) {
        const auto* format = levels_->item(row, format_column);
        const auto* sort = levels_->item(row, sort_column);
        const auto* descending = levels_->item(row, descending_column);
        view->levels.push_back(
            {.format = format != nullptr ? format->text().toStdString() : std::string{},
             .sort = sort != nullptr ? sort->text().trimmed().toStdString() : std::string{},
             .descending = descending != nullptr && descending->checkState() == Qt::Checked});
    }
    if (auto* item = list_->item(shown_); item != nullptr) {
        item->setText(view->name);
    }
}

void LibraryViewsDialog::edited() {
    if (filling_) {
        return;
    }
    const auto* view = current();
    if (view != nullptr && !view->builtin && (sender() == name_ || sender() == levels_)) {
        readEdits();
        dirty_ = true;
    }
    QString problem;
    if (view != nullptr) {
        if (view->name.trimmed().isEmpty()) {
            problem = tr("The view needs a name.");
        }
        for (std::size_t index = 0; problem.isEmpty() && index < view->levels.size(); ++index) {
            const auto& level = view->levels[index];
            auto error = libraryViewLevelError(text(level.format));
            if (error.isEmpty() && !level.sort.empty()) {
                error = libraryViewLevelError(text(level.sort));
            }
            if (!error.isEmpty()) {
                problem = tr("Level %1: %2").arg(index + 1U).arg(error);
            }
        }
    }
    error_->setText(problem);
    error_->setVisible(!problem.isEmpty());
    save_->setEnabled(dirty_ && problem.isEmpty());
    if (problem.isEmpty()) {
        preview_timer_.start();
    }
}

void LibraryViewsDialog::preview() {
    const auto* view = current();
    if (view == nullptr) {
        return;
    }
    // A shipped tree is previewed as it is shown, not as its levels.
    if (view->own_tree) {
        preview_->previewView(view->id);
    } else if (!view->levels.empty()) {
        preview_->previewLevels(view->levels);
    }
}

void LibraryViewsDialog::addView(const bool copy) {
    LibraryViewDefinition view;
    if (copy && current() != nullptr) {
        view = *current();
        view.name = tr("%1 (copy)").arg(view.name);
        view.own_tree = false;
    } else {
        view.name = tr("New view");
        view.levels = {{.format = "%albumartist%", .sort = {}, .descending = false},
                       {.format = "%album%", .sort = "%date%", .descending = false}};
    }
    view.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    view.builtin = false;
    views_.push_back(std::move(view));
    list_->addItem(views_.back().name);
    dirty_ = true;
    list_->setCurrentRow(list_->count() - 1);
    name_->setFocus();
    name_->selectAll();
}

void LibraryViewsDialog::removeView() {
    const auto* view = current();
    if (view == nullptr || view->builtin) {
        return;
    }
    const auto row = shown_;
    views_.erase(views_.begin() + row);
    shown_ = -1;
    delete list_->takeItem(row);
    dirty_ = true;
    list_->setCurrentRow(std::min(row, list_->count() - 1));
    save_->setEnabled(true);
}

void LibraryViewsDialog::addLevel() {
    auto* view = current();
    if (view == nullptr || view->builtin) {
        return;
    }
    view->levels.push_back({.format = "%album%", .sort = {}, .descending = false});
    dirty_ = true;
    showLevels();
    levels_->setCurrentCell(levels_->rowCount() - 1, format_column);
    levels_->editItem(levels_->item(levels_->rowCount() - 1, format_column));
    edited();
}

void LibraryViewsDialog::removeLevel() {
    auto* view = current();
    const auto row = levels_->currentRow();
    if (view == nullptr || view->builtin || row < 0 || view->levels.size() < 2U) {
        return;
    }
    view->levels.erase(view->levels.begin() + row);
    dirty_ = true;
    showLevels();
    levels_->setCurrentCell(std::min(row, levels_->rowCount() - 1), format_column);
    edited();
}

void LibraryViewsDialog::moveLevel(const int by) {
    auto* view = current();
    const auto row = levels_->currentRow();
    const auto target = row + by;
    if (view == nullptr || view->builtin || row < 0 || target < 0 ||
        target >= static_cast<int>(view->levels.size())) {
        return;
    }
    std::swap(view->levels[static_cast<std::size_t>(row)],
              view->levels[static_cast<std::size_t>(target)]);
    dirty_ = true;
    showLevels();
    levels_->setCurrentCell(target, format_column);
    edited();
}

void LibraryViewsDialog::save() {
    std::vector<LibraryViewDefinition> custom;
    for (const auto& view : views_) {
        if (!view.builtin) {
            custom.push_back(view);
        }
    }
    if (auto saved = saveCustomLibraryViews(custom); !saved) {
        error_->setText(QString::fromStdString(saved.error().message));
        error_->show();
        return;
    }
    dirty_ = false;
    save_->setEnabled(false);
}

} // namespace trackknife::bench
