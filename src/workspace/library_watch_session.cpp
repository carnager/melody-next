// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/library_watch_session.hpp"

#include "bench/post_back.hpp"

#include <QPointer>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace trackknife::bench {
namespace {

struct Asked {
    std::optional<bool> watching;
    // Given on the engine's command line, which wins over its file.
    bool from_flags{false};
    QString error;
};

[[nodiscard]] std::optional<bool> toggle(std::string text) {
    std::ranges::transform(text, text.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (text == "yes" || text == "true" || text == "on" || text == "1") {
        return true;
    }
    if (text == "no" || text == "false" || text == "off" || text == "0") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] Asked ask(const protocol::Endpoint& endpoint) {
    auto client = protocol::Client::connect(endpoint);
    if (!client) {
        return {.watching = std::nullopt, .from_flags = false,
                .error = QString::fromStdString(client.error().message)};
    }
    auto answer = (*client)->call("config.get", protocol::Json::object(), std::chrono::seconds{5});
    (*client)->close();
    if (!answer) {
        return {.watching = std::nullopt, .from_flags = false,
                .error = QString::fromStdString(answer.error().message)};
    }
    Asked asked{.watching = true, .from_flags = false, .error = {}};
    if (const auto settings = answer->find("settings");
        settings != answer->end() && settings->is_object()) {
        if (const auto watch = settings->find("watch");
            watch != settings->end() && watch->is_string()) {
            asked.watching = toggle(watch->get<std::string>()).value_or(true);
        }
    }
    if (const auto flags = answer->find("from_flags"); flags != answer->end() && flags->is_array()) {
        asked.from_flags = std::ranges::any_of(*flags, [](const protocol::Json& key) {
            return key.is_string() && key.get<std::string>() == "watch";
        });
    }
    return asked;
}

} // namespace

LibraryWatchSession::LibraryWatchSession(protocol::Endpoint endpoint, QObject* parent)
    : QObject(parent), endpoint_(std::move(endpoint)) {}

void LibraryWatchSession::load() {
    if (busy_) {
        return;
    }
    busy_ = true;
    status_ = tr("Asking the engine…");
    emit changed();
    const QPointer self{this};
    static_cast<void>(QtConcurrent::run([self, endpoint = endpoint_] {
        auto asked = ask(endpoint);
        postBack(self, [self, asked = std::move(asked)] {
            self->busy_ = false;
            if (!asked.watching) {
                self->known_ = false;
                self->status_ = tr("Could not ask the engine · %1").arg(asked.error);
            } else {
                self->known_ = true;
                self->watching_ = *asked.watching;
                self->status_ = asked.from_flags
                                    ? tr("Set on the engine's command line, which wins over "
                                         "this")
                                    : QString{};
            }
            emit self->changed();
        });
    }));
}

void LibraryWatchSession::setWatching(const bool on) {
    if (busy_ || !known_ || on == watching_) {
        return;
    }
    busy_ = true;
    watching_ = on;
    status_ = tr("Restarting the engine…");
    emit changed();
    const QPointer self{this};
    static_cast<void>(QtConcurrent::run([self, endpoint = endpoint_, on] {
        QString error;
        if (auto client = protocol::Client::connect(endpoint); !client) {
            error = QString::fromStdString(client.error().message);
        } else {
            // Watching is the engine's default: only off is written.
            auto change = protocol::Json{{"key", "watch"},
                                         {"value", on ? protocol::Json(nullptr)
                                                      : protocol::Json("no")}};
            auto set = (*client)->call("config.set",
                                       protocol::Json{{"changes", protocol::Json::array({change})}},
                                       std::chrono::seconds{5});
            if (!set) {
                error = QString::fromStdString(set.error().message);
            } else {
                // The engine answers, then stops; its service starts it again.
                static_cast<void>(
                    (*client)->call("engine.restart", protocol::Json::object(),
                                    std::chrono::seconds{5}));
            }
            (*client)->close();
        }
        postBack(self, [self, error, on] {
            self->busy_ = false;
            if (!error.isEmpty()) {
                self->watching_ = !on;
                self->status_ = tr("The engine did not take it · %1").arg(error);
                emit self->changed();
                return;
            }
            // Asked again once it is back, to show what it runs with.
            self->status_ = tr("Restarting the engine…");
            emit self->changed();
            QTimer::singleShot(std::chrono::seconds{3}, self, [self] {
                if (self) {
                    self->load();
                }
            });
        });
    }));
}

} // namespace trackknife::bench
