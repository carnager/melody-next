// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/list_persistence_service.hpp"
#include "trackknife/persistence/workspace_backup.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QString>
#include <QThread>

#include <optional>
#include <type_traits>
#include <utility>

#if defined(TRACKKNIFE_THREAD_SANITIZER)
extern "C" void __tsan_acquire(void* address);
extern "C" void __tsan_release(void* address);
#endif

namespace trackknife::ui {
namespace {

void threadSanitizerAcquire(void* const address) noexcept {
#if defined(TRACKKNIFE_THREAD_SANITIZER)
    __tsan_acquire(address);
#else
    static_cast<void>(address);
#endif
}

void threadSanitizerRelease(void* const address) noexcept {
#if defined(TRACKKNIFE_THREAD_SANITIZER)
    __tsan_release(address);
#else
    static_cast<void>(address);
#endif
}

[[nodiscard]] QString errorText(const core::Error& error) {
    return QString::fromStdString(error.message);
}

[[nodiscard]] QString settingsErrorText(const QSettings::Status status) {
    switch (status) {
    case QSettings::NoError:
        return {};
    case QSettings::AccessError:
        return QStringLiteral("Settings storage could not be accessed");
    case QSettings::FormatError:
        return QStringLiteral("Settings storage has an invalid format");
    }
    return QStringLiteral("Settings storage failed");
}

// The system Qt libraries are not ThreadSanitizer-instrumented, so their
// queued-event synchronization is invisible to TSan. Model the real
// enqueue/dequeue happens-before edge around each callable without suppressing
// accesses in project code.
template <typename Callable> class QueuedInvocation final {
  public:
    explicit QueuedInvocation(Callable callable) : callable_(std::move(callable)) { release(); }
    QueuedInvocation(const QueuedInvocation& other) : callable_(other.callable_) { release(); }
    QueuedInvocation(QueuedInvocation&& other) noexcept(
        std::is_nothrow_move_constructible_v<Callable>)
        : callable_(std::move(other.callable_)) {
        release();
    }

    QueuedInvocation& operator=(const QueuedInvocation&) = delete;
    QueuedInvocation& operator=(QueuedInvocation&&) = delete;

    void operator()() {
        acquire();
        std::invoke(callable_);
    }

  private:
    void release() noexcept {
#if defined(TRACKKNIFE_THREAD_SANITIZER)
        __tsan_release(&synchronization_);
#endif
    }

    void acquire() noexcept {
#if defined(TRACKKNIFE_THREAD_SANITIZER)
        __tsan_acquire(&synchronization_);
#endif
    }

    char synchronization_{0};
    Callable callable_;
};

template <typename Callable> void invokeQueued(QObject* receiver, Callable&& callable) {
    static_cast<void>(QMetaObject::invokeMethod(
        receiver, QueuedInvocation<std::decay_t<Callable>>{std::forward<Callable>(callable)},
        Qt::QueuedConnection));
}

template <typename Callable> void invokeBlocking(QObject* receiver, Callable&& callable) {
    char completion{0};
    threadSanitizerRelease(&completion);
    static_cast<void>(QMetaObject::invokeMethod(
        receiver,
        QueuedInvocation{[callable = std::forward<Callable>(callable), &completion]() mutable {
            std::invoke(callable);
            threadSanitizerRelease(&completion);
        }},
        Qt::BlockingQueuedConnection));
    threadSanitizerAcquire(&completion);
}

} // namespace

struct ListPersistenceService::State {
    std::filesystem::path database_path;
    std::optional<persistence::ListRepository> repository;
    QString initialization_error;
};

ListPersistenceService::ListPersistenceService(std::filesystem::path database_path, QObject* parent)
    : QObject(parent), thread_(new QThread(this)), worker_(new QObject),
      state_(std::make_shared<State>(State{.database_path = std::move(database_path),
                                           .repository = std::nullopt,
                                           .initialization_error = {}})) {
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_->setObjectName(QStringLiteral("trackknife-persistence"));
    thread_->start();
}

ListPersistenceService::~ListPersistenceService() {
    if (thread_ == nullptr || worker_ == nullptr) {
        return;
    }
    invokeBlocking(worker_, [state = state_] { state->repository.reset(); });
    thread_->quit();
    thread_->wait();
}

void ListPersistenceService::initialize(WorkspaceCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, callback = std::move(callback)]() mutable {
        PersistedWorkspace snapshot;
        auto opened = persistence::ListRepository::open(state->database_path);
        if (!opened) {
            state->initialization_error = errorText(opened.error());
        } else {
            state->repository.emplace(std::move(*opened));
            auto lists = state->repository->load_all();
            auto profiles = state->repository->load_profiles();
            auto presets = state->repository->load_view_presets();
            if (!lists) {
                state->initialization_error = errorText(lists.error());
            } else if (!profiles) {
                state->initialization_error = errorText(profiles.error());
            } else if (!presets) {
                state->initialization_error = errorText(presets.error());
            } else {
                snapshot.lists = std::move(*lists);
                snapshot.profiles = std::move(*profiles);
                snapshot.view_presets = std::move(*presets);
            }
        }
        if (!self) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), snapshot = std::move(snapshot),
                            error = state->initialization_error]() mutable {
            callback(std::move(snapshot), std::move(error));
        });
    });
}

void ListPersistenceService::saveWorkspace(std::vector<persistence::ListDocument> lists,
                                           std::vector<persistence::TrackViewPreset> presets,
                                           CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, lists = std::move(lists),
                           presets = std::move(presets), callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && state->repository) {
            if (auto stored = state->repository->replace_all(lists); !stored) {
                error = errorText(stored.error());
            } else if (auto stored_presets = state->repository->replace_view_presets(presets);
                       !stored_presets) {
                error = errorText(stored_presets.error());
            }
        }
        if (!callback || !self) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::saveProfiles(std::vector<persistence::ConnectionProfile> profiles,
                                          CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, profiles = std::move(profiles),
                           callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && state->repository) {
            if (auto stored = state->repository->replace_profiles(profiles); !stored) {
                error = errorText(stored.error());
            }
        }
        if (!callback || !self) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::backupDatabase(std::filesystem::path destination,
                                            CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, destination = std::move(destination),
                           callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            auto backup =
                persistence::create_workspace_database_backup(state->database_path, destination);
            if (!backup) {
                error = errorText(backup.error());
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::recordLocalListen(persistence::ListItem source,
                                               core::StableId occurrence_id,
                                               std::int64_t played_at_ms,
                                               CompletionCallback callback) {
    if (pending_listens_ >= 16) {
        if (callback)
            callback(QStringLiteral("Listening history is busy; this listen was not saved."));
        return;
    }
    ++pending_listens_;
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, source = std::move(source), occurrence_id,
                           played_at_ms, callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository)
            error = QStringLiteral("Listening history persistence is not initialized");
        else if (error.isEmpty()) {
            auto stored =
                state->repository->record_local_listen(source, occurrence_id, played_at_ms);
            if (!stored)
                error = errorText(stored.error());
        }
        if (!self)
            return;
        invokeQueued(self, [self, callback = std::move(callback), error]() mutable {
            if (!self)
                return;
            --self->pending_listens_;
            if (error.isEmpty())
                emit self->listeningHistoryChanged();
            if (callback)
                callback(error);
        });
    });
}

void ListPersistenceService::loadListeningHistory(std::vector<persistence::ListItem> sources,
                                                  ListeningHistoryCallback callback) {
    if (sources.size() > 64 || pending_history_reads_ >= 4) {
        callback({}, QStringLiteral("Listening-history read limit reached. Try again."));
        return;
    }
    ++pending_history_reads_;
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, sources = std::move(sources),
                           callback = std::move(callback)]() mutable {
        std::vector<std::optional<persistence::LocalListeningHistory>> results;
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository)
            error = QStringLiteral("Listening history persistence is not initialized");
        if (error.isEmpty()) {
            results.reserve(sources.size());
            for (const auto& source : sources) {
                auto loaded = state->repository->lookup_local_listening_history(source);
                if (!loaded) {
                    error = errorText(loaded.error());
                    results.clear();
                    break;
                }
                results.push_back(std::move(*loaded));
            }
        }
        if (!self)
            return;
        invokeQueued(self, [self, callback = std::move(callback), results = std::move(results),
                            error]() mutable {
            if (!self)
                return;
            --self->pending_history_reads_;
            callback(std::move(results), error);
        });
    });
}

void ListPersistenceService::loadMetadataTransformationChains(
    TransformationChainsCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, callback = std::move(callback)]() mutable {
        std::vector<persistence::SavedMetadataTransformationChain> chains;
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            auto loaded = state->repository->load_metadata_transformation_chains();
            if (!loaded) {
                error = errorText(loaded.error());
            } else {
                chains = std::move(*loaded);
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), chains = std::move(chains),
                            error]() mutable { callback(std::move(chains), error); });
    });
}

void ListPersistenceService::saveMetadataTransformationChain(
    persistence::SavedMetadataTransformationChain chain, CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, chain = std::move(chain),
                           callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            if (auto stored = state->repository->upsert_metadata_transformation_chain(chain);
                !stored) {
                error = errorText(stored.error());
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::removeMetadataTransformationChain(core::StableId id,
                                                               CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, id, callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            if (auto removed = state->repository->remove_metadata_transformation_chain(id);
                !removed) {
                error = errorText(removed.error());
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::loadOutputProfiles(OutputProfilesCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, callback = std::move(callback)]() mutable {
        std::vector<persistence::SavedOutputLayoutProfile> layouts;
        std::vector<persistence::SavedDestinationProfile> destinations;
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            auto loaded_layouts = state->repository->load_output_layout_profiles();
            if (!loaded_layouts) {
                error = errorText(loaded_layouts.error());
            } else {
                auto loaded_destinations = state->repository->load_destination_profiles();
                if (!loaded_destinations) {
                    error = errorText(loaded_destinations.error());
                } else {
                    layouts = std::move(*loaded_layouts);
                    destinations = std::move(*loaded_destinations);
                }
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), layouts = std::move(layouts),
                            destinations = std::move(destinations), error]() mutable {
            callback(std::move(layouts), std::move(destinations), error);
        });
    });
}

void ListPersistenceService::loadEncoderPresets(EncoderPresetsCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, callback = std::move(callback)]() mutable {
        std::vector<persistence::SavedEncoderPreset> presets;
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            if (auto loaded = state->repository->load_encoder_presets(); loaded) {
                presets = std::move(*loaded);
            } else {
                error = errorText(loaded.error());
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), presets = std::move(presets),
                            error]() mutable { callback(std::move(presets), error); });
    });
}

void ListPersistenceService::saveEncoderPreset(persistence::SavedEncoderPreset preset,
                                               CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, preset = std::move(preset),
                           callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            if (auto stored = state->repository->upsert_encoder_preset(preset); !stored) {
                error = errorText(stored.error());
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::removeEncoderPreset(core::StableId id, CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, id, callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            if (auto removed = state->repository->remove_encoder_preset(id); !removed) {
                error = errorText(removed.error());
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::saveOutputLayoutProfile(persistence::SavedOutputLayoutProfile profile,
                                                     CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, profile = std::move(profile),
                           callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            if (auto stored = state->repository->upsert_output_layout_profile(profile); !stored) {
                error = errorText(stored.error());
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::removeOutputLayoutProfile(core::StableId id,
                                                       CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, id, callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            if (auto removed = state->repository->remove_output_layout_profile(id); !removed) {
                error = errorText(removed.error());
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::saveDestinationProfile(persistence::SavedDestinationProfile profile,
                                                    CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, profile = std::move(profile),
                           callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            if (auto stored = state->repository->upsert_destination_profile(profile); !stored) {
                error = errorText(stored.error());
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::removeDestinationProfile(core::StableId id,
                                                      CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, id, callback = std::move(callback)]() mutable {
        QString error = state->initialization_error;
        if (error.isEmpty() && !state->repository) {
            error = QStringLiteral("List persistence is not initialized");
        } else if (error.isEmpty()) {
            if (auto removed = state->repository->remove_destination_profile(id); !removed) {
                error = errorText(removed.error());
            }
        }
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::loadUiState(QString key, UiStateCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, key = std::move(key), callback = std::move(callback)]() mutable {
        QSettings settings;
        auto value = settings.value(key).toByteArray();
        auto error = settingsErrorText(settings.status());
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), value = std::move(value),
                            error = std::move(error)]() mutable {
            callback(std::move(value), std::move(error));
        });
    });
}

void ListPersistenceService::saveLocalResume(std::optional<LocalResumeCheckpoint> checkpoint,
                                             CompletionCallback callback) {
    if (pending_resume_writes_ >= 2) {
        if (callback)
            callback(QStringLiteral("Playback resume save is busy"));
        return;
    }
    ++pending_resume_writes_;
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, checkpoint = std::move(checkpoint),
                           callback = std::move(callback)]() mutable {
        QString error;
        QByteArray payload;
        if (checkpoint) {
            if (!state->repository)
                error = QStringLiteral("Playback resume storage is unavailable");
            else {
                auto key = state->repository->local_listening_key(checkpoint->source);
                if (!key)
                    error = errorText(key.error());
                else {
                    payload =
                        QJsonDocument(
                            QJsonObject{{QStringLiteral("version"), 1},
                                        {QStringLiteral("list"), checkpoint->list_id},
                                        {QStringLiteral("row"), checkpoint->row},
                                        {QStringLiteral("source"), QString::fromStdString(*key)},
                                        {QStringLiteral("position_ms"),
                                         QString::number(checkpoint->position_ms)}})
                            .toJson(QJsonDocument::Compact);
                }
            }
        }
        if (error.isEmpty()) {
            QSettings settings;
            settings.setValue(QStringLiteral("playback/local-resume-v1"), payload);
            settings.sync();
            error = settingsErrorText(settings.status());
        }
        if (!self)
            return;
        invokeQueued(self, [self, callback = std::move(callback), error]() mutable {
            if (!self)
                return;
            --self->pending_resume_writes_;
            if (callback)
                callback(error);
        });
    });
}

void ListPersistenceService::verifyLocalResumeSource(persistence::ListItem source,
                                                     QString expected_key,
                                                     CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, state = state_, source = std::move(source),
                           expected_key = std::move(expected_key),
                           callback = std::move(callback)]() mutable {
        QString error;
        if (!state->repository)
            error = QStringLiteral("Playback resume storage is unavailable");
        else {
            auto key = state->repository->local_listening_key(source);
            if (!key)
                error = errorText(key.error());
            else if (QString::fromStdString(*key) != expected_key)
                error = QStringLiteral("Saved playback track no longer matches this list entry");
        }
        if (!self)
            return;
        invokeQueued(self, [callback = std::move(callback), error]() mutable { callback(error); });
    });
}

void ListPersistenceService::saveUiState(QString key, QByteArray value,
                                         CompletionCallback callback) {
    const QPointer self{this};
    invokeQueued(worker_, [self, key = std::move(key), value = std::move(value),
                           callback = std::move(callback)]() mutable {
        QSettings settings;
        settings.setValue(key, value);
        settings.sync();
        auto error = settingsErrorText(settings.status());
        if (!self || !callback) {
            return;
        }
        invokeQueued(self, [callback = std::move(callback), error = std::move(error)]() mutable {
            callback(std::move(error));
        });
    });
}

QString
ListPersistenceService::saveWorkspaceAndWait(std::vector<persistence::ListDocument> lists,
                                             std::vector<persistence::TrackViewPreset> presets) {
    QString error;
    invokeBlocking(
        worker_, [state = state_, lists = std::move(lists), presets = std::move(presets), &error] {
            error = state->initialization_error;
            if (!error.isEmpty() || !state->repository) {
                return;
            }
            if (auto stored = state->repository->replace_all(lists); !stored) {
                error = errorText(stored.error());
            } else if (auto stored_presets = state->repository->replace_view_presets(presets);
                       !stored_presets) {
                error = errorText(stored_presets.error());
            }
        });
    return error;
}

core::Result<persistence::LocalMetadataRefreshResult>
ListPersistenceService::refreshLocalMetadataAndWait(persistence::LocalMetadataRefresh refresh) {
    if (QThread::currentThread() == thread()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invariant,
            .message = "Metadata refresh cannot block the UI thread",
            .context = {},
        });
    }
    if (QThread::currentThread() == thread_) {
        if (!state_->initialization_error.isEmpty() || !state_->repository) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = state_->initialization_error.isEmpty()
                               ? "List persistence is not initialized"
                               : state_->initialization_error.toStdString(),
                .context = {},
            });
        }
        return state_->repository->refresh_local_metadata(refresh);
    }
    std::optional<core::Result<persistence::LocalMetadataRefreshResult>> result;
    invokeBlocking(worker_, [state = state_, refresh = std::move(refresh), &result] {
        if (!state->initialization_error.isEmpty() || !state->repository) {
            result = std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = state->initialization_error.isEmpty()
                               ? "List persistence is not initialized"
                               : state->initialization_error.toStdString(),
                .context = {},
            });
            return;
        }
        result = state->repository->refresh_local_metadata(refresh);
    });
    if (!result) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invariant,
            .message = "Metadata refresh did not return a persistence result",
            .context = {},
        });
    }
    return std::move(*result);
}

core::Result<persistence::LocalSourceRelocationResult>
ListPersistenceService::relocateLocalSourceAndWait(persistence::LocalSourceRelocation relocation) {
    if (QThread::currentThread() == thread()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invariant,
            .message = "Local-source relocation cannot block the UI thread",
            .context = {},
        });
    }
    if (QThread::currentThread() == thread_) {
        if (!state_->initialization_error.isEmpty() || !state_->repository) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = state_->initialization_error.isEmpty()
                               ? "List persistence is not initialized"
                               : state_->initialization_error.toStdString(),
                .context = {},
            });
        }
        return state_->repository->relocate_local_source(relocation);
    }
    std::optional<core::Result<persistence::LocalSourceRelocationResult>> result;
    invokeBlocking(worker_, [state = state_, relocation = std::move(relocation), &result] {
        if (!state->initialization_error.isEmpty() || !state->repository) {
            result = std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = state->initialization_error.isEmpty()
                               ? "List persistence is not initialized"
                               : state->initialization_error.toStdString(),
                .context = {},
            });
            return;
        }
        result = state->repository->relocate_local_source(relocation);
    });
    if (!result) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invariant,
            .message = "Local-source relocation did not return a persistence result",
            .context = {},
        });
    }
    return std::move(*result);
}

} // namespace trackknife::ui
