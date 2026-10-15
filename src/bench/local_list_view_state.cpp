// SPDX-License-Identifier: GPL-3.0-only

// ADR-0282: a list drawn by a view -- its expression columns and groups,
// evaluated off the window's thread and kept with the rows.

#include "bench/local_list_model.hpp"

#include "uicommon/track_row_roles.hpp"

#include <QBrush>
#include <QFont>
#include <QFuture>
#include <QGuiApplication>
#include <QPalette>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>

namespace trackknife::bench {
namespace {

// Every view any list is given has a number of its own, so a row copied from
// another list does not bring that list's cells with it.
std::atomic<std::uint64_t> view_generations{0U};

// At most this many rows are copied for one evaluation: the first of a long
// list show soon, and the window's thread never copies the whole list at once.
constexpr std::size_t evaluation_batch = 2'000U;

[[nodiscard]] lists::OwnedViewRow owned(const LocalTrackRow& row) {
    lists::OwnedViewRow copy;
    copy.raw_path = row.raw_path;
    copy.metadata = row.metadata;
    copy.title = row.title;
    copy.artist = row.artist;
    copy.album = row.album;
    copy.album_artist = row.album_artist;
    copy.date = row.date;
    copy.track_number = row.track_number;
    copy.duration_ms = row.duration_ms;
    if (row.technicals) {
        copy.codec = row.technicals->codec;
        copy.sample_rate = row.technicals->sample_rate;
        copy.bits = row.technicals->bits;
        copy.channels = row.technicals->channels;
        copy.bit_rate = row.technicals->bit_rate;
    }
    copy.rating = row.rating;
    copy.history = row.history;
    copy.added = row.added;
    copy.album_added = row.album_added;
    return copy;
}

[[nodiscard]] Qt::Alignment alignment_of(const lists::ColumnAlignment alignment) {
    switch (alignment) {
    case lists::ColumnAlignment::right:
        return Qt::AlignRight | Qt::AlignVCenter;
    case lists::ColumnAlignment::center:
        return Qt::AlignHCenter | Qt::AlignVCenter;
    case lists::ColumnAlignment::left:
        break;
    }
    return Qt::AlignLeft | Qt::AlignVCenter;
}

} // namespace

void LocalListModel::setListView(std::shared_ptr<const lists::CompiledListView> view) {
    const auto columns = view ? static_cast<int>(view->expression_columns().size()) : 0;
    if (expression_columns_ > 0) {
        beginRemoveColumns({}, local_column_count, local_column_count + expression_columns_ - 1);
        expression_columns_ = 0;
        endRemoveColumns();
    }
    list_view_ = std::move(view);
    planColumns();
    view_generation_ = ++view_generations;
    group_headers_.clear();
    group_starts_.clear();
    group_header_keys_.clear();
    subgroup_labels_.clear();
    const bool was_ready = groups_ready_;
    groups_ready_ = false;
    if (columns > 0) {
        beginInsertColumns({}, local_column_count, local_column_count + columns - 1);
        expression_columns_ = columns;
        endInsertColumns();
    }
    if (was_ready) {
        emit expressionGroupsChanged();
    }
    scheduleEvaluation();
}

void LocalListModel::planColumns() {
    column_plans_.clear();
    if (!list_view_) {
        return;
    }
    const auto columns =
        local_column_count + static_cast<int>(list_view_->expression_columns().size());
    const auto position = [](const std::vector<std::string>& ids, const std::string& id) {
        const auto found = std::ranges::find(ids, id);
        return found == ids.end() ? -1 : static_cast<int>(found - ids.begin());
    };
    const auto& view_columns = list_view_->view().columns;
    column_plans_.reserve(static_cast<std::size_t>(columns));
    for (int column = 0; column < columns; ++column) {
        const auto id = columnId(column).toStdString();
        ColumnPlan plan;
        if (column < local_column_count) {
            plan.builtin = position(list_view_->builtin_expression_columns(), id);
            plan.shows_expression = list_view_->builtin_shows_expression(id);
        }
        plan.second_line = position(list_view_->second_line_columns(), id);
        if (const auto found = std::ranges::find(view_columns, id, &lists::ViewColumn::id);
            found != view_columns.end()) {
            plan.alignment = QVariant::fromValue(alignment_of(found->alignment));
        }
        column_plans_.push_back(std::move(plan));
    }
}

bool LocalListModel::expressionGrouping() const noexcept {
    return list_view_ != nullptr && list_view_->groups_by_expression();
}

QString LocalListModel::columnId(const int column) const {
    if (column >= 0 && column < local_column_count) {
        return QString::fromLatin1(lists::builtin_column_ids.at(static_cast<std::size_t>(column)));
    }
    const auto expression = static_cast<std::size_t>(column - local_column_count);
    if (list_view_ && column >= local_column_count &&
        expression < list_view_->expression_columns().size()) {
        return QString::fromStdString(list_view_->expression_columns()[expression]);
    }
    return {};
}

int LocalListModel::columnOf(const QString& id) const {
    const auto text = id.toStdString();
    if (const auto found = std::ranges::find(lists::builtin_column_ids, text);
        found != lists::builtin_column_ids.end()) {
        return static_cast<int>(found - lists::builtin_column_ids.begin());
    }
    if (list_view_) {
        const auto& ids = list_view_->expression_columns();
        if (const auto found = std::ranges::find(ids, text); found != ids.end()) {
            return local_column_count + static_cast<int>(found - ids.begin());
        }
    }
    return -1;
}

std::vector<lists::OwnedViewRow> LocalListModel::viewRows() const {
    std::vector<lists::OwnedViewRow> copies;
    copies.reserve(rows_.size());
    for (const auto& row : rows_) {
        copies.push_back(owned(row));
    }
    return copies;
}

std::vector<LocalListModel::HistoryRequest> LocalListModel::takeHistoryRequests() {
    std::vector<HistoryRequest> requests;
    if (!list_view_ || !list_view_->reads_history()) {
        return requests;
    }
    for (auto& row : rows_) {
        if (row.history_loaded || row.history_requested) {
            continue;
        }
        row.history_requested = true;
        persistence::ListItem source;
        source.source = persistence::ListSource::local;
        source.source_reference = row.raw_path;
        source.source_revision = row.source_revision;
        source.source_selection = persistence::ListItemSourceSelection{row.selection.stream_index,
                                                                       row.selection.subsong_index};
        if (row.segment) {
            source.segment =
                persistence::ListItemSegment{row.segment->start_sample, row.segment->end_sample};
        }
        // As the tab search asks: an unnamed album is no album.
        requests.push_back(
            {.entry = row.entry_id,
             .source = {.source = std::move(source),
                        .album_hash = row.album.empty() ? std::string{} : row.album_rating_hash}});
    }
    return requests;
}

void LocalListModel::applyHistory(const std::vector<core::StableId>& entries,
                                  const std::vector<std::array<std::int64_t, 6>>& history) {
    if (entries.size() != history.size()) {
        historyUnavailable(entries);
        return;
    }
    int first = -1;
    int last = -1;
    int hint = 0;
    for (std::size_t at = 0U; at < entries.size(); ++at) {
        const auto row = rowOfEntry(entries[at], hint);
        if (row < 0) {
            continue;
        }
        hint = row + 1;
        auto& target = rows_[static_cast<std::size_t>(row)];
        // Asked again since -- a listen recorded meanwhile: that answer is
        // the one to take.
        if (!target.history_requested) {
            continue;
        }
        target.history = history[at];
        target.history_requested = false;
        target.history_loaded = true;
        forgetEvaluation(target);
        first = first < 0 ? row : std::min(first, row);
        last = std::max(last, row);
    }
    historyShown(first, last);
    scheduleEvaluation();
}

void LocalListModel::historyUnavailable(const std::vector<core::StableId>& entries) {
    int first = -1;
    int last = -1;
    int hint = 0;
    for (const auto& entry : entries) {
        const auto row = rowOfEntry(entry, hint);
        if (row < 0) {
            continue;
        }
        hint = row + 1;
        // Shown without: an engine that cannot say does not hold the list.
        auto& target = rows_[static_cast<std::size_t>(row)];
        target.history.reset();
        target.history_requested = false;
        target.history_loaded = true;
        forgetEvaluation(target);
        first = first < 0 ? row : std::min(first, row);
        last = std::max(last, row);
    }
    historyShown(first, last);
    scheduleEvaluation();
}

void LocalListModel::forgetHistory(const std::vector<std::string>& raw_paths) {
    bool forgot = false;
    for (auto& row : rows_) {
        if (std::ranges::contains(raw_paths, row.raw_path)) {
            row.history_loaded = false;
            row.history_requested = false;
            forgot = true;
        }
    }
    if (forgot) {
        scheduleEvaluation();
    }
}

void LocalListModel::forgetEvaluation(LocalTrackRow& row) {
    row.evaluated.reset();
    ++row.evaluation_ticket;
}

bool LocalListModel::showsOwnExpression(const int column) const {
    const auto* plan = columnPlan(column);
    return list_view_ != nullptr && plan != nullptr && plan->shows_expression;
}

void LocalListModel::scheduleEvaluation() {
    if (!list_view_) {
        return;
    }
    if (evaluation_timer_ == nullptr) {
        evaluation_timer_ = new QTimer(this);
        evaluation_timer_->setSingleShot(true);
        connect(evaluation_timer_, &QTimer::timeout, this, &LocalListModel::evaluatePending);
    }
    evaluation_timer_->start(0);
}

void LocalListModel::evaluatePending() {
    if (!list_view_ || evaluating_) {
        return;
    }
    std::vector<PendingEvaluation> pending;
    std::vector<lists::OwnedViewRow> copies;
    // A view that reads listening history evaluates a row once its history
    // is here; the rows without are asked for.
    const auto needs_history = list_view_->reads_history();
    bool wanted = false;
    for (std::size_t at = 0U; at < rows_.size() && pending.size() < evaluation_batch; ++at) {
        const auto& row = rows_[at];
        if (needs_history && !row.history_loaded) {
            wanted = wanted || !row.history_requested;
            continue;
        }
        if (!evaluatedNow(row)) {
            pending.push_back({.entry = row.entry_id,
                               .ticket = row.evaluation_ticket,
                               .hint = static_cast<int>(at)});
            copies.push_back(owned(row));
        }
    }
    if (wanted) {
        emit historyWanted();
    }
    if (pending.empty()) {
        regroup();
        return;
    }
    evaluating_ = true;
    QtConcurrent::run([view = list_view_, copies = std::move(copies)] {
        std::vector<lists::ViewRow> rows;
        rows.reserve(copies.size());
        for (const auto& copy : copies) {
            rows.push_back(copy.view());
        }
        auto evaluated = lists::evaluate_list_rows(*view, rows);
        return evaluated ? std::move(*evaluated) : std::vector<lists::EvaluatedRow>{};
    })
        .then(this, [this, view = view_generation_,
                     pending = std::move(pending)](std::vector<lists::EvaluatedRow> evaluated) {
            evaluating_ = false;
            mergeEvaluation(view, pending, std::move(evaluated));
            // The rest, and whatever changed meanwhile.
            scheduleEvaluation();
        });
}

void LocalListModel::mergeEvaluation(const std::uint64_t view,
                                     const std::vector<PendingEvaluation>& pending,
                                     std::vector<lists::EvaluatedRow> evaluated) {
    if (view != view_generation_ || evaluated.size() != pending.size()) {
        return;
    }
    int first = -1;
    int last = -1;
    for (std::size_t at = 0U; at < pending.size(); ++at) {
        const auto row = rowOfEntry(pending[at].entry, pending[at].hint);
        // A row that changed while it was evaluated is evaluated again.
        if (row < 0 ||
            rows_[static_cast<std::size_t>(row)].evaluation_ticket != pending[at].ticket) {
            continue;
        }
        auto& target = rows_[static_cast<std::size_t>(row)];
        target.evaluated = std::make_shared<const lists::EvaluatedRow>(std::move(evaluated[at]));
        target.evaluated_view = view;
        first = first < 0 ? row : std::min(first, row);
        last = std::max(last, row);
    }
    if (first < 0) {
        return;
    }
    // Built-in columns take their second lines and styles from the
    // evaluation too -- not their text, which the list's jobs read as an edit.
    // Their text too where they show an expression of the view's (ADR-0285).
    QList<int> builtin_roles{ui::track_second_line_role, Qt::ForegroundRole, Qt::FontRole};
    if (std::ranges::any_of(list_view_->builtin_expression_columns(), [this](const auto& id) {
            return list_view_->builtin_shows_expression(id);
        })) {
        builtin_roles.append(Qt::DisplayRole);
    }
    emit dataChanged(index(first, 0), index(last, local_column_count - 1), builtin_roles);
    if (expression_columns_ > 0) {
        emit dataChanged(
            index(first, local_column_count), index(last, columnCount() - 1),
            {Qt::DisplayRole, ui::track_second_line_role, Qt::ForegroundRole, Qt::FontRole});
    }
}

void LocalListModel::regroup() {
    if (!expressionGrouping()) {
        return;
    }
    const bool was_ready = groups_ready_;
    std::vector<const lists::EvaluatedRow*> evaluated;
    std::vector<std::int64_t> durations;
    evaluated.reserve(rows_.size());
    durations.reserve(rows_.size());
    for (const auto& row : rows_) {
        if (!evaluatedNow(row)) {
            // Not all known yet: ungrouped until they are.
            groups_ready_ = false;
            if (was_ready) {
                emit expressionGroupsChanged();
            }
            return;
        }
        evaluated.push_back(row.evaluated.get());
        durations.push_back(row.duration_ms.value_or(0));
    }
    std::vector<std::uint8_t> starts(rows_.size(), 0U);
    std::vector<QString> header_keys(rows_.size());
    std::vector<QString> labels(rows_.size());
    std::vector<QString> missing_keys;
    QSet<QString> missing;
    std::vector<lists::OwnedViewRow> missing_rows;
    std::vector<std::pair<std::size_t, std::int64_t>> missing_sizes;
    for (const auto& run : lists::list_group_runs(evaluated, durations)) {
        starts[run.first] = 1U;
        // A header is evaluated again only when what it can say changed: its
        // group's key, first row, size or length.
        const auto key = QString::fromStdString(evaluated[run.first]->group_key) + QChar::Null +
                         QString::fromStdString(rows_[run.first].entry_id.to_string()) +
                         QChar::Null + QString::number(run.count) + QChar::Null +
                         QString::number(run.length_ms) + QChar::Null +
                         QString::number(rows_[run.first].evaluation_ticket);
        header_keys[run.first] = key;
        if (!group_headers_.contains(key) && !missing.contains(key)) {
            missing.insert(key);
            missing_keys.push_back(key);
            missing_rows.push_back(owned(rows_[run.first]));
            missing_sizes.emplace_back(run.count, run.length_ms);
        }
        for (const auto start : run.subgroup_starts) {
            labels[start] = QString::fromStdString(evaluated[start]->subgroup_label);
        }
    }
    // The rows whose group roles say something else now: where a group or
    // subgroup starts, and which group -- its header's key -- each row is in.
    // Rows inserted or removed since, or groups not shown until now: every
    // row.
    std::vector<int> changed_rows;
    const bool aligned = group_starts_.size() == rows_.size() &&
                         group_header_keys_.size() == rows_.size() &&
                         subgroup_labels_.size() == rows_.size();
    if (aligned) {
        const QString* was_in = nullptr;
        const QString* is_in = nullptr;
        for (std::size_t at = 0U; at < rows_.size(); ++at) {
            if (group_starts_[at] != 0U) {
                was_in = &group_header_keys_[at];
            }
            if (starts[at] != 0U) {
                is_in = &header_keys[at];
            }
            if (starts[at] != group_starts_[at] || labels[at] != subgroup_labels_[at] ||
                header_keys[at] != group_header_keys_[at] ||
                (was_in == nullptr) != (is_in == nullptr) ||
                (was_in != nullptr && *was_in != *is_in)) {
                changed_rows.push_back(static_cast<int>(at));
            }
        }
    }
    const bool changed = !aligned || !changed_rows.empty();
    group_starts_ = std::move(starts);
    group_header_keys_ = std::move(header_keys);
    subgroup_labels_ = std::move(labels);
    groups_ready_ = true;
    if (!missing_keys.empty() && !headers_evaluating_) {
        headers_evaluating_ = true;
        QtConcurrent::run([view = list_view_, rows = std::move(missing_rows),
                           sizes = std::move(missing_sizes)] {
            std::vector<lists::GroupHeader> headers;
            headers.reserve(rows.size());
            for (std::size_t at = 0U; at < rows.size(); ++at) {
                headers.push_back(lists::evaluate_group_header(*view, rows[at].view(),
                                                               sizes[at].first, sizes[at].second));
            }
            return headers;
        })
            .then(this, [this, view = view_generation_,
                         keys = std::move(missing_keys)](std::vector<lists::GroupHeader> headers) {
                headers_evaluating_ = false;
                mergeHeaders(view, keys, std::move(headers));
            });
    }
    const QList<int> group_roles{ui::track_album_group_start_role, ui::track_disc_start_role,
                                 ui::track_group_key_role, ui::track_group_title_role};
    if (changed && !rows_.empty()) {
        if (aligned && was_ready) {
            emitRowRuns(changed_rows, 0, 0, group_roles);
        } else {
            emit dataChanged(index(0, 0), index(static_cast<int>(rows_.size()) - 1, 0),
                             group_roles);
        }
    }
    if (!was_ready) {
        emit expressionGroupsChanged();
    }
}

void LocalListModel::mergeHeaders(const std::uint64_t view, const std::vector<QString>& keys,
                                  std::vector<lists::GroupHeader> headers) {
    if (view != view_generation_ || keys.size() != headers.size()) {
        return;
    }
    // Headers of groups that are gone go too -- not those still shown, or
    // each new header would have every other one evaluated again.
    if (group_headers_.size() > 4 * std::max<qsizetype>(64, static_cast<qsizetype>(keys.size()))) {
        QSet<QString> live;
        for (const auto& key : group_header_keys_) {
            if (!key.isEmpty()) {
                live.insert(key);
            }
        }
        if (group_headers_.size() > 2 * std::max<qsizetype>(64, live.size())) {
            group_headers_.removeIf(
                [&live](const QHash<QString, GroupHeaderCache>::iterator& header) {
                    return !live.contains(header.key());
                });
        }
    }
    QSet<QString> arrived;
    for (std::size_t at = 0U; at < keys.size(); ++at) {
        group_headers_.insert(keys[at], {.title = QString::fromStdString(headers[at].title),
                                         .details = QString::fromStdString(headers[at].details)});
        arrived.insert(keys[at]);
    }
    // The group starts these headers are for; other rows have no header.
    std::vector<int> starts;
    if (group_header_keys_.size() == rows_.size() && group_starts_.size() == rows_.size()) {
        for (std::size_t at = 0U; at < rows_.size(); ++at) {
            if (group_starts_[at] != 0U && arrived.contains(group_header_keys_[at])) {
                starts.push_back(static_cast<int>(at));
            }
        }
        emitRowRuns(starts, 0, 0, {ui::track_group_title_role, ui::track_group_details_role});
    } else if (!rows_.empty()) {
        emit dataChanged(index(0, 0), index(static_cast<int>(rows_.size()) - 1, 0),
                         {ui::track_group_title_role, ui::track_group_details_role});
    }
    // Groups that changed while these were evaluated.
    regroup();
}

QVariant LocalListModel::viewStyle(const QModelIndex& index, const int role) const {
    const auto& row = rows_[static_cast<std::size_t>(index.row())];
    if (!evaluatedNow(row)) {
        return {};
    }
    auto style = lists::parse_cell_style(row.evaluated->row_style);
    if (index.column() >= local_column_count) {
        const auto cell = static_cast<std::size_t>(index.column() - local_column_count);
        if (cell < row.evaluated->cell_styles.size()) {
            style = lists::combined_style(
                style, lists::parse_cell_style(row.evaluated->cell_styles[cell]));
        }
    } else if (const auto* plan = columnPlan(index.column());
               plan != nullptr && plan->builtin >= 0 &&
               static_cast<std::size_t>(plan->builtin) < row.evaluated->builtin_styles.size()) {
        // ADR-0285: a built-in column's own style.
        style = lists::combined_style(
            style, lists::parse_cell_style(
                       row.evaluated->builtin_styles[static_cast<std::size_t>(plan->builtin)]));
    }
    if (!style.any()) {
        return {};
    }
    if (role == Qt::FontRole) {
        if (!style.bold && !style.italic) {
            return {};
        }
        QFont font;
        font.setWeight(style.bold ? QFont::DemiBold : QFont::Normal);
        font.setItalic(style.italic);
        return font;
    }
    // The playing row keeps its own colour, and the stars theirs.
    if (index.row() == current_row_ || index.column() == local_rating_column) {
        return {};
    }
    const auto palette = QGuiApplication::palette();
    if (style.warning) {
        return QBrush{QColor{0xd0, 0x85, 0x2a}};
    }
    if (style.accent) {
        return QBrush{palette.color(QPalette::Highlight).lighter(115)};
    }
    if (style.quiet) {
        return QBrush{palette.color(QPalette::PlaceholderText)};
    }
    return {};
}

QVariant LocalListModel::viewData(const QModelIndex& index, const int role) const {
    const auto at = static_cast<std::size_t>(index.row());
    const auto& row = rows_[at];
    // ADR-0284: the cell's second line, where its column has one.
    const auto* plan = columnPlan(index.column());
    if (role == ui::track_second_line_role) {
        if (!evaluatedNow(row) || plan == nullptr || plan->second_line < 0) {
            return QString{};
        }
        const auto line = static_cast<std::size_t>(plan->second_line);
        return line >= row.evaluated->second_lines.size()
                   ? QString{}
                   : QString::fromStdString(row.evaluated->second_lines[line]);
    }
    // ADR-0285: a built-in column's own expression.
    if (index.column() < local_column_count &&
        (role == Qt::DisplayRole || role == Qt::TextAlignmentRole)) {
        if (role == Qt::TextAlignmentRole) {
            return plan != nullptr ? plan->alignment : QVariant{};
        }
        const auto cell = plan != nullptr ? plan->builtin : -1;
        return evaluatedNow(row) && cell >= 0 &&
                       static_cast<std::size_t>(cell) < row.evaluated->builtin_cells.size()
                   ? QString::fromStdString(
                         row.evaluated->builtin_cells[static_cast<std::size_t>(cell)])
                   : QString{};
    }
    if (index.column() >= local_column_count) {
        const auto expression = static_cast<std::size_t>(index.column() - local_column_count);
        if (role == Qt::DisplayRole) {
            return evaluatedNow(row) && expression < row.evaluated->cells.size()
                       ? QString::fromStdString(row.evaluated->cells[expression])
                       : QString{};
        }
        if (role == Qt::TextAlignmentRole) {
            return plan != nullptr ? plan->alignment : QVariant{};
        }
        return {};
    }
    // The groups' roles: answered only by a list grouped by an expression,
    // once its groups are known; otherwise the album's own.
    const bool grouped = groups_ready_ && group_starts_.size() == rows_.size();
    switch (role) {
    case ui::track_group_key_role:
        return grouped && evaluatedNow(row)
                   ? QVariant{QString::fromStdString(row.evaluated->group_key)}
                   : QVariant{};
    case ui::track_album_group_start_role:
        return grouped ? QVariant{group_starts_[at] != 0U} : QVariant{};
    case ui::track_disc_start_role:
        return grouped ? QVariant{subgroup_labels_[at]} : QVariant{QString{}};
    case ui::track_group_title_role:
    case ui::track_group_details_role: {
        if (!grouped || group_starts_[at] == 0U) {
            return {};
        }
        // Not evaluated yet: an empty header, not the album's.
        const auto found = group_headers_.constFind(group_header_keys_[at]);
        if (found == group_headers_.constEnd()) {
            return QString{};
        }
        return role == ui::track_group_title_role ? found->title : found->details;
    }
    default:
        return {};
    }
}

} // namespace trackknife::bench
