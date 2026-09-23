// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/output_profiles_widget.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"

#include <QDialog>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QSpinBox;
class QDoubleSpinBox;

namespace trackknife::bench {

// The application settings screen (ADR-0112, ADR-0185): a paged dialog —
// General (notifications, appearance), Playback (local
// buffering and ReplayGain preamps), Naming (the reusable
// output-layout and move-destination profile managers), ReplayGain
// (set-once scan preferences), and Covers (the ADR-0184 storage policy
// captured at cover review). Simple values persist
// through QSettings on Save; profile edits persist immediately through the
// injected store.
class ShortcutSettings;
class SettingsDialog final : public QDialog {
    Q_OBJECT

  signals:
    // Re-emitted from the Naming page so open tag editors can refresh
    // their profile selectors.
    void outputProfilesChanged();

  public:
    enum class Page : std::uint8_t {
        general,
        playback,
        library,
        engine,
        naming,
        replaygain,
        covers,
        metadata_services,
        lastfm,
        shortcuts
    };
    explicit SettingsDialog(QWidget* parent = nullptr, OutputProfileStore profile_store = {},
                            std::function<QWidget*(QWidget*)> library_folders = {},
                            std::function<QWidget*(QWidget*)> lastfm = {},
                            QList<QAction*> shortcuts = {});
    void showPage(Page page);
    void editCustomBuffer();
    void focusReplayGainPreamp();
    [[nodiscard]] static metadata::ArtworkStoragePolicy artworkPolicy();

    // QSettings keys shared with the consumers.
    static constexpr auto acoustid_client_key = "musicbrainz/acoustid-client-key";
    // ADR-0220: empty means the library is opened in this process, which is
    // what it has always done. A socket path routes it through an engine
    // instead, so pointing at one is a deliberate act and the default is
    // unchanged behaviour.
    static constexpr auto library_engine_socket_key = "library/engine-socket";
    // ADR-0227: this computer's engine, when it is one already running rather
    // than the one the workspace starts. Not shown in Settings: it is for
    // tests and for developing the engine, which run their own.
    static constexpr auto library_local_engine_socket_key = "library/local-engine-socket";
    // ADR-0223: the password a TCP engine asks for, if it has one. The key
    // keeps its old name so a saved value survives.
    static constexpr auto library_engine_token_key = "library/engine-token";
    // ADR-0226/0228: how this computer's engine is shared on the network.
    static constexpr auto engine_share_key = "engine/share";
    static constexpr auto engine_listen_key = "engine/listen";
    static constexpr auto engine_listen_default = "0.0.0.0:6600";
    static constexpr auto engine_stream_port_key = "engine/stream-port";
    static constexpr int engine_stream_port_default = 6601;
    static constexpr auto engine_password_key = "engine/password";
    static constexpr auto engine_music_root_key = "engine/music-root";
    static constexpr auto replaygain_sidecar_only_key = "replaygain/sidecar-only";
    static constexpr auto replaygain_true_peak_key = "replaygain/true-peak";
    static constexpr auto artwork_embed_key = "artwork/embed";
    static constexpr auto artwork_folder_image_key = "artwork/write-folder-image";
    static constexpr auto artwork_folder_image_name_key = "artwork/folder-image-name";
    static constexpr auto artwork_fetch_source_key = "artwork/fetch-source";
    // Longest edge of a newly written cover, in pixels; 0 is no limit.
    static constexpr auto artwork_max_embedded_edge_key = "artwork/max-embedded-edge";
    static constexpr auto artwork_max_folder_edge_key = "artwork/max-folder-edge";

  private:
    void save();
    ShortcutSettings* shortcuts_{};

    QListWidget* pages_{nullptr};
    QStackedWidget* stack_{nullptr};
    QCheckBox* panel_animations_{nullptr};
    QCheckBox* notifications_{nullptr};
    QCheckBox* notifications_background_{nullptr};
    QComboBox* buffer_profile_{nullptr};
    QSpinBox* buffer_capacity_{nullptr};
    QSpinBox* buffer_threshold_{nullptr};
    QDoubleSpinBox* preamp_with_{nullptr};
    QDoubleSpinBox* preamp_without_{nullptr};
    QLineEdit* engine_socket_{nullptr};
    QLineEdit* engine_token_{nullptr};
    QCheckBox* engine_share_{nullptr};
    QLineEdit* engine_listen_{nullptr};
    QSpinBox* engine_stream_port_{nullptr};
    QLineEdit* engine_password_{nullptr};
    QLineEdit* engine_music_root_{nullptr};
    QLabel* engine_agent_command_{nullptr};
    QLineEdit* lastfm_key_{nullptr};
    QLineEdit* acoustid_key_{nullptr};
    QCheckBox* replaygain_sidecar_only_{nullptr};
    QCheckBox* replaygain_true_peak_{nullptr};
    QCheckBox* artwork_embed_{nullptr};
    QCheckBox* artwork_folder_image_{nullptr};
    QComboBox* artwork_folder_image_name_{nullptr};
    QComboBox* artwork_fetch_source_{nullptr};
    QSpinBox* artwork_max_embedded_edge_{nullptr};
    QSpinBox* artwork_max_folder_edge_{nullptr};
};

} // namespace trackknife::bench
