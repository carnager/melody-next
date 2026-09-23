// SPDX-License-Identifier: GPL-3.0-only

#pragma once

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

    // `remote`: the paths are a remote engine's (ADR-0227), which only a
    // tab of that engine can take.
    explicit LocalFilesMimeData(Resolver resolver, const bool remote = false)
        : resolver_(std::move(resolver)), remote_(remote) {
        setData(mimeType(), QByteArrayLiteral("1"));
    }
    void resolve(Completion completion) const { resolver_(std::move(completion)); }
    [[nodiscard]] bool remote() const noexcept { return remote_; }

  private:
    Resolver resolver_;
    bool remote_{false};
};

} // namespace trackknife::ui
