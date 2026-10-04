// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/output_profile_store.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <optional>
#include <string>
#include <vector>

namespace trackknife::bench {

// The naming layouts and move destinations (ADR-0185, ADR-0237): one saved
// layout or destination edited at a time, saved, removed, a destination
// root chosen on the machine of the engine it belongs to, and this
// computer's destinations copied to an engine that reaches them under
// another name. Every engine's destinations are held at once, so they can
// be listed together (ADR-0264); the one edited is on the shown place.
// Everything is saved at once, not on the Settings screen's Save. The
// window's File operations page draws it.
class ProfilesSession final : public QObject {
    Q_OBJECT

  public:
    struct Choice {
        QString label;
        QVariant value;
    };

    explicit ProfilesSession(OutputProfileStore store, QObject* parent = nullptr);
    ~ProfilesSession() override;

    [[nodiscard]] static std::vector<Choice> sanitizationPolicies();

    // What is shown.
    [[nodiscard]] QStringList layoutNames() const;
    [[nodiscard]] QStringList destinationNames() const;
    [[nodiscard]] QStringList placeNames() const;
    [[nodiscard]] QString placeKey(int place) const;
    [[nodiscard]] int placeCount() const { return static_cast<int>(places_.size()); }
    // The destinations held for `place`, and what keeps it from showing
    // them -- loading, or an error -- if anything.
    [[nodiscard]] QStringList destinationNamesOn(int place) const;
    [[nodiscard]] QString destinationRootOn(int place, int row) const;
    [[nodiscard]] QString placeNote(int place) const;
    [[nodiscard]] int layoutRow() const;
    [[nodiscard]] int destinationRow() const;
    [[nodiscard]] int place() const { return place_; }
    [[nodiscard]] QString placeName() const;
    [[nodiscard]] QString layoutName() const { return layout_name_; }
    [[nodiscard]] QString directoryExpression() const { return directory_expression_; }
    [[nodiscard]] QString basenameExpression() const { return basename_expression_; }
    [[nodiscard]] QString sanitization() const { return sanitization_; }
    [[nodiscard]] QString destinationName() const { return destination_name_; }
    [[nodiscard]] QString destinationRoot() const;
    [[nodiscard]] const std::string& destinationRootRawPath() const { return root_raw_path_; }
    [[nodiscard]] QString status() const { return status_; }
    // Copying this computer's destinations here: how many, if any.
    [[nodiscard]] int copyable() const { return copyable_; }
    // How a root is chosen on this place's machine; empty: this computer's
    // file dialog.
    [[nodiscard]] EngineFolderLister folders() const;

    // Enablement.
    [[nodiscard]] bool available() const { return !loading_ && !mutation_running_; }
    // The shown place's destinations are held.
    [[nodiscard]] bool destinationsAvailable() const;
    [[nodiscard]] bool canChoosePlace() const { return available() && places_.size() > 1U; }
    [[nodiscard]] bool canEditLayouts() const;
    [[nodiscard]] bool canSaveLayout() const;
    [[nodiscard]] bool canRemoveLayout() const;
    [[nodiscard]] bool canEditDestinations() const;
    [[nodiscard]] bool canSaveDestination() const;
    [[nodiscard]] bool canRemoveDestination() const;

    // The place `key` names (EngineKey's spelling), or -1.
    [[nodiscard]] int placeOf(const QString& key) const;

    // What the user does.
    void selectLayout(int row);
    void newLayout() { selectLayout(-1); }
    void setLayoutName(const QString& text);
    void setDirectoryExpression(const QString& text);
    void setBasenameExpression(const QString& text);
    void setSanitization(const QString& policy);
    void saveLayout();
    void removeLayout();
    void selectPlace(int index);
    void selectDestination(int row);
    void newDestination() { selectDestination(-1); }
    // A destination of `place`, or a new one there (`row` -1): that place
    // shown.
    void selectDestinationOn(int place, int row);
    void newDestinationOn(int place) { selectDestinationOn(place, -1); }
    void setDestinationName(const QString& text);
    void chooseRoot(std::string raw_path);
    void saveDestination();
    void removeDestination();
    void copyDestinations();

  signals:
    void changed();
    // The saved layouts or destinations, or the places, were replaced.
    void listsChanged();
    // Saved: the tag editors' selectors follow.
    void profilesChanged();

  private:
    void reload();
    void reloadDestinations(int place);
    void countCopyable();
    [[nodiscard]] QString summary() const;
    [[nodiscard]] std::vector<persistence::SavedDestinationProfile>& shown() {
        return held_[static_cast<std::size_t>(place_)].destinations;
    }
    [[nodiscard]] const std::vector<persistence::SavedDestinationProfile>& shown() const {
        return held_[static_cast<std::size_t>(place_)].destinations;
    }
    void rebuildLists(std::optional<core::StableId> layout_id,
                      std::optional<core::StableId> destination_id);
    [[nodiscard]] const DestinationPlace& shownPlace() const;

    OutputProfileStore store_;
    std::vector<DestinationPlace> places_;
    int place_{0};
    std::vector<persistence::SavedOutputLayoutProfile> layouts_;
    // Each place's destinations, as places_.
    struct Held {
        std::vector<persistence::SavedDestinationProfile> destinations;
        QString error;
        bool loading{false};
    };
    std::vector<Held> held_;
    std::optional<core::StableId> editing_layout_id_;
    std::optional<core::StableId> editing_destination_id_;
    QString layout_name_;
    QString directory_expression_;
    QString basename_expression_;
    QString sanitization_{QStringLiteral("linux")};
    QString destination_name_;
    std::string root_raw_path_;
    QString status_;
    int copyable_{0};
    bool loading_{false};
    bool mutation_running_{false};
};

} // namespace trackknife::bench
