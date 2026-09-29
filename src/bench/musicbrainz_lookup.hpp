// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <functional>

namespace trackknife::bench {

// Bench-level fetch boundary: the paced ADR-0088 client behind a durable
// worker-thread cache. Empty means MusicBrainz is unavailable and Identify
// stays disabled.
// One fpcalc fingerprint of one local file.
struct AcoustIdFingerprint {
    std::size_t duration_seconds{0U};
    QString fingerprint;
};

struct MusicBrainzLookupService {
    std::function<void(const QString& url, std::function<void(core::Result<QByteArray>)>)> fetch;
    // AcoustID (ADR-0096): optional fingerprint identification. Both empty
    // means the Identify dialog offers text search only.
    std::function<void(const QString& file_path,
                       std::function<void(core::Result<AcoustIdFingerprint>)>)>
        fingerprint;
    std::function<void(const AcoustIdFingerprint& fingerprint,
                       std::function<void(core::Result<QByteArray>)>)>
        acoustid_lookup;
};

} // namespace trackknife::bench
