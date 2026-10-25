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
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QSpinBox;
class QDoubleSpinBox;
class QMenu;

namespace trackknife::discovery {
class Browser;
}

namespace trackknife::bench {

struct Plugin;

class OutputProfilesManager;

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
        // ADR-0286: Engines -- the engines, the library's music folders.
        engine,
        file_operations,
        ratings,
        covers,
        metadata_services,
        // ADR-0288: the plugins found, each enabled here.
        plugins,
        lastfm,
        shortcuts,
        // Not pages: File operations, opened at its tab -- naming layouts,
        // ReplayGain, Undo. (Move destinations: showDestinationsOf.)
        naming,
        replaygain,
        undo
    };
    explicit SettingsDialog(QWidget* parent = nullptr, OutputProfileStore profile_store = {},
                            std::function<QWidget*(QWidget*)> library_folders = {},
                            std::function<QWidget*(QWidget*)> lastfm = {},
                            QList<QAction*> shortcuts = {},
                            std::function<QWidget*(QWidget*)> undo_locations = {},
                            std::function<QWidget*(QWidget*)> playlist_folders = {},
                            std::function<QWidget*(QWidget*)> library_watch = {});
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
    // ADR-0300: a plugin's settings, in a dialog of its own; what it sets
    // goes into the draft on OK.
    void editPluginSettings(const Plugin& plugin);
    void refreshEngines();
    void refreshEngineRows();
    void refreshFound();
    // ADR-0286: Advanced for the engine chosen in the list; the music
    // folders of the one holding the library; what is known of an engine
    // ("connected"), by "local" or its address.
    void showChosenEngine();
    void showLibraryFolders();
    [[nodiscard]] QString engineState(const QString& address) const;
    // A field shown from, and written to, the draft.
    void bind(QCheckBox* box, const char* key);
    void bind(QLineEdit* field, const char* key);
    void bind(QSpinBox* box, const char* key);
    void bind(QDoubleSpinBox* box, const char* key);
    void bind(QComboBox* box, const std::vector<SettingsSession::Choice>& choices, const char* key);
    ShortcutSettings* shortcuts_{};
    SettingsSession* session_{nullptr};
    std::vector<std::function<void()>> syncs_;
    bool syncing_{false};
    QMenu* found_menu_{nullptr};

    QListWidget* pages_{nullptr};
    QStackedWidget* stack_{nullptr};
    QTabWidget* file_operations_{nullptr};
    QWidget* replaygain_tab_{nullptr};
    QWidget* undo_tab_{nullptr};
    QCheckBox* panel_animations_{nullptr};
    QComboBox* color_scheme_{nullptr};
    QCheckBox* notifications_{nullptr};
    QCheckBox* notifications_background_{nullptr};
    QComboBox* buffer_profile_{nullptr};
    QSpinBox* buffer_capacity_{nullptr};
    QSpinBox* buffer_threshold_{nullptr};
    QDoubleSpinBox* preamp_with_{nullptr};
    QDoubleSpinBox* preamp_without_{nullptr};
    QLineEdit* engine_socket_{nullptr};
    QLineEdit* remote_folder_{nullptr};
    QLineEdit* remote_mount_{nullptr};
    QComboBox* remote_stream_{nullptr};
    QListWidget* engines_view_{nullptr};
    QCheckBox* engine_upnp_{nullptr};
    QButtonGroup* library_holders_{nullptr};
    QGroupBox* folders_box_{nullptr};
    QStackedWidget* library_folders_{nullptr};
    QLabel* folders_missing_{nullptr};
    // ADR-0296: the library engine's playlist folder, a page per engine.
    QGroupBox* playlists_box_{nullptr};
    QStackedWidget* playlist_folders_{nullptr};
    // ADR-0322: whether the engine chosen watches its library's folders.
    QStackedWidget* library_watch_{nullptr};
    QGroupBox* remote_box_{nullptr};
    QWidget* own_password_{nullptr};
    QPushButton* remove_engine_{nullptr};
    bool adding_{false};
    QCheckBox* play_for_remote_{nullptr};
    QComboBox* stream_nearby_{nullptr};
    QComboBox* stream_away_{nullptr};
    QLineEdit* engine_listen_{nullptr};
    QSpinBox* engine_stream_port_{nullptr};
    QLineEdit* engine_password_{nullptr};
    QLineEdit* engine_music_root_{nullptr};
    QLabel* engine_agent_command_{nullptr};
    QLineEdit* lastfm_key_{nullptr};
    QLineEdit* acoustid_key_{nullptr};
    QCheckBox* ratings_in_tags_{nullptr};
    QCheckBox* rating_backup_{nullptr};
    QLineEdit* rating_backup_tag_{nullptr};
    QLabel* rating_backup_note_{nullptr};
    OutputProfilesManager* output_profiles_{nullptr};
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
