// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/persistence/list_repository.hpp"

#include <QByteArray>
#include <QObject>

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

class QThread;

namespace trackknife::engine {
class Workspace;
}

namespace trackknife::ui {

struct PersistedWorkspace {
    std::vector<persistence::ListDocument> lists;
    std::vector<persistence::ConnectionProfile> profiles;
    std::vector<persistence::TrackViewPreset> view_presets;
};

class ListPersistenceService final : public QObject {
    Q_OBJECT

  public:
    using WorkspaceCallback = std::function<void(PersistedWorkspace, QString)>;
    using CompletionCallback = std::function<void(QString)>;
    using UiStateCallback = std::function<void(QByteArray, QString)>;
    using ListeningHistoryCallback = std::function<void(
        std::vector<std::optional<persistence::LocalListeningHistory>>, QString)>;
    using TransformationChainsCallback =
        std::function<void(std::vector<persistence::SavedMetadataTransformationChain>, QString)>;
    using OutputProfilesCallback =
        std::function<void(std::vector<persistence::SavedOutputLayoutProfile>,
                           std::vector<persistence::SavedDestinationProfile>, QString)>;
    using EncoderPresetsCallback =
        std::function<void(std::vector<persistence::SavedEncoderPreset>, QString)>;

    explicit ListPersistenceService(std::filesystem::path database_path, QObject* parent = nullptr);
    ~ListPersistenceService() override;

    ListPersistenceService(const ListPersistenceService&) = delete;
    ListPersistenceService& operator=(const ListPersistenceService&) = delete;

    void initialize(WorkspaceCallback callback);
    void saveWorkspace(std::vector<persistence::ListDocument> lists,
                       std::vector<persistence::TrackViewPreset> presets,
                       CompletionCallback callback = {});
    void saveProfiles(std::vector<persistence::ConnectionProfile> profiles,
                      CompletionCallback callback = {});
    void loadMetadataTransformationChains(TransformationChainsCallback callback);
    void saveMetadataTransformationChain(persistence::SavedMetadataTransformationChain chain,
                                         CompletionCallback callback = {});
    void removeMetadataTransformationChain(core::StableId id, CompletionCallback callback = {});
    void loadOutputProfiles(OutputProfilesCallback callback);
    void saveOutputLayoutProfile(persistence::SavedOutputLayoutProfile profile,
                                 CompletionCallback callback = {});
    void removeOutputLayoutProfile(core::StableId id, CompletionCallback callback = {});
    void saveDestinationProfile(persistence::SavedDestinationProfile profile,
                                CompletionCallback callback = {});
    void removeDestinationProfile(core::StableId id, CompletionCallback callback = {});
    void loadEncoderPresets(EncoderPresetsCallback callback);
    void saveEncoderPreset(persistence::SavedEncoderPreset preset,
                           CompletionCallback callback = {});
    void removeEncoderPreset(core::StableId id, CompletionCallback callback = {});
    void loadUiState(QString key, UiStateCallback callback);
    void saveUiState(QString key, QByteArray value, CompletionCallback callback = {});
    void backupDatabase(std::filesystem::path destination, CompletionCallback callback);
    void recordLocalListen(persistence::ListItem source, core::StableId occurrence_id,
                           std::int64_t played_at_ms, CompletionCallback callback);
    // At most 64 qualified sources per read and four pending reads across views.
    void loadListeningHistory(std::vector<persistence::ListItem> sources,
                              ListeningHistoryCallback callback);

    // Window shutdown is the only blocking persistence boundary. Database work
    // still runs on the service thread and the call guarantees durable edits.
    [[nodiscard]] QString saveWorkspaceAndWait(std::vector<persistence::ListDocument> lists,
                                               std::vector<persistence::TrackViewPreset> presets);
    // Called from a mutation worker after physical publication. The SQLite
    // transaction stays on the serialized persistence thread.
    [[nodiscard]] core::Result<persistence::LocalMetadataRefreshResult>
    refreshLocalMetadataAndWait(persistence::LocalMetadataRefresh refresh);
    [[nodiscard]] core::Result<persistence::LocalSourceRelocationResult>
    relocateLocalSourceAndWait(persistence::LocalSourceRelocation relocation);

  signals:
    void listeningHistoryChanged();

  private:
    struct State;
    QThread* thread_{nullptr};
    QObject* worker_{nullptr};
    std::shared_ptr<State> state_;
    unsigned pending_listens_{0};
    unsigned pending_history_reads_{0};
    unsigned pending_resume_writes_{0};
};

} // namespace trackknife::ui
