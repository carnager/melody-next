// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "uicommon/engine_key.hpp"

#include <QMimeData>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::ui {

// An in-process selection whose raw paths are resolved asynchronously after
// the drop. Creating or hovering a drag never walks files or queries SQLite.
class LocalFilesMimeData final : public QMimeData {
  public:
    using Completion = std::function<void(std::vector<std::string>)>;
    using Resolver = std::function<void(Completion)>;
    static QString mimeType() { return QStringLiteral("application/x-trackknife-local-files"); }

    // `engine`: whose paths these are (ADR-0227, ADR-0234) -- a list of that
    // engine takes them as they are; another has them mapped, or cannot.
    explicit LocalFilesMimeData(Resolver resolver, EngineKey engine = EngineKey::local())
        : resolver_(std::move(resolver)), engine_(std::move(engine)) {
        setData(mimeType(), QByteArrayLiteral("1"));
    }
    void resolve(Completion completion) const { resolver_(std::move(completion)); }
    [[nodiscard]] const EngineKey& engine() const noexcept { return engine_; }

  private:
    Resolver resolver_;
    EngineKey engine_;
};

} // namespace trackknife::ui
