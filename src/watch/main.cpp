// SPDX-License-Identifier: GPL-3.0-only

// melody-watch (ADR-0232): runs where the music is -- a NAS that cannot run
// the engine -- and tells the engine which files changed, so it re-reads
// those over its mount instead of scanning the whole library across the
// network.

#include "trackknife/core/local_sources.hpp"
#include "trackknife/protocol/client.hpp"
#include "trackknife/protocol/message.hpp"
#include "trackknife/watch/watch.hpp"

#include <sys/stat.h>

#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using trackknife::protocol::Client;
using trackknife::protocol::Endpoint;
using trackknife::watch::Change;
using trackknife::watch::FolderMapping;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t stopping = 0;

void usage(std::ostream& out) {
    out << "usage: melody-watch [--server HOST:PORT] [--password PASS | --password-file FILE]\n"
           "                    [--settle SECONDS] [--no-catch-up] FOLDER[=ENGINE_FOLDER]...\n"
           "\n"
           "Watches music folders on this machine and tells an engine elsewhere which\n"
           "files changed, so it re-reads just those instead of scanning the library\n"
           "over the network. For a NAS that cannot run melodyd.\n"
           "\n"
           "  FOLDER=ENGINE_FOLDER  a folder here, and where the engine sees it, e.g.\n"
           "                        /volume1/music=/mnt/nas/music; the same path on\n"
           "                        both when there is no '='. The engine's library\n"
           "                        must include ENGINE_FOLDER.\n"
           "  --server HOST:PORT    the engine (or $MELODY_SERVER)\n"
           "  --password PASS, --password-file FILE\n"
           "                        its password (or $MELODY_PASSWORD)\n"
           "  --settle SECONDS      wait this long after the last change before\n"
           "                        telling the engine (default 2)\n"
           "  --no-catch-up         do not compare the folders with the engine's\n"
           "                        library at start, which finds what changed while\n"
           "                        this was not running\n";
}

void say(const std::string& message) { std::cerr << "melody-watch: " << message << std::endl; }

struct Options final {
    std::string server;
    std::string password;
    std::chrono::seconds settle{2};
    bool catch_up{true};
    std::vector<FolderMapping> folders;
};

[[nodiscard]] std::optional<Options> read_options(const int argc, char** argv) {
    Options options;
    if (const char* server = std::getenv("MELODY_SERVER"); server != nullptr) {
        options.server = server;
    }
    if (const char* password = std::getenv("MELODY_PASSWORD"); password != nullptr) {
        options.password = password;
    }
    for (int index = 1; index < argc; ++index) {
        const std::string argument{argv[index]};
        const auto value = [&]() -> std::optional<std::string> {
            if (index + 1 >= argc) {
                say(argument + " wants a value");
                return std::nullopt;
            }
            return std::string{argv[++index]};
        };
        if (argument == "--help" || argument == "-h") {
            usage(std::cout);
            std::exit(EXIT_SUCCESS);
        } else if (argument == "--server") {
            auto given = value();
            if (!given) {
                return std::nullopt;
            }
            options.server = *given;
        } else if (argument == "--password") {
            auto given = value();
            if (!given) {
                return std::nullopt;
            }
            options.password = *given;
        } else if (argument == "--password-file") {
            auto given = value();
            if (!given) {
                return std::nullopt;
            }
            std::ifstream file{*given};
            std::getline(file, options.password);
            while (!options.password.empty() &&
                   (options.password.back() == '\r' || options.password.back() == ' ')) {
                options.password.pop_back();
            }
            if (options.password.empty()) {
                say("no password in " + *given);
                return std::nullopt;
            }
        } else if (argument == "--settle") {
            auto given = value();
            if (!given) {
                return std::nullopt;
            }
            try {
                options.settle = std::chrono::seconds{std::max(0, std::stoi(*given))};
            } catch (const std::exception&) {
                say("--settle wants a number of seconds");
                return std::nullopt;
            }
        } else if (argument == "--no-catch-up") {
            options.catch_up = false;
        } else if (argument.starts_with("--")) {
            say("unknown option " + argument);
            return std::nullopt;
        } else {
            auto mapping = trackknife::watch::parse_mapping(argument);
            if (!mapping) {
                say("not a folder, or not absolute: " + argument);
                return std::nullopt;
            }
            options.folders.push_back(std::move(*mapping));
        }
    }
    if (options.folders.empty() || options.server.empty()) {
        usage(std::cerr);
        return std::nullopt;
    }
    return options;
}

class Watcher final {
  public:
    explicit Watcher(Options options, Endpoint endpoint)
        : options_(std::move(options)), endpoint_(std::move(endpoint)) {
        if (options_.catch_up) {
            for (std::size_t index = 0; index < options_.folders.size(); ++index) {
                catch_up_.insert(index);
            }
        }
    }

    int run() {
        auto made = trackknife::watch::TreeWatcher::create();
        if (!made) {
            say(made.error().message);
            return EXIT_FAILURE;
        }
        tree_ = std::move(*made);
        for (const auto& folder : options_.folders) {
            if (auto added = tree_->add_tree(folder.local); !added) {
                say("cannot watch " + folder.local + ": " + added.error().message);
                return EXIT_FAILURE;
            }
        }
        say("watching " + std::to_string(tree_->watched()) + " folders for " + endpoint_.describe());
        if (tree_->exhausted()) {
            say("out of inotify watches: some folders are not watched. Raise "
                "fs.inotify.max_user_watches (sysctl) and restart");
        }

        auto quiet_since = Clock::now();
        auto oldest = Clock::time_point::max();
        while (stopping == 0) {
            const auto changes = tree_->read(std::chrono::milliseconds{500});
            if (!changes.empty()) {
                quiet_since = Clock::now();
                oldest = std::min(oldest, quiet_since);
                take(changes);
            }
            if (pending_.empty() && catch_up_.empty()) {
                oldest = Clock::time_point::max();
                continue;
            }
            // Once the changes settle -- an album copied in is many files --
            // or when they have been coming for long enough regardless.
            const auto now = Clock::now();
            const bool settled = now - quiet_since >= options_.settle;
            const bool overdue = oldest != Clock::time_point::max() &&
                                 now - oldest >= std::chrono::seconds{30};
            if ((settled || overdue || !catch_up_.empty()) && now >= retry_at_) {
                if (deliver()) {
                    oldest = Clock::time_point::max();
                }
            }
        }
        say("stopped");
        return EXIT_SUCCESS;
    }

  private:
    void take(const std::vector<Change>& changes) {
        for (const auto& change : changes) {
            if (change.kind == Change::Kind::overflow) {
                say("changes were lost (inotify queue overflow); comparing the folders with "
                    "the library again");
                for (std::size_t index = 0; index < options_.folders.size(); ++index) {
                    catch_up_.insert(index);
                }
                continue;
            }
            if (change.kind == Change::Kind::file) {
                if (!trackknife::core::is_audio_path(change.path) || unchanged(change.path)) {
                    continue;
                }
            }
            for (const auto& folder : options_.folders) {
                if (auto path = trackknife::watch::to_engine(folder, change.path)) {
                    pending_.insert(std::move(*path));
                    break;
                }
            }
        }
    }

    // Whether a file is as it was when it was last reported. A file opened
    // for writing and closed unwritten -- by a program reading tags the lazy
    // way, the engine's own read over NFS among them -- is reported as
    // written; sending it would have it read again, and round it goes.
    [[nodiscard]] bool unchanged(const std::string& path) {
        struct stat status {};
        if (::lstat(path.c_str(), &status) != 0) {
            reported_.erase(path);
            return false;
        }
        const auto now = std::pair{static_cast<std::int64_t>(status.st_size),
                                   status.st_mtim.tv_sec * 1'000'000'000LL + status.st_mtim.tv_nsec};
        const auto [known, added] = reported_.try_emplace(path, now);
        if (!added && known->second == now) {
            return true;
        }
        known->second = now;
        return false;
    }

    // Sends what is pending. False when the engine could not be reached;
    // what was not sent is kept for the next try.
    bool deliver() {
        if (!connected()) {
            return false;
        }
        for (auto index = catch_up_.begin(); index != catch_up_.end();) {
            if (!catch_up(options_.folders[*index])) {
                return false;
            }
            index = catch_up_.erase(index);
        }
        std::size_t sent = 0;
        std::size_t reread = 0;
        while (!pending_.empty()) {
            auto batch = Json::array();
            std::vector<std::string> taken;
            for (auto path = pending_.begin(); path != pending_.end() && taken.size() < 500U; ++path) {
                batch.push_back(trackknife::protocol::encode_raw_path(*path));
                taken.push_back(*path);
            }
            // The engine reads each file over its mount; a batch can take a
            // while, which is the engine working, not the connection gone.
            auto answer = client_->call("catalogue.refresh", Json{{"paths", std::move(batch)}},
                                        std::chrono::minutes{10});
            if (!answer) {
                failed("could not send changes", answer.error());
                return false;
            }
            for (const auto& path : taken) {
                pending_.erase(path);
            }
            sent += taken.size();
            if (const auto count = answer->find("refreshed");
                count != answer->end() && count->is_array() && !count->empty() &&
                count->front().is_number_unsigned()) {
                reread += count->front().get<std::size_t>();
            }
        }
        if (sent > 0) {
            say("sent " + std::to_string(sent) + " changed paths; the library changed for " +
                std::to_string(reread));
        }
        backoff_ = std::chrono::seconds{2};
        return true;
    }

    [[nodiscard]] bool catch_up(const FolderMapping& folder) {
        std::vector<trackknife::watch::IndexedFile> indexed;
        std::string after;
        while (true) {
            auto params = Json{{"path", trackknife::protocol::encode_raw_path(folder.engine)},
                               {"limit", 2000}};
            if (!after.empty()) {
                params["after"] = trackknife::protocol::encode_raw_path(after);
            }
            auto page = client_->call("catalogue.inventory", params, std::chrono::minutes{2});
            if (!page) {
                failed("could not ask what the library holds under " + folder.engine, page.error());
                return false;
            }
            for (const auto& entry : page->value("entries", Json::array())) {
                auto path = trackknife::protocol::decode_raw_path(entry.value("path", std::string{}));
                if (!path) {
                    continue;
                }
                indexed.push_back(trackknife::watch::IndexedFile{
                    .path = std::move(*path),
                    .size = entry.value("size", std::uint64_t{0}),
                    .modified_seconds = entry.value("modified", std::int64_t{0}),
                    .available = entry.value("available", true)});
            }
            if (!page->value("more", false) || indexed.empty()) {
                break;
            }
            after = indexed.back().path;
        }
        const auto plan =
            trackknife::watch::plan_catch_up(trackknife::watch::present_files(folder), std::move(indexed));
        say(folder.local + ": " + std::to_string(plan.added) + " new, " +
            std::to_string(plan.changed) + " changed, " + std::to_string(plan.vanished) +
            " gone since the library last saw them");
        if (plan.vanished_withheld) {
            say("most of the library under " + folder.engine +
                " seems gone from here -- not telling the engine to drop it. Is " + folder.local +
                " the folder the engine sees as " + folder.engine + ", and is it mounted?");
        }
        pending_.insert(plan.refresh.begin(), plan.refresh.end());
        return true;
    }

    [[nodiscard]] bool connected() {
        if (client_ && client_->connected()) {
            return true;
        }
        client_.reset();
        auto made = Client::connect(endpoint_);
        if (!made) {
            failed("cannot reach the engine at " + endpoint_.describe(), made.error());
            return false;
        }
        client_ = std::move(*made);
        if (was_away_) {
            say("reached the engine again");
            was_away_ = false;
        }
        return true;
    }

    void failed(const std::string& what, const trackknife::core::Error& error) {
        // Said once, not every few seconds while the engine is away.
        if (!was_away_) {
            say(what + ": " + error.message + "; changes are kept and sent when it answers");
        }
        was_away_ = true;
        client_.reset();
        retry_at_ = Clock::now() + backoff_;
        backoff_ = std::min(backoff_ * 2, std::chrono::seconds{60});
    }

    Options options_;
    Endpoint endpoint_;
    std::unique_ptr<trackknife::watch::TreeWatcher> tree_;
    std::unique_ptr<Client> client_;
    // Engine paths waiting to be sent; a set, so a file written ten times is
    // re-read once.
    std::set<std::string> pending_;
    // Folders to compare with the library before anything else is sent.
    std::set<std::size_t> catch_up_;
    // Size and modification time of each file as last reported, by its
    // path here.
    std::unordered_map<std::string, std::pair<std::int64_t, std::int64_t>> reported_;
    Clock::time_point retry_at_{};
    std::chrono::seconds backoff_{2};
    bool was_away_{false};
};

} // namespace

int main(int argc, char** argv) {
    auto options = read_options(argc, argv);
    if (!options) {
        return EXIT_FAILURE;
    }
    auto endpoint = Endpoint::parse(options->server, options->password);
    if (!endpoint) {
        say("--server wants HOST:PORT or a socket path, got " + options->server);
        return EXIT_FAILURE;
    }
    std::signal(SIGINT, [](int) { stopping = 1; });
    std::signal(SIGTERM, [](int) { stopping = 1; });
    Watcher watcher{std::move(*options), std::move(*endpoint)};
    return watcher.run();
}
