// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/artwork_export.hpp"

#include "trackknife/core/local_sources.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <ranges>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace trackknife::operations {
namespace {

constexpr std::size_t maximum_export_items = 64U;
constexpr std::size_t maximum_export_parallelism = 8U;

[[nodiscard]] core::Error export_error(const core::ErrorCode code, std::string message,
                                       const std::string& destination = {}) {
    core::Error result{.code = code, .message = std::move(message), .context = {}};
    if (!destination.empty()) {
        result.context.push_back(
            {.key = "destination", .value = core::escape_raw_path(destination)});
    }
    return result;
}

class OwnedDestination final {
  public:
    OwnedDestination(std::string raw_path, const int descriptor)
        : raw_path_{std::move(raw_path)}, descriptor_{descriptor} {}
    OwnedDestination(const OwnedDestination&) = delete;
    OwnedDestination& operator=(const OwnedDestination&) = delete;
    ~OwnedDestination() {
        if (descriptor_ >= 0) {
            static_cast<void>(::close(descriptor_));
        }
        if (owned_) {
            static_cast<void>(::unlink(raw_path_.c_str()));
        }
    }

    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] bool close() noexcept {
        const auto descriptor = std::exchange(descriptor_, -1);
        return descriptor < 0 || ::close(descriptor) == 0;
    }
    void release() noexcept { owned_ = false; }

  private:
    std::string raw_path_;
    int descriptor_{-1};
    bool owned_{true};
};

[[nodiscard]] metadata::ArtworkImageFile
image_evidence(const metadata::ArtworkInventoryItem& item) {
    return metadata::ArtworkImageFile{
        .raw_path = item.raw_source_path,
        .source_revision = item.source_revision,
        .mime_type = item.mime_type,
        .width = item.width,
        .height = item.height,
        .byte_size = item.byte_size,
        .content_fingerprint = item.content_fingerprint,
        .embedded_source_ordinal = item.provenance == metadata::ArtworkProvenance::embedded
                                       ? std::optional{item.source_ordinal}
                                       : std::nullopt,
    };
}

[[nodiscard]] core::Result<void> export_one(const ArtworkExportRequest& request,
                                            const ArtworkExportOptions& options,
                                            const core::CancellationToken& cancellation) {
    const auto maximum_item_bytes = options.maximum_item_bytes;
    if (request.destination_raw_path.empty() ||
        request.destination_raw_path.find('\0') != std::string::npos) {
        return std::unexpected(export_error(core::ErrorCode::invalid_argument,
                                            "artwork export destination is invalid",
                                            request.destination_raw_path));
    }
    auto bytes =
        options.image_bytes
            ? options.image_bytes(image_evidence(request.source), maximum_item_bytes, cancellation)
            : metadata::read_artwork_image_bytes(image_evidence(request.source), maximum_item_bytes,
                                                 cancellation);
    if (!bytes) {
        return std::unexpected(std::move(bytes.error()));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(export_error(core::ErrorCode::cancelled,
                                            "artwork export was cancelled",
                                            request.destination_raw_path));
    }
    const auto descriptor =
        ::open(request.destination_raw_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (descriptor < 0) {
        return std::unexpected(
            export_error(errno == EEXIST ? core::ErrorCode::conflict : core::ErrorCode::io,
                         errno == EEXIST ? "artwork export destination already exists"
                                         : "artwork export destination could not be created",
                         request.destination_raw_path));
    }
    OwnedDestination destination{request.destination_raw_path, descriptor};
    std::size_t written = 0U;
    while (written < bytes->size()) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(export_error(core::ErrorCode::cancelled,
                                                "artwork export was cancelled",
                                                request.destination_raw_path));
        }
        const auto amount =
            ::write(destination.descriptor(), bytes->data() + written, bytes->size() - written);
        if (amount < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(export_error(core::ErrorCode::io,
                                                "writing exported artwork failed",
                                                request.destination_raw_path));
        }
        if (amount == 0) {
            return std::unexpected(export_error(core::ErrorCode::io,
                                                "writing exported artwork made no progress",
                                                request.destination_raw_path));
        }
        written += static_cast<std::size_t>(amount);
    }
    if (::fsync(destination.descriptor()) != 0 || !destination.close()) {
        return std::unexpected(export_error(core::ErrorCode::io,
                                            "durably closing exported artwork failed",
                                            request.destination_raw_path));
    }
    const auto parent = std::filesystem::path{request.destination_raw_path}.parent_path();
    const auto parent_path = parent.empty() ? std::string{"."} : parent.native();
    const auto parent_descriptor = ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent_descriptor < 0) {
        return std::unexpected(export_error(core::ErrorCode::io,
                                            "opening artwork export directory failed",
                                            request.destination_raw_path));
    }
    if (::fsync(parent_descriptor) != 0) {
        static_cast<void>(::close(parent_descriptor));
        return std::unexpected(export_error(core::ErrorCode::io,
                                            "durably recording exported artwork failed",
                                            request.destination_raw_path));
    }
    if (::close(parent_descriptor) != 0) {
        return std::unexpected(export_error(core::ErrorCode::io,
                                            "closing artwork export directory failed",
                                            request.destination_raw_path));
    }
    destination.release();
    return {};
}

} // namespace

std::size_t ArtworkExportResult::exported_item_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(items, ArtworkExportItemState::exported,
                                                       &ArtworkExportItemResult::state));
}

std::size_t ArtworkExportResult::failed_item_count() const noexcept {
    return static_cast<std::size_t>(
        std::ranges::count(items, ArtworkExportItemState::failed, &ArtworkExportItemResult::state));
}

std::size_t ArtworkExportResult::cancelled_item_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(items, ArtworkExportItemState::cancelled,
                                                       &ArtworkExportItemResult::state));
}

core::Result<ArtworkExportResult>
export_artwork_items(const std::vector<ArtworkExportRequest>& requests,
                     const ArtworkExportProgressCallback& progress,
                     const core::CancellationToken& cancellation,
                     const ArtworkExportOptions& options) {
    if (requests.empty() || requests.size() > maximum_export_items ||
        options.maximum_parallelism == 0U ||
        options.maximum_parallelism > maximum_export_parallelism ||
        options.maximum_item_bytes == 0U) {
        return std::unexpected(export_error(
            core::ErrorCode::invalid_argument,
            "artwork export requires 1–64 items, a nonzero byte limit, and 1–8 workers"));
    }
    ArtworkExportResult result;
    result.items.reserve(requests.size());
    for (std::size_t index = 0U; index < requests.size(); ++index) {
        result.items.push_back(ArtworkExportItemResult{
            .item_index = index,
            .destination_raw_path = requests[index].destination_raw_path,
            .state = ArtworkExportItemState::pending,
            .issue = std::nullopt,
        });
    }
    std::atomic_size_t next_item{0U};
    std::atomic_size_t completed_items{0U};
    std::mutex progress_mutex;
    const auto report = [&](const std::size_t index, const ArtworkExportItemState state,
                            const std::size_t completed,
                            const std::optional<core::Error>& issue = std::nullopt) {
        if (!progress) {
            return;
        }
        const ArtworkExportProgress update{
            .item_index = index,
            .state = state,
            .completed_items = completed,
            .total_items = requests.size(),
            .issue = issue,
        };
        std::scoped_lock lock{progress_mutex};
        progress(update);
    };
    const auto worker = [&] {
        while (!cancellation.is_cancellation_requested()) {
            const auto index = next_item.fetch_add(1U, std::memory_order_relaxed);
            if (index >= requests.size()) {
                return;
            }
            auto& item = result.items[index];
            item.state = ArtworkExportItemState::running;
            report(index, item.state, completed_items.load(std::memory_order_relaxed));
            auto exported = export_one(requests[index], options, cancellation);
            if (exported) {
                item.state = ArtworkExportItemState::exported;
            } else {
                item.issue = std::move(exported.error());
                item.state = item.issue->code == core::ErrorCode::cancelled
                                 ? ArtworkExportItemState::cancelled
                                 : ArtworkExportItemState::failed;
            }
            const auto completed = completed_items.fetch_add(1U, std::memory_order_relaxed) + 1U;
            report(index, item.state, completed, item.issue);
        }
    };
    const auto worker_count = std::min(options.maximum_parallelism, requests.size());
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0U; index < worker_count; ++index) {
        workers.emplace_back(worker);
    }
    workers.clear();
    for (auto& item : result.items) {
        if (item.state != ArtworkExportItemState::pending) {
            continue;
        }
        item.state = ArtworkExportItemState::cancelled;
        item.issue = export_error(core::ErrorCode::cancelled,
                                  "artwork export was cancelled before this item started",
                                  item.destination_raw_path);
        const auto completed = completed_items.fetch_add(1U, std::memory_order_relaxed) + 1U;
        report(item.item_index, item.state, completed, item.issue);
    }
    result.cancellation_requested =
        cancellation.is_cancellation_requested() || result.cancelled_item_count() > 0U;
    return result;
}

} // namespace trackknife::operations
