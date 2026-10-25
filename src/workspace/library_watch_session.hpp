// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/protocol/client.hpp"

#include <QObject>
#include <QString>

namespace trackknife::bench {

// ADR-0322: whether one engine watches its library's folders for changes --
// `watch` in its melodyd.conf -- asked of the engine and changed there, at
// once: it restarts to use the change. The same for this computer's engine
// and one elsewhere.
class LibraryWatchSession final : public QObject {
    Q_OBJECT

  public:
    explicit LibraryWatchSession(protocol::Endpoint endpoint, QObject* parent = nullptr);

    [[nodiscard]] bool known() const { return known_; }
    [[nodiscard]] bool busy() const { return busy_; }
    [[nodiscard]] bool watching() const { return watching_; }
    // What to say beside it: asking, restarting, why it cannot be set.
    [[nodiscard]] QString status() const { return status_; }

    void load();
    void setWatching(bool on);

  signals:
    void changed();

  private:
    protocol::Endpoint endpoint_;
    bool known_{false};
    bool busy_{false};
    bool watching_{true};
    QString status_;
};

} // namespace trackknife::bench
