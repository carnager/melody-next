// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/operations/output_path_plan.hpp"
#include "trackknife/operations/preparation_plan.hpp"
#include "workspace/tagger_session.hpp"

#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace trackknife::bench {

// ADR-0261, Apply: the albums chosen of those staged, written in one go from
// the tagger's draft -- their tags, and renamed or moved by a naming preset
// when chosen. Moved below a destination, or, without one, one plan per
// library folder, each album going under the library folder it is in. An album with a file that cannot be
// written as planned -- changed since it was read, say -- is left out,
// saying why, and the rest written.
class AlbumBatchWrite final : public QObject {
    Q_OBJECT
  public:
    struct Options {
        // The preset whose file-name pattern renames, and whose folder
        // pattern moves; none, not done.
        std::optional<operations::OutputLayoutProfile> rename;
        std::optional<operations::OutputLayoutProfile> move;
        // Where to move to; none, the library folder each album is in.
        std::optional<operations::DestinationProfile> destination;
    };
    struct Album {
        std::size_t album{0U};
        std::vector<std::size_t> items;
    };
    enum class Outcome : std::uint8_t { pending, written, partly_written, left_out, failed, stopped };
    struct AlbumOutcome {
        std::size_t album{0U};
        Outcome outcome{Outcome::pending};
        std::size_t written{0U};
        std::size_t files{0U};
        QString note;
    };

    AlbumBatchWrite(TaggerSession& tagger, std::vector<Album> albums, Options options,
                    QObject* parent = nullptr);
    ~AlbumBatchWrite() override;

    // Once; finished() comes after, also when nothing could be done.
    void start();
    // The files under way are finished; the rest are not started.
    void stop();
    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] std::size_t filesDone() const;
    [[nodiscard]] std::size_t filesTotal() const { return files_total_; }
    [[nodiscard]] const std::vector<AlbumOutcome>& outcomes() const { return outcomes_; }

  signals:
    void progressed();
    // Written and the files read again.
    void finished();

  private:
    struct Group {
        std::optional<operations::DestinationProfile> destination;
        std::vector<std::size_t> albums;
    };
    struct Progress {
        std::atomic<std::size_t> done{0U};
    };

    void planGroup();
    void planned(std::shared_ptr<core::Result<operations::PreparationPlan>> result);
    void applied(std::shared_ptr<const operations::PreparationPlan> plan,
                 std::shared_ptr<core::Result<operations::MetadataApplyResult>> tags,
                 std::shared_ptr<core::Result<operations::FilePublicationApplyResult>> files);
    void nextGroup();
    void finish();
    [[nodiscard]] AlbumOutcome& outcomeOf(std::size_t album);
    // The albums of `group` not left out, and their rows.
    [[nodiscard]] std::vector<std::size_t> itemsOf(const Group& group) const;
    void leaveOut(std::size_t item, const QString& why);
    void failGroup(const QString& why);
    [[nodiscard]] std::vector<std::size_t> itemsAt(const std::string& raw_path) const;

    QPointer<TaggerSession> tagger_;
    std::vector<Album> albums_;
    Options options_;
    operations::PreparationOperationSelection operations_;
    std::vector<Group> groups_;
    std::size_t group_{0U};
    // Planned once more without what was left out.
    bool replanned_{false};
    std::vector<AlbumOutcome> outcomes_;
    // Which album each row is in.
    std::unordered_map<std::size_t, std::size_t> album_of_;
    std::unordered_map<std::string, std::vector<std::size_t>> items_at_;
    std::vector<TaggerSession::Rewritten> rewritten_;
    std::size_t files_total_{0U};
    std::size_t files_before_{0U};
    std::shared_ptr<Progress> progress_;
    core::CancellationSource cancellation_;
    bool running_{false};
    bool stopping_{false};
};

} // namespace trackknife::bench
