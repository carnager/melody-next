// SPDX-License-Identifier: GPL-3.0-only

// ADR-0228: melody-agent, an output an engine plays on. It connects to a
// melodyd, registers under a name, and plays what the engine asks on this
// machine's audio. The engine's player decides what plays; this makes sound.

#include "agent/agent.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic_bool stop_requested{false};

void request_stop(int) { stop_requested.store(true); }

void usage() {
    std::cerr
        << "usage: melody-agent --server HOST:PORT [--token TOKEN | --token-file FILE]\n"
        << "                    [--name NAME] [--music-root DIR] [--stream]\n"
        << "\n"
        << "  --server        the engine: HOST:PORT (melodyd --listen), or a unix socket path\n"
        << "  --token         the engine's token, from its engine.token (TCP only)\n"
        << "  --token-file    read the token from FILE instead\n"
        << "  --name          what the engine calls this output (default: the host name)\n"
        << "  --music-root    where the engine's music is on this machine: files it names\n"
        << "                  relative to its own root are opened under this one\n"
        << "  --stream        this machine cannot open the files; the engine streams them\n"
        << "                  (it needs --http for that)\n";
}

[[nodiscard]] std::string host_name() {
    std::string name(256, '\0');
    if (::gethostname(name.data(), name.size()) != 0) {
        return "melody-agent";
    }
    name.resize(std::char_traits<char>::length(name.c_str()));
    return name;
}

} // namespace

int main(int argc, char** argv) {
    std::string server;
    std::string token;
    std::string token_file;
    trackknife::agent::AgentConfig config;
    config.name = host_name();

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto value = [&]() -> std::string {
            return index + 1 < argc ? argv[++index] : std::string{};
        };
        if (argument == "--server") {
            server = value();
        } else if (argument == "--token") {
            token = value();
        } else if (argument == "--token-file") {
            token_file = value();
        } else if (argument == "--name") {
            config.name = value();
        } else if (argument == "--music-root") {
            config.music_root = std::filesystem::path{value()};
        } else if (argument == "--stream") {
            config.stream_only = true;
        } else if (argument == "--help" || argument == "-h") {
            usage();
            return EXIT_SUCCESS;
        } else {
            std::cerr << "melody-agent: unrecognised argument " << argument << "\n\n";
            usage();
            return EXIT_FAILURE;
        }
    }
    if (!token_file.empty()) {
        std::ifstream file{token_file};
        std::getline(file, token);
        while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) {
            token.pop_back();
        }
        if (token.empty()) {
            std::cerr << "melody-agent: no token in " << token_file << "\n";
            return EXIT_FAILURE;
        }
    }
    const auto endpoint = trackknife::protocol::Endpoint::parse(server, token);
    if (!endpoint) {
        std::cerr << "melody-agent: --server wants HOST:PORT or a socket path\n\n";
        usage();
        return EXIT_FAILURE;
    }
    if (endpoint->tcp() && token.empty()) {
        std::cerr << "melody-agent: a TCP engine needs its token (--token or --token-file)\n";
        return EXIT_FAILURE;
    }
    config.server = *endpoint;

    auto agent = trackknife::agent::Agent::create(config);
    if (!agent) {
        std::cerr << "melody-agent: " << agent.error().message << "\n";
        return EXIT_FAILURE;
    }
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    std::signal(SIGPIPE, SIG_IGN);
    (*agent)->start();
    std::cerr << "melody-agent: \"" << config.name << "\" playing for " << endpoint->describe()
              << "\n";
    while (!stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    (*agent)->stop();
    return EXIT_SUCCESS;
}
