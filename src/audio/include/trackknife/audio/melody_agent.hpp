// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/local_audition.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::audio {

inline constexpr int melody_agent_protocol_version = 2;

// Melody keys outputs by name, so the default must distinguish computers.
[[nodiscard]] std::string default_melody_agent_name();

struct MelodyAgentConfig {
    std::string name{default_melody_agent_name()};
    std::string host{"127.0.0.1"};
    unsigned port{6600U};
    std::optional<std::string> local_music_root;
    std::optional<std::string> stream_base_url;
    std::string stream_format;
    std::optional<unsigned> maximum_bit_rate;
    std::chrono::milliseconds reconnect_delay{2'000};
    std::chrono::milliseconds report_period{2'000};

    friend bool operator==(const MelodyAgentConfig&, const MelodyAgentConfig&) = default;
};

struct MelodyAgentQueueItem {
    int position{-1};
    std::string uri;
    std::string song_id;
    double duration_seconds{0.0};
    std::optional<double> track_gain_db;
    std::optional<double> album_gain_db;

    friend bool operator==(const MelodyAgentQueueItem&, const MelodyAgentQueueItem&) = default;
};

struct MelodyAgentSnapshot {
    bool connected{false};
    bool registered{false};
    std::uint64_t session_generation{0U};
    std::uint64_t queue_version{0U};
    std::size_t queue_item_count{0U};
    std::size_t queue_gain_item_count{0U};
    int current_position{-1};
    int preloaded_position{-1};
    std::string visible_track_identity;
    bool using_direct_source{false};
    ReplayGainMode replay_gain_mode{ReplayGainMode::off};
    std::optional<double> track_gain_db;
    std::optional<double> album_gain_db;
    std::optional<double> received_track_gain_db;
    std::optional<double> received_album_gain_db;
    float effective_gain_multiplier{1.0F};
    std::optional<core::Error> issue;

    friend bool operator==(const MelodyAgentSnapshot&, const MelodyAgentSnapshot&) = default;
};

// Resolves one Melody queue item without changing its visible identity. A
// configured root accepts only contained relative MPD paths; otherwise the
// stable song ID selects the server stream endpoint.
[[nodiscard]] core::Result<std::pair<std::string, bool>>
resolve_melody_agent_source(const MelodyAgentConfig& config, const MelodyAgentQueueItem& item);

// A dedicated server-authority output. It owns only its protocol worker; PCM,
// PipeWire, gapless preload, ReplayGain and the real-time boundary remain in
// the supplied LocalAuditionService. Public construction never blocks.
class MelodyAgentService final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<MelodyAgentService>>
    create(MelodyAgentConfig config, LocalAuditionService& player);

    MelodyAgentService(const MelodyAgentService&) = delete;
    MelodyAgentService& operator=(const MelodyAgentService&) = delete;
    MelodyAgentService(MelodyAgentService&&) = delete;
    MelodyAgentService& operator=(MelodyAgentService&&) = delete;
    ~MelodyAgentService();

    [[nodiscard]] MelodyAgentSnapshot snapshot() const;
    [[nodiscard]] const std::string& name() const noexcept;

  private:
    struct Impl;
    explicit MelodyAgentService(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::audio
