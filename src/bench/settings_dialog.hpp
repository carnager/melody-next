// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/settings_keys.hpp"
#include "workspace/settings_session.hpp"

#include "bench/remote_engines.hpp"

#include "bench/output_profiles_widget.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"

#include <QDialog>

#include <functional>
#include <memory>
#include <vector>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QSpinBox;
class QDoubleSpinBox;
class QMenu;

namespace trackknife::discovery {
class Browser;
}

namespace trackknife::bench {

class OutputProfilesManagerWidget;

// The application settings screen (ADR-0112, ADR-0185): a paged dialog —
// General (notifications, appearance), Playback (local
// buffering and ReplayGain preamps), Naming (the reusable
// output-layout and move-destination profile managers), ReplayGain
// (set-once scan preferences), and Covers (the ADR-0184 storage policy
// captured at cover review). Simple values persist
// through QSettings on Save; profile edits persist immediately through the
// injected store.
class ShortcutSettings;
// The keys live in SettingsKeys, which needs no widgets, so what reads them
// does not pull in this dialog; SettingsDialog::x_key still names them.
class SettingsDialog final : public QDialog, public SettingsKeys {
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
    ~SettingsDialog() override;
    void showPage(Page page);
    // ADR-0237: the Naming page, with the move destinations of the engine
    // `key` names.
    void showDestinationsOf(const QString& key);
    // The Naming page, on its naming layouts.
    void showNamingLayouts();
    void editCustomBuffer();
    void focusReplayGainPreamp();


  private:
    void sync();
    void refreshEngines();
    void refreshFound();
    // A field shown from, and written to, the draft.
    void bind(QCheckBox* box, const char* key);
    void bind(QLineEdit* field, const char* key);
    void bind(QSpinBox* box, const char* key);
    void bind(QDoubleSpinBox* box, const char* key);
    void bind(QComboBox* box, const std::vector<SettingsSession::Choice>& choices,
              const char* key);
    ShortcutSettings* shortcuts_{};
    SettingsSession* session_{nullptr};
    std::vector<std::function<void()>> syncs_;
    bool syncing_{false};
    QMenu* found_menu_{nullptr};

    QListWidget* pages_{nullptr};
    QStackedWidget* stack_{nullptr};
    QCheckBox* panel_animations_{nullptr};
    QComboBox* lists_display_{nullptr};
    QCheckBox* notifications_{nullptr};
    QCheckBox* notifications_background_{nullptr};
    QComboBox* buffer_profile_{nullptr};
    QSpinBox* buffer_capacity_{nullptr};
    QSpinBox* buffer_threshold_{nullptr};
    QDoubleSpinBox* preamp_with_{nullptr};
    QDoubleSpinBox* preamp_without_{nullptr};
    QLineEdit* engine_socket_{nullptr};
    QLineEdit* engine_token_{nullptr};
    QLineEdit* remote_folder_{nullptr};
    QLineEdit* remote_mount_{nullptr};
    QComboBox* remote_stream_{nullptr};
    QListWidget* engines_view_{nullptr};
    QCheckBox* engine_share_{nullptr};
    QCheckBox* play_for_remote_{nullptr};
    QComboBox* stream_nearby_{nullptr};
    QComboBox* stream_away_{nullptr};
    QCheckBox* show_local_library_{nullptr};
    QLineEdit* engine_listen_{nullptr};
    QSpinBox* engine_stream_port_{nullptr};
    QLineEdit* engine_password_{nullptr};
    QLineEdit* engine_music_root_{nullptr};
    QLabel* engine_agent_command_{nullptr};
    QLineEdit* lastfm_key_{nullptr};
    QLineEdit* acoustid_key_{nullptr};
    QCheckBox* ratings_in_tags_{nullptr};
    OutputProfilesManagerWidget* output_profiles_{nullptr};
    QComboBox* rating_tag_scale_{nullptr};
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
