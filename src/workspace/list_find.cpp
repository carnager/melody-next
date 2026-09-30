// SPDX-License-Identifier: GPL-3.0-only

#include "workspace/list_find.hpp"

#include "bench/bench_main_window_helpers.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/core/unicode.hpp"

#include <QElapsedTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::bench {
namespace {

constexpr std::size_t row_text_limit = 64U * 1024U;
constexpr std::size_t batch_text_limit = 256U * 1024U;
constexpr std::size_t batch_row_limit = 128U;

struct Candidate {
    int row{};
    // Last field is the authority-owned source: only local paths are escaped.
    std::vector<std::string> fields;
    bool local_path{false};
    bool oversized{false};
};

} // namespace

ListFind::ListFind(QObject* parent) : QObject(parent) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(150);
    connect(&debounce_, &QTimer::timeout, this, [this] { start(false, true); });
    pool_.setMaxThreadCount(1);
    connect(&watcher_, &QFutureWatcherBase::finished, this, &ListFind::finish);
}

ListFind::~ListFind() {
    cancel();
    pool_.waitForDone();
}

void ListFind::setStatus(const QString& status) {
    status_ = status;
    emit changed();
}

void ListFind::cancel() {
    ++generation_;
    searching_ = false;
    cancellation_.request_cancellation();
    debounce_.stop();
}

void ListFind::setList(LocalListModel* model, std::function<int()> current_row) {
    if (model_ == model && model != nullptr) {
        return;
    }
    cancel();
    shown_ = false;
    for (const auto& connection : connections_)
        disconnect(connection);
    connections_.clear();
    model_ = model;
    current_row_ = std::move(current_row);
    status_.clear();
    emit changed();
    if (model_ == nullptr)
        return;
    const auto changed_rows = [this] { invalidate(); };
    connections_.push_back(connect(model_, &QAbstractItemModel::rowsInserted, this, changed_rows));
    connections_.push_back(connect(model_, &QAbstractItemModel::rowsRemoved, this, changed_rows));
    connections_.push_back(connect(model_, &QAbstractItemModel::rowsMoved, this, changed_rows));
    connections_.push_back(connect(model_, &QAbstractItemModel::modelReset, this, changed_rows));
    connections_.push_back(connect(model_, &QAbstractItemModel::layoutChanged, this, changed_rows));
    connections_.push_back(
        connect(model_, &QAbstractItemModel::dataChanged, this,
                [this](const QModelIndex&, const QModelIndex&, const QList<int>& roles) {
                    if (roles.empty() || roles.contains(Qt::DisplayRole))
                        invalidate();
                }));
    connections_.push_back(connect(model_, &QObject::destroyed, this, [this] {
        cancel();
        shown_ = false;
        emit changed();
    }));
}

void ListFind::selectionChanged() {
    if (searching_) {
        cancel();
        setStatus(tr("Selection changed — search again"));
    }
}

void ListFind::setQuery(const QString& query) {
    if (query == query_) {
        return;
    }
    query_ = query;
    cancel();
    status_.clear();
    if (!query_.isEmpty()) {
        status_ = tr("Searching…");
        debounce_.start();
    }
    emit changed();
}

void ListFind::invalidate() {
    cancel();
    if (shown_ && !query_.isEmpty())
        setStatus(tr("List changed — search again"));
}

void ListFind::open() {
    if (!model_)
        return;
    shown_ = true;
    emit changed();
    emit opened();
}

void ListFind::dismiss() {
    cancel();
    shown_ = false;
    emit changed();
    emit dismissed();
}

void ListFind::findNext(const bool backwards) {
    if (!model_)
        return;
    if (query_.isEmpty()) {
        open();
        return;
    }
    shown_ = true;
    emit changed();
    start(backwards, false);
}

void ListFind::start(const bool backwards, const bool include_current) {
    cancel();
    if (!model_ || !shown_ || query_.isEmpty())
        return;
    total_ = model_->rowCount();
    if (total_ == 0) {
        setStatus(tr("List is empty"));
        return;
    }
    direction_ = backwards ? -1 : 1;
    cursor_ = current_row_ ? current_row_() : -1;
    wrapped_ = false;
    if (cursor_ < 0) {
        cursor_ = backwards ? total_ - 1 : 0;
    } else if (!include_current) {
        cursor_ += direction_;
        if (cursor_ < 0 || cursor_ >= total_) {
            cursor_ = backwards ? total_ - 1 : 0;
            wrapped_ = true;
        }
    }
    visited_ = 0;
    cancellation_ = core::CancellationSource{};
    searching_ = true;
    setStatus(tr("Searching…"));
    pump();
}

void ListFind::pump() {
    if (!searching_ || worker_busy_ || !model_)
        return;
    if (visited_ == total_) {
        searching_ = false;
        setStatus(tr("No matches"));
        return;
    }
    std::vector<Candidate> candidates;
    candidates.reserve(batch_row_limit);
    std::size_t batch_bytes = 0;
    QElapsedTimer capture_time;
    capture_time.start();
    const LocalListModel* local = model_.data();
    while (visited_ < total_ && candidates.size() < batch_row_limit && capture_time.elapsed() < 4) {
        std::vector<std::string_view> fields;
        // Backs formatted texts (durations) referenced by fields for the
        // remainder of this loop iteration.
        std::vector<std::string> owned_texts;
        bool oversized = false;
        // ADR-0142: Find matches every value a row carries, not only the
        // cached display projection. A uniform per-row value cap keeps
        // pathological documents an explicit limit error.
        constexpr std::size_t row_value_limit = 1'024U;
        const auto duration_text = [&owned_texts,
                                    &fields](const std::optional<std::int64_t> milliseconds) {
            if (milliseconds) {
                owned_texts.push_back(utf8Bytes(formatTime(*milliseconds)));
                fields.push_back(owned_texts.back());
            }
        };
        if (local) {
            const auto& row = local->rows()[static_cast<std::size_t>(cursor_)];
            std::size_t value_count = 0U;
            for (const auto& field : row.metadata.fields) {
                value_count += field.values.size();
            }
            oversized = value_count > row_value_limit;
            if (!oversized) {
                fields = {row.title,        row.artist, row.album,
                          row.album_artist, row.date,   row.track_number};
                for (const auto& field : row.metadata.fields) {
                    for (const auto& value : field.values) {
                        fields.push_back(value);
                    }
                }
                // ADR-0153: retained probe technicals join the haystack, so
                // a codec or a sample rate is findable like a tag. Reserved
                // up front: fields holds views into owned_texts, so the
                // vector must never reallocate after the first push.
                owned_texts.reserve(4U);
                if (row.technicals) {
                    fields.push_back(row.technicals->codec);
                    owned_texts.push_back(std::to_string(row.technicals->sample_rate));
                    fields.push_back(owned_texts.back());
                    owned_texts.push_back(std::to_string(row.technicals->bits));
                    fields.push_back(owned_texts.back());
                    owned_texts.push_back(std::to_string(row.technicals->channels));
                    fields.push_back(owned_texts.back());
                }
                duration_text(row.duration_ms);
                fields.push_back(row.raw_path);
            }
        }
        std::size_t bytes = 0;
        for (const auto field : fields) {
            if (field.size() > row_text_limit - bytes) {
                oversized = true;
                break;
            }
            bytes += field.size();
        }
        if (!candidates.empty() && bytes > batch_text_limit - batch_bytes)
            break;
        Candidate candidate{
            .row = cursor_, .fields = {}, .local_path = local != nullptr, .oversized = oversized};
        if (!oversized) {
            candidate.fields.reserve(fields.size());
            for (const auto field : fields)
                candidate.fields.emplace_back(field);
        }
        candidates.push_back(std::move(candidate));
        batch_bytes += bytes;
        ++visited_;
        // Do not span the wrap boundary in a batch: its feedback belongs
        // only to matches actually found on the far side of that boundary.
        cursor_ += direction_;
        if (cursor_ < 0 || cursor_ >= total_)
            break;
    }
    const auto generation = generation_;
    const auto cancellation = cancellation_.token();
    auto query = query_.toUtf8().toStdString();
    worker_busy_ = true;
    watcher_.setFuture(
        QtConcurrent::run(&pool_, [candidates = std::move(candidates), query = std::move(query),
                                   generation, cancellation] {
            BatchResult result{.generation = generation, .row = -1, .error = {}};
            const auto needle = core::unicodeSimpleLower(query);
            if (!needle) {
                result.error = QStringLiteral("Invalid search text");
                return result;
            }
            for (const auto& candidate : candidates) {
                if (cancellation.is_cancellation_requested())
                    return result;
                if (candidate.oversized) {
                    result.error =
                        QStringLiteral(
                            "Search stopped: track %1 exceeds find limits (64 KiB / 1024 tags)")
                            .arg(candidate.row + 1);
                    return result;
                }
                for (std::size_t field = 0; field < candidate.fields.size(); ++field) {
                    const auto text = candidate.local_path && field + 1 == candidate.fields.size()
                                          ? core::display_raw_path(candidate.fields[field])
                                          : candidate.fields[field];
                    const auto haystack = core::unicodeSimpleLower(text);
                    if (!haystack) {
                        result.error =
                            QStringLiteral("Search stopped: invalid metadata text at track %1")
                                .arg(candidate.row + 1);
                        return result;
                    }
                    if (haystack->find(*needle) != std::string::npos) {
                        result.row = candidate.row;
                        return result;
                    }
                }
            }
            return result;
        }));
}

void ListFind::finish() {
    const auto result = watcher_.result();
    worker_busy_ = false;
    if (result.generation == generation_ && searching_ && model_) {
        if (!result.error.isEmpty()) {
            searching_ = false;
            setStatus(result.error);
        } else if (result.row >= 0) {
            searching_ = false;
            emit found(result.row);
            setStatus(wrapped_
                                 ? tr("Wrapped · Track %1 of %2").arg(result.row + 1).arg(total_)
                                 : tr("Track %1 of %2").arg(result.row + 1).arg(total_));
        } else {
            if (cursor_ < 0 || cursor_ >= total_) {
                cursor_ = direction_ < 0 ? total_ - 1 : 0;
                wrapped_ = true;
            }
            setStatus(tr("Searching… %1 of %2").arg(visited_).arg(total_));
        }
    }
    if (searching_)
        QTimer::singleShot(0, this, &ListFind::pump);
}

} // namespace trackknife::bench
