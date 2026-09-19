// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/output_profiles_widget.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QStackedWidget;

namespace trackknife::bench {

// The application settings screen (ADR-0112, ADR-0185): a paged dialog —
// General (startup context, MPD music folder), Naming (the reusable
// output-layout and move-destination profile managers), ReplayGain
// (set-once scan preferences), and Covers (the ADR-0184 storage policy
// keys; enforcement lands with the policy work). Simple values persist
// through QSettings on Save; profile edits persist immediately through the
// injected store.
class SettingsDialog final : public QDialog {
    Q_OBJECT

  signals:
    // Re-emitted from the Naming page so open tag editors can refresh
    // their profile selectors.
    void outputProfilesChanged();

  public:
    enum class Page : std::uint8_t { general, naming, replaygain, covers };
    explicit SettingsDialog(QWidget* parent = nullptr,
                            OutputProfileStore profile_store = {});
    void showPage(Page page);

    // QSettings keys shared with the consumers.
    static constexpr auto startup_context_key = "startup/context";
    static constexpr auto music_root_key = "mpd/music-root";
    static constexpr auto replaygain_sidecar_only_key = "replaygain/sidecar-only";
    static constexpr auto replaygain_true_peak_key = "replaygain/true-peak";
    static constexpr auto artwork_embed_key = "artwork/embed";
    static constexpr auto artwork_folder_image_key = "artwork/write-folder-image";
    static constexpr auto artwork_folder_image_name_key = "artwork/folder-image-name";
    static constexpr auto artwork_fetch_source_key = "artwork/fetch-source";

  private:
    void save();

    QListWidget* pages_{nullptr};
    QStackedWidget* stack_{nullptr};
    QComboBox* startup_{nullptr};
    QLineEdit* music_root_{nullptr};
    QCheckBox* replaygain_sidecar_only_{nullptr};
    QCheckBox* replaygain_true_peak_{nullptr};
    QCheckBox* artwork_embed_{nullptr};
    QCheckBox* artwork_folder_image_{nullptr};
    QComboBox* artwork_folder_image_name_{nullptr};
    QComboBox* artwork_fetch_source_{nullptr};
};

} // namespace trackknife::bench
