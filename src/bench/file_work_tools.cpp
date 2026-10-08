// SPDX-License-Identifier: GPL-3.0-only

#include "bench/file_work_tools.hpp"

#include "bench/artwork_fitting.hpp"
#include "bench/post_back.hpp"

#include "bench/artwork_fitting.hpp"

#include <QFile>
#include <QMetaObject>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <map>
#include <utility>

namespace trackknife::bench {

FileWorkTools engineFileWorkTools(std::shared_ptr<engine::RemoteFileWork> work) {
    return FileWorkTools{
        .access = work->access(),
        .scanner =
            [work](std::span<const loudness::LoudnessScanItem> items,
                   const loudness::LoudnessScanOptions& options,
                   const loudness::LoudnessScanProgressCallback& progress,
                   const core::CancellationToken& cancellation) {
                return work->scan(items, options, progress, cancellation);
            },
        .probe =
            [work](const std::string& raw_path, const core::CancellationToken& cancellation) {
                return work->probe(raw_path, cancellation);
            },
        .artwork = work->artwork_access(),
        .stage = [work](std::span<const unsigned char> bytes) { return work->stage(bytes); },
        .preflight =
            [work](const operations::OutputPathPlan& plan,
                   const core::CancellationToken& cancellation) {
                return work->preflight(plan, cancellation);
            }};
}

operations::ArtworkImageFitter artworkFitterFor(const FileWorkTools& tools) {
    return tools.stage ? engineArtworkFitter(tools.artwork, tools.stage) : artworkFitter();
}

std::uint32_t largestCoverEdge(const metadata::ArtworkStoragePolicy& policy) {
    std::uint32_t largest = 0U;
    for (const auto& [writes, edge] :
         {std::pair{policy.embed, policy.max_embedded_edge},
          std::pair{policy.write_folder_image, policy.max_folder_edge}}) {
        if (!writes) {
            continue;
        }
        if (edge == 0U) {
            return 0U;
        }
        largest = std::max(largest, edge);
    }
    return largest;
}

core::Result<std::vector<metadata::ArtworkWritePlanIntent>>
stageReplacements(std::vector<metadata::ArtworkWritePlanIntent> intents, const FileWorkTools& tools,
                  const core::CancellationToken& cancellation,
                  const metadata::ArtworkStoragePolicy& policy) {
    if (!tools.stage) {
        return intents;
    }
    const auto edge = largestCoverEdge(policy);
    // One handover per distinct image, however many files it goes into.
    std::map<std::string, std::string> staged;
    for (auto& intent : intents) {
        if (!intent.replacement_raw_path) {
            continue;
        }
        const auto here = *intent.replacement_raw_path;
        auto found = staged.find(here);
        if (found == staged.end()) {
            auto image = metadata::read_artwork_image_file(
                here, operations::maximum_fittable_artwork_bytes, cancellation);
            if (!image) {
                return std::unexpected(std::move(image.error()));
            }
            auto bytes = metadata::read_artwork_image_bytes(
                *image, operations::maximum_fittable_artwork_bytes, cancellation);
            if (!bytes) {
                return std::unexpected(std::move(bytes.error()));
            }
            // Resized here first when every destination would shrink it: the
            // engine then makes each copy from this one.
            std::optional<FittedArtwork> fitted;
            if (edge > 0U) {
                auto fit = fitArtworkBytes(*bytes, edge, cancellation);
                if (!fit) {
                    return std::unexpected(std::move(fit.error()));
                }
                fitted = std::move(*fit);
            }
            auto there =
                fitted ? tools.stage(std::span{
                             reinterpret_cast<const unsigned char*>(fitted->bytes.constData()),
                             static_cast<std::size_t>(fitted->bytes.size())})
                       : tools.stage(*bytes);
            if (!there) {
                return std::unexpected(std::move(there.error()));
            }
            found = staged.emplace(here, there->raw_path).first;
        }
        intent.replacement_raw_path = found->second;
    }
    return intents;
}

namespace {

// Runs `work` off this thread and hands its answer to `completion` on
// `context`'s; dropped when `context` is gone.
template <typename T, typename Work>
void answered_on(QObject* context, Work work, std::function<void(core::Result<T>)> completion) {
    const QPointer<QObject> guard{context};
    static_cast<void>(QtConcurrent::run([guard, work = std::move(work),
                                         completion = std::move(completion)]() mutable {
        auto answer = work();
        postBack(guard, [answer = std::move(answer), completion = std::move(completion)]() mutable {
            completion(std::move(answer));
        });
    }));
}

[[nodiscard]] QByteArray bytes_of(const std::string& body) {
    return QByteArray{body.data(), static_cast<qsizetype>(body.size())};
}

} // namespace

MusicBrainzLookupService engineLookupService(std::shared_ptr<engine::RemoteFileWork> work,
                                             QObject* context) {
    return MusicBrainzLookupService{
        .fetch =
            [work, context](const QString& url,
                            std::function<void(core::Result<QByteArray>)> completion) {
                answered_on<QByteArray>(
                    context,
                    [work, url = url.toStdString()]() -> core::Result<QByteArray> {
                        auto body = work->fetch(url, {});
                        return body ? core::Result<QByteArray>{bytes_of(*body)}
                                    : std::unexpected(std::move(body.error()));
                    },
                    std::move(completion));
            },
        .fingerprint =
            [work, context](const QString& file_path,
                            std::function<void(core::Result<AcoustIdFingerprint>)> completion) {
                // The engine's path, as the list names the file.
                const auto encoded = QFile::encodeName(file_path);
                answered_on<AcoustIdFingerprint>(
                    context,
                    [work, raw_path = std::string{encoded.constData(),
                                                  static_cast<std::size_t>(encoded.size())}]()
                        -> core::Result<AcoustIdFingerprint> {
                        auto printed = work->fingerprint(raw_path, {});
                        if (!printed) {
                            return std::unexpected(std::move(printed.error()));
                        }
                        return AcoustIdFingerprint{
                            .duration_seconds = printed->duration_seconds,
                            .fingerprint = QString::fromStdString(printed->fingerprint)};
                    },
                    std::move(completion));
            },
        .acoustid_lookup =
            [work, context](const AcoustIdFingerprint& fingerprint,
                            std::function<void(core::Result<QByteArray>)> completion) {
                answered_on<QByteArray>(
                    context,
                    [work, printed =
                               engine::MetadataServices::Fingerprint{
                                   .duration_seconds = fingerprint.duration_seconds,
                                   .fingerprint = fingerprint.fingerprint.toStdString()}]()
                        -> core::Result<QByteArray> {
                        auto body = work->acoustid_lookup(printed, {});
                        return body ? core::Result<QByteArray>{bytes_of(*body)}
                                    : std::unexpected(std::move(body.error()));
                    },
                    std::move(completion));
            },
    };
}

} // namespace trackknife::bench
