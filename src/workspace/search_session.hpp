// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/catalogue_source.hpp"
#include "bench/engine_key.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/query/search_presets.hpp"
#include "trackknife/query/tkq.hpp"
#include "workspace/library_browser.hpp"

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariant>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class QAbstractItemModel;

namespace trackknife::bench {

// ADR-0153: the standalone search. One tkq/word query, several scopes:
// this computer's library, another engine's, or a snapshot of the current
// local tab through the shared row evaluation (probing missing technicals
// on demand). Saved searches, presets, and what is found put in a tab.
// The window's Search dialog draws it.
class SearchSession final : public QObject {
    Q_OBJECT

  public:
    struct TabSnapshot {
        QString title;
        std::vector<LocalTrackRow> rows;
    };
    using TabAccess = std::function<std::optional<TabSnapshot>()>;
    using TechnicalsSink = std::function<void(std::string, LocalTrackTechnicals)>;

    // Another engine's library, offered as a scope of its own (ADR-0227,
    // ADR-0234) under its name.
    struct OtherLibrary {
        EngineKey engine;
        const CatalogueSource* catalogues{nullptr};
        QString name;
    };
    struct Scope {
        QString label;
        // "local", "tab", or an engine's key.
        QString value;
    };
    // A line of the results: a heading, or an artist, album or track found.
    struct Result {
        QString label;
        bool heading{false};
        persistence::LibraryEntryKind kind{persistence::LibraryEntryKind::track};
        std::size_t index{0U};
    };
    // What a preset asks before it is used.
    struct PresetInput {
        enum class Kind { none, integer, text };
        Kind kind{Kind::none};
        QString title;
        QString prompt;
        QString value;
        int minimum{0};
        int maximum{0};
        int step{1};
    };
    struct Preset {
        QString title;
        QString id;
        // Into query::search_presets().
        int index{0};
    };
    struct PresetGroup {
        QString topic;
        std::vector<Preset> presets;
    };

    SearchSession(const CatalogueSource& catalogues, TabAccess tab_access,
                  TechnicalsSink technicals_sink, std::vector<OtherLibrary> others = {},
                  QObject* parent = nullptr);
    ~SearchSession() override;

    // The draft.
    [[nodiscard]] const std::vector<Scope>& scopes() const { return scopes_; }
    [[nodiscard]] int scope() const { return scope_; }
    [[nodiscard]] QString text() const { return text_; }
    [[nodiscard]] bool queryMode() const { return query_mode_; }
    void setScope(int index);
    void setText(const QString& text);
    void setQueryMode(bool on);
    // The library of the tab it was opened from: its engine's. "Current
    // tab" is kept.
    void followLibrary(const EngineKey& engine);
    // The current tab's model, whose edits rerun a current-tab search.
    void watchCurrentModel(QAbstractItemModel* model);

    // What is shown.
    [[nodiscard]] QString error() const { return error_; }
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] const std::vector<Result>& results() const { return results_; }
    [[nodiscard]] bool canOpen() const { return !result_rows_.empty() || !result_paths_.empty(); }

    // Saved searches: "Saved searches…" first.
    [[nodiscard]] QStringList savedNames() const;
    [[nodiscard]] QStringList savedTooltips() const;
    [[nodiscard]] int savedIndex() const { return saved_index_; }
    [[nodiscard]] QString savedStatus() const { return saved_status_; }
    [[nodiscard]] bool savedAvailable() const { return catalog_ready_ && !catalog_busy_; }
    [[nodiscard]] bool canSave() const;
    [[nodiscard]] bool canUpdate() const;
    [[nodiscard]] bool canRenameOrDelete() const;
    [[nodiscard]] QString suggestedName() const { return text_.trimmed().left(80); }
    [[nodiscard]] QString selectedName() const;
    [[nodiscard]] QString deleteQuestion() const;
    // Chosen in the list, not yet run.
    void selectSaved(int index);
    // Chosen and run.
    void useSaved(int index);
    void saveAs(const QString& name);
    void update();
    void rename(const QString& name);
    void remove();

    // Presets: a starting point, adjusted and saved as a search of one's own.
    [[nodiscard]] static std::vector<PresetGroup> presetGroups();
    [[nodiscard]] static PresetInput presetInput(int preset);
    // Used, with what was asked (ignored when nothing is).
    void usePreset(int preset, const QString& value);

    // What is found, put somewhere: every track, or the rows chosen (in
    // list order) with artists and albums resolved to their tracks.
    void openAll(LocalLibraryAction action);
    void openRows(std::vector<int> rows, LocalLibraryAction action);

  signals:
    void changed();
    void resultsChanged();
    void savedChanged();
    // Every scope carries cached rows directly; opening never starts file
    // discovery. `engine`: whose library the rows are, for a tab of that
    // engine.
    void rowsRequested(QString name, std::vector<LocalTrackRow> rows, LocalLibraryAction action,
                       const trackknife::ui::EngineKey& engine);

  private:
    struct Outcome {
        std::vector<std::string> labels;
        std::vector<LocalTrackRow> rows;
        // A query in a library: every match by its file, its rows read only
        // when they are played or opened (the labels come formatted).
        std::vector<std::string> paths;
        // Words in a library: artists and albums as well as tracks, each
        // shown under its own heading.
        bool grouped{false};
        std::vector<persistence::LibraryEntry> artists;
        std::vector<persistence::LibraryEntry> albums;
        std::vector<std::pair<std::string, LocalTrackTechnicals>> probed;
        QString error;
        std::size_t scanned{0U};
    };

    void loadSavedSearches(std::optional<persistence::SavedSearch> write = std::nullopt,
                           bool remove = false);
    void finishSavedSearches();
    [[nodiscard]] std::optional<persistence::SavedSearch> selectedSearch() const;
    void scheduleSearch();
    void startSearch();
    void finishSearch();
    [[nodiscard]] std::optional<query::CompiledTkq> compileInput();
    [[nodiscard]] bool databaseScope() const;
    [[nodiscard]] EngineKey scopeEngine() const;
    [[nodiscard]] const CatalogueSource* scopeCatalogues() const;
    [[nodiscard]] QString resultName() const;
    // Rows for the chosen results, read on a worker, then rowsRequested.
    void resolveTracks(std::vector<std::string> paths,
                       std::vector<persistence::LibraryEntry> entries,
                       std::vector<LocalTrackRow> picked, LocalLibraryAction action);

    const CatalogueSource* catalogues_{nullptr};
    std::vector<OtherLibrary> others_;
    TabAccess tab_access_;
    TechnicalsSink technicals_sink_;
    std::vector<Scope> scopes_;
    int scope_{0};
    QString text_;
    bool query_mode_{false};
    QString error_;
    QString status_;
    std::vector<Result> results_;
    std::vector<QMetaObject::Connection> current_model_connections_;
    std::vector<persistence::SavedSearch> catalog_;
    int saved_index_{0};
    QString saved_status_;
    QFutureWatcher<core::Result<std::vector<persistence::SavedSearch>>> catalog_watcher_;
    std::optional<core::StableId> catalog_selection_;
    bool catalog_busy_{false};
    bool catalog_ready_{false};
    std::size_t search_job_generation_{0U};
    QTimer debounce_;
    QFutureWatcher<Outcome> watcher_;
    core::CancellationSource cancellation_;
    std::size_t generation_{0U};
    bool searching_{false};
    // The last successful search's full result payload.
    std::vector<LocalTrackRow> result_rows_;
    // The matches of a query in a library, when they are not rows yet.
    std::vector<std::string> result_paths_;
    std::vector<persistence::LibraryEntry> result_artists_;
    std::vector<persistence::LibraryEntry> result_albums_;
    EngineKey result_engine_{EngineKey::local()};
    QString result_query_;
};

} // namespace trackknife::bench
