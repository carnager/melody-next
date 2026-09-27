// SPDX-License-Identifier: GPL-3.0-only

#include "bench/file_work_tools.hpp"

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
            }};
}

} // namespace trackknife::bench
