// SPDX-License-Identifier: GPL-3.0-only

#include "bench/bench_main_window.hpp"
#include "bench/local_list_model.hpp"
#include "uicommon/queue_item_delegate.hpp"
#include "uicommon/queue_table_view.hpp"
#include "uicommon/track_row_roles.hpp"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHeaderView>
#include <QImage>
#include <QItemSelectionModel>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTest>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using trackknife::bench::LocalListModel;
using trackknife::bench::LocalTrackRow;
using trackknife::ui::QueueTableView;

struct Summary {
    double p50_ms{0.0};
    double p95_ms{0.0};
    double worst_ms{0.0};
};

struct Measurement {
    std::string_view name;
    double budget_ms{0.0};
    Summary summary;
};

[[nodiscard]] Summary summarize(std::vector<double> samples) {
    std::ranges::sort(samples);
    const auto percentile = [&samples](const double fraction) {
        const auto position =
            static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1U));
        return samples.at(position);
    };
    return {.p50_ms = percentile(0.50), .p95_ms = percentile(0.95), .worst_ms = samples.back()};
}

template <typename Operation> [[nodiscard]] double measure(Operation&& operation) {
    QElapsedTimer timer;
    timer.start();
    operation();
    QApplication::sendPostedEvents();
    QApplication::processEvents(QEventLoop::AllEvents);
    return static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
}

[[nodiscard]] std::vector<LocalTrackRow> syntheticRows(const int row_count) {
    constexpr int tracks_per_album = 10;
    std::vector<LocalTrackRow> rows;
    rows.reserve(static_cast<std::size_t>(row_count));
    for (int index = 0; index < row_count; ++index) {
        const auto album = index / tracks_per_album;
        const auto track = index % tracks_per_album;
        const auto shared_source = album % 5 == 0;
        const auto path = shared_source ? "/benchmark/cue/album-" + std::to_string(album) + ".flac"
                                        : "/benchmark/files/album-" + std::to_string(album) +
                                              "/track-" + std::to_string(track) + ".flac";
        std::optional<trackknife::formats::SampleRange> segment;
        if (shared_source) {
            segment = trackknife::formats::SampleRange{
                .start_sample = static_cast<std::int64_t>(track) * 10'584'000,
                .end_sample = static_cast<std::int64_t>(track + 1) * 10'584'000};
        }
        rows.push_back(LocalTrackRow{
            .raw_path = path,
            .logical_reference =
                shared_source
                    ? std::optional<std::string>{path + ".cue#" + std::to_string(track + 1)}
                    : std::nullopt,
            .selection = {},
            .segment = segment,
            .title = "Long deterministic title " + std::to_string(index) +
                     " — single-line metadata benchmark",
            .artist = "Track artist " + std::to_string(album % 317),
            .album = "Album " + std::to_string(album) + " with a deliberately long title",
            .album_artist = "Album artist " + std::to_string(album % 211),
            .date = std::to_string(1970 + album % 57),
            .track_number = std::to_string(track + 1),
            .duration_ms = 180'000 + (index % 120) * 1'000,
            .metadata = {},
            .source_revision = std::nullopt,
            .probed = true,
        });
    }
    return rows;
}

[[nodiscard]] int requestedRowCount(const QStringList& arguments, const bool quick) {
    auto result = quick ? 10'000 : 100'000;
    for (const auto& argument : arguments) {
        constexpr std::string_view prefix{"--rows="};
        const auto encoded = argument.toLatin1();
        const std::string_view value{encoded.constData(), static_cast<std::size_t>(encoded.size())};
        if (!value.starts_with(prefix)) {
            continue;
        }
        int parsed = 0;
        const auto number = value.substr(prefix.size());
        const auto conversion =
            std::from_chars(number.data(), number.data() + number.size(), parsed);
        if (conversion.ec == std::errc{} && conversion.ptr == number.data() + number.size() &&
            parsed >= 10'000 && parsed <= 1'000'000) {
            result = parsed;
        }
    }
    return result;
}

void configureCachedTab(QueueTableView& target, LocalListModel& model,
                        const QueueTableView& source) {
    target.setModel(&model);
    for (const auto property : std::array{trackknife::ui::track_artwork_column_property,
                                          trackknife::ui::track_artist_column_property,
                                          trackknife::ui::track_number_column_property,
                                          trackknife::ui::track_title_column_property,
                                          trackknife::ui::track_album_column_property,
                                          trackknife::ui::track_date_column_property,
                                          trackknife::ui::track_length_column_property,
                                          trackknife::ui::track_separate_number_property,
                                          trackknife::ui::track_side_artwork_property}) {
        target.setProperty(property, source.property(property));
    }
    target.setItemDelegate(new trackknife::ui::QueueItemDelegate(&target));
    target.setAlternatingRowColors(true);
    target.setShowGrid(false);
    target.setSelectionBehavior(QAbstractItemView::SelectRows);
    target.setSelectionMode(QAbstractItemView::ExtendedSelection);
    target.setEditTriggers(QAbstractItemView::NoEditTriggers);
    target.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    target.setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    target.setWordWrap(false);
    target.setTextElideMode(Qt::ElideRight);
    target.verticalHeader()->setDefaultSectionSize(22);
    target.verticalHeader()->setMinimumSectionSize(18);
    target.verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    target.verticalHeader()->hide();
    target.horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    for (int column = 0; column < model.columnCount(); ++column) {
        target.setColumnHidden(column, source.isColumnHidden(column));
        target.setColumnWidth(column, source.columnWidth(column));
    }
    target.setAlbumArtworkColumn(source.albumArtworkColumn());
    target.setAlbumGroupingEnabled(true);
}

void report(const Measurement& measurement) {
    std::cout << measurement.name << ',' << measurement.budget_ms << ','
              << measurement.summary.p50_ms << ',' << measurement.summary.p95_ms << ','
              << measurement.summary.worst_ms << ','
              << (measurement.summary.p95_ms <= measurement.budget_ms ? "within" : "over") << '\n';
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("TrackknifeBenchmarks"));
    QApplication::setApplicationName(QStringLiteral("TrackknifeUiBenchmark"));
    QStandardPaths::setTestModeEnabled(true);
    QSettings{}.clear();
    const auto application_data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    for (const auto& name : {QStringLiteral("lists.sqlite"), QStringLiteral("lists.sqlite-wal"),
                             QStringLiteral("lists.sqlite-shm")}) {
        static_cast<void>(QFile::remove(QDir{application_data}.filePath(name)));
    }

    const auto quick = application.arguments().contains(QStringLiteral("--quick"));
    // Measured and reported, not judged: a sanitizer build is several times
    // slower, and its timings say nothing about the budgets.
    const auto report_only = application.arguments().contains(QStringLiteral("--report-only"));
    const auto row_count = requestedRowCount(application.arguments(), quick);
    const auto sample_count = quick ? 12 : 120;

    trackknife::bench::BenchMainWindow window;
    window.resize(1'280, 760);
    window.show();
    static_cast<void>(QTest::qWaitForWindowExposed(&window, 2'000));
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("bench-tabs"));
    QueueTableView* view = nullptr;
    LocalListModel* model = nullptr;
    for (int attempt = 0; tabs != nullptr && model == nullptr && attempt < 200; ++attempt) {
        for (int index = 0; index < tabs->count(); ++index) {
            auto* candidate = dynamic_cast<QueueTableView*>(tabs->widget(index));
            auto* candidate_model =
                candidate == nullptr ? nullptr : qobject_cast<LocalListModel*>(candidate->model());
            if (candidate_model != nullptr) {
                view = candidate;
                model = candidate_model;
                break;
            }
        }
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        if (model == nullptr) {
            QTest::qWait(5);
        }
    }
    auto* plain = window.findChild<QAction*>(QStringLiteral("action-track-layout-plain"));
    auto* grouped = window.findChild<QAction*>(QStringLiteral("action-track-layout-albums-side"));
    if (tabs == nullptr || view == nullptr || model == nullptr || plain == nullptr ||
        grouped == nullptr) {
        std::cerr << "benchmark could not locate the Trackknife list workspace\n";
        return EXIT_FAILURE;
    }

    auto rows = syntheticRows(row_count);
    const auto initial_grouping_ms = measure([&model, &rows, view] {
        model->replaceRows(std::move(rows));
        view->viewport()->repaint();
    });

    auto* cached_view = new QueueTableView(tabs);
    configureCachedTab(*cached_view, *model, *view);
    const auto local_tab_index = tabs->indexOf(view);
    const auto cached_tab_index = tabs->addTab(cached_view, QStringLiteral("Cached list"));
    QApplication::processEvents(QEventLoop::AllEvents);

    std::vector<double> scroll_samples;
    std::vector<double> selection_samples;
    std::vector<double> tab_samples;
    std::vector<double> grouping_samples;
    std::vector<double> artwork_samples;
    scroll_samples.reserve(static_cast<std::size_t>(sample_count));
    selection_samples.reserve(static_cast<std::size_t>(sample_count));
    tab_samples.reserve(static_cast<std::size_t>(sample_count));
    grouping_samples.reserve(static_cast<std::size_t>(sample_count));
    artwork_samples.reserve(static_cast<std::size_t>(sample_count));

    tabs->setCurrentIndex(local_tab_index);
    for (int sample = 0; sample < sample_count; ++sample) {
        const auto row = static_cast<int>((static_cast<qint64>(sample) * 104'729) % row_count);
        const auto target = model->index(row, trackknife::bench::local_title_column);
        scroll_samples.push_back(measure([view, &target] {
            view->scrollTo(target, QAbstractItemView::PositionAtCenter);
            view->viewport()->repaint();
        }));
        selection_samples.push_back(measure([view, &target] {
            view->selectionModel()->setCurrentIndex(target, QItemSelectionModel::ClearAndSelect |
                                                                QItemSelectionModel::Rows);
            view->viewport()->repaint();
        }));
        tab_samples.push_back(measure([tabs, local_tab_index, cached_tab_index] {
            tabs->setCurrentIndex(tabs->currentIndex() == local_tab_index ? cached_tab_index
                                                                          : local_tab_index);
            tabs->currentWidget()->repaint();
        }));
        tabs->setCurrentIndex(local_tab_index);
        plain->trigger();
        QApplication::processEvents(QEventLoop::AllEvents);
        grouping_samples.push_back(measure([grouped, view] {
            grouped->trigger();
            view->viewport()->repaint();
        }));
        QImage cover{24, 24, QImage::Format_RGB32};
        cover.fill(QColor::fromHsv(sample % 360, 180, 210));
        const auto artwork_key = model->groupKey(row - row % 10);
        artwork_samples.push_back(measure([model, &artwork_key, cover = std::move(cover), view]() {
            model->setArtwork(artwork_key, cover);
            view->viewport()->repaint();
        }));
    }

    const std::array measurements{
        Measurement{"initial_grouping_and_first_paint", 75.0,
                    Summary{.p50_ms = initial_grouping_ms,
                            .p95_ms = initial_grouping_ms,
                            .worst_ms = initial_grouping_ms}},
        Measurement{"grouped_scroll_and_paint", 16.7, summarize(std::move(scroll_samples))},
        Measurement{"selection_status_and_paint", 50.0, summarize(std::move(selection_samples))},
        Measurement{"cached_tab_switch_and_paint", 50.0, summarize(std::move(tab_samples))},
        Measurement{"cached_grouping_and_paint", 50.0, summarize(std::move(grouping_samples))},
        Measurement{"artwork_update_and_paint", 50.0, summarize(std::move(artwork_samples))},
    };

    std::cout << "platform," << QApplication::platformName().toStdString() << '\n';
    std::cout << "logical_rows," << row_count << '\n';
    std::cout << "albums," << (row_count + 9) / 10 << '\n';
    std::cout << "samples," << sample_count << '\n';
    std::cout << "metric,budget_ms,p50_ms,p95_ms,worst_ms,result\n";
    auto within_budgets = true;
    for (const auto& measurement : measurements) {
        report(measurement);
        within_budgets = within_budgets && measurement.summary.p95_ms <= measurement.budget_ms;
    }

    QSettings{}.clear();
    return within_budgets || report_only ? EXIT_SUCCESS : EXIT_FAILURE;
}
