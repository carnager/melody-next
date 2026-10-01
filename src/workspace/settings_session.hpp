// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/remote_engines.hpp"
#include "bench/settings_keys.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace trackknife::discovery {
class Browser;
}

namespace trackknife::bench {

// The settings screen (ADR-0112, ADR-0185), as a draft of the values Save
// writes: each by its QSettings key, read when the screen opens. Beside the
// plain values: the playback buffer's presets, the command for an output
// agent, the engines elsewhere with the ones announcing themselves on the
// network, and what Save refuses. Pages that keep their own state -- library
// folders, naming profiles, Last.fm, shortcuts -- are drawn beside it. Both
// windows' settings screens draw it.
class SettingsSession final : public QObject {
    Q_OBJECT

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
    struct Choice {
        QString label;
        QVariant value;
    };
    // An engine announcing itself on the network.
    struct Found {
        QString label;
        QString address;
    };

    explicit SettingsSession(QObject* parent = nullptr);
    ~SettingsSession() override;

    [[nodiscard]] static QStringList pageTitles();
    // What a page says about when its changes take effect, if it does not
    // wait for Save.
    [[nodiscard]] static QString saveNote(Page page);
    [[nodiscard]] static std::vector<Choice> listsDisplays();
    [[nodiscard]] static std::vector<Choice> bufferProfiles();
    [[nodiscard]] static std::vector<Choice> ratingScales();
    // ADR-0247: the system's colours, or Trackknife's light or dark scheme.
    [[nodiscard]] static std::vector<Choice> colorSchemes();
    // What to say under the backup tag's name (ADR-0245): why it cannot hold
    // the copy, a warning when it is an official tag whose content the copy
    // replaces, or nothing.
    [[nodiscard]] static QString ratingBackupNote(const QString& name);
    [[nodiscard]] static std::vector<Choice> fetchSources();
    [[nodiscard]] static QStringList folderImageNames();
    [[nodiscard]] static double maximumPreamp();
    // The rates offered for streaming to this computer's speakers: from an
    // engine nearby, through a VPN or router, and from one engine; with the
    // value set now, when it is none of them.
    [[nodiscard]] std::vector<Choice> nearbyRates() const;
    [[nodiscard]] std::vector<Choice> awayRates() const;
    [[nodiscard]] std::vector<Choice> engineRates() const;

    // The draft, by settings key.
    [[nodiscard]] QVariant value(const QString& key) const { return values_.value(key); }
    [[nodiscard]] const QVariantMap& values() const { return values_; }
    void setValue(const QString& key, const QVariant& value);
    // A preset fills the buffer's capacity and start; Custom leaves them to
    // be set.
    [[nodiscard]] bool bufferCustom() const;
    [[nodiscard]] QString agentCommand() const;

    // The engines elsewhere, as edited; one is shown.
    [[nodiscard]] QStringList engineLabels() const;
    [[nodiscard]] int currentEngine() const { return engine_current_; }
    [[nodiscard]] RemoteEngineSetting engine() const;
    void selectEngine(int row);
    void editEngine(const RemoteEngineSetting& edited);
    void addEngine();
    void removeEngine();
    void chooseFoundEngine(const QString& address);
    [[nodiscard]] const std::vector<Found>& found() const { return found_; }
    // Why no engines can be found here, if they cannot.
    [[nodiscard]] QString discoveryError() const { return discovery_error_; }

    // Written; or the page whose value Save refuses (sharing without a
    // password), untouched.
    [[nodiscard]] std::optional<Page> save();

  signals:
    // A value changed (by the user, or a preset).
    void changed();
    // The engines' list changed; `current` is the one to show.
    void enginesChanged(int current);
    void foundChanged();

  private:
    [[nodiscard]] std::vector<Choice> rates(std::initializer_list<int> offered, int current) const;

    QVariantMap values_;
    std::vector<RemoteEngineSetting> engines_;
    int engine_current_{-1};
    std::vector<Found> found_;
    QString discovery_error_;
    std::unique_ptr<discovery::Browser> browser_;
};

} // namespace trackknife::bench
