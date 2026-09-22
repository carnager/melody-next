// SPDX-License-Identifier: GPL-3.0-only

// ADR-0222: the golden corpus, run against the C++ codec. The same file is
// meant to be run by the Go implementation, which is what stops the two
// drifting; a case added here is a case both must satisfy.

#include "trackknife/protocol/dispatch.hpp"
#include "trackknife/protocol/message.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

[[nodiscard]] std::string bytes_from_hex(const std::string& hex) {
    std::string bytes;
    for (std::size_t index = 0; index + 1U < hex.size(); index += 2U) {
        bytes.push_back(static_cast<char>(std::stoi(hex.substr(index, 2U), nullptr, 16)));
    }
    return bytes;
}

void run_corpus(const std::filesystem::path& path) {
    namespace protocol = trackknife::protocol;
    std::ifstream file{path};
    require(file.is_open(), "the corpus must be readable");
    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto corpus = protocol::Json::parse(buffer.str(), nullptr, false);
    require(!corpus.is_discarded(), "the corpus must be valid JSON");
    require(corpus.at("protocol").at("version") == 1, "this test runs protocol v1");

    std::size_t checked = 0;
    for (const auto& entry : corpus.at("cases")) {
        const auto id = entry.at("id").get<std::string>();
        const auto line = entry.at("line").get<std::string>();
        const auto& expect = entry.at("expect");
        const auto kind = expect.at("kind").get<std::string>();
        auto parsed = protocol::parse_message(line);

        if (kind == "error") {
            require(!parsed, id + ": must be rejected");
            ++checked;
            continue;
        }
        require(parsed.has_value(), id + ": must parse");

        if (kind == "request") {
            const auto* request = std::get_if<protocol::Request>(&*parsed);
            require(request != nullptr, id + ": must be a request");
            require(request->id == expect.at("id").get<std::int64_t>(), id + ": id");
            require(request->method == expect.at("method").get<std::string>(), id + ": method");
            require(request->params == expect.at("params"), id + ": params");
        } else if (kind == "notification") {
            const auto* notification = std::get_if<protocol::Notification>(&*parsed);
            require(notification != nullptr, id + ": must be a notification");
            require(notification->method == expect.at("method").get<std::string>(),
                    id + ": method");
            require(notification->params == expect.at("params"), id + ": params");
        } else if (kind == "response") {
            const auto* response = std::get_if<protocol::Response>(&*parsed);
            require(response != nullptr, id + ": must be a response");
            require(response->id == expect.at("id").get<std::int64_t>(), id + ": id");
            if (expect.contains("result")) {
                require(response->result.has_value(), id + ": must carry a result");
                require(*response->result == expect.at("result"), id + ": result");
                require(!response->error.has_value(), id + ": must not also carry an error");
            } else {
                require(response->error.has_value(), id + ": must carry an error");
                const auto& failure = expect.at("error");
                require(response->error->code == failure.at("code").get<std::string>(),
                        id + ": code");
                require(response->error->message == failure.at("message").get<std::string>(),
                        id + ": message");
                require(response->error->context == failure.at("context"), id + ": context");
            }
        } else if (kind == "event") {
            const auto* event = std::get_if<protocol::Event>(&*parsed);
            require(event != nullptr, id + ": must be an event");
            require(event->name == expect.at("event").get<std::string>(), id + ": event name");
            require(event->data == expect.at("data"), id + ": data");
        } else {
            require(false, id + ": unknown expected kind");
        }

        // Round trip: re-encoding a parsed message must parse back identically.
        // Byte equality with the input is deliberately not required, since
        // absent optional members are filled in on the way out.
        const auto encoded = protocol::encode_message(*parsed);
        require(encoded.find('\n') == std::string::npos,
                id + ": an encoded message must never contain a newline");
        auto reparsed = protocol::parse_message(encoded);
        require(reparsed.has_value(), id + ": re-encoded message must parse");
        require(*reparsed == *parsed, id + ": round trip must preserve the message");
        ++checked;
    }

    for (const auto& entry : corpus.at("raw_path_cases")) {
        const auto id = entry.at("id").get<std::string>();
        const auto raw = bytes_from_hex(entry.at("raw_bytes_hex").get<std::string>());
        const auto expected = entry.at("encoded").get<std::string>();
        require(protocol::encode_raw_path(raw) == expected, id + ": encoding");
        auto decoded = protocol::decode_raw_path(expected);
        require(decoded.has_value(), id + ": decoding must succeed");
        require(*decoded == raw, id + ": decoding must recover the exact bytes");
        ++checked;
    }

    // Error codes are the wire contract: the strings, not the enumerator
    // names. Every code must round trip, and the corpus must name all of them
    // so adding one to the enum without deciding its wire name fails here.
    std::size_t codes = 0;
    for (const auto& entry : corpus.at("error_code_cases")) {
        const auto name = entry.at("code").get<std::string>();
        const auto code = protocol::error_code_from_name(name);
        require(protocol::error_code_name(code) == name, name + ": must round trip");
        ++codes;
    }
    require(codes == 10U, "the corpus must name every core::ErrorCode");
    // An unrecognised code is not a parse failure: a newer engine may report
    // something this client has never heard of.
    require(protocol::error_code_from_name("from_the_future") ==
                trackknife::core::ErrorCode::invariant,
            "an unknown code must land on invariant rather than being rejected");

    protocol::Dispatcher dispatcher;
    dispatcher.on("test.echoes",
                  [](const protocol::Json& params) -> trackknife::core::Result<protocol::Json> {
                      return params;
                  });
    dispatcher.on("test.fails",
                  [](const protocol::Json&) -> trackknife::core::Result<protocol::Json> {
                      return std::unexpected(
                          trackknife::core::Error{.code = trackknife::core::ErrorCode::not_found,
                                                  .message = "no such entry",
                                                  .context = {{.key = "entry", .value = "7f3a"}}});
                  });
    require(dispatcher.knows("test.echoes"), "a registered method is known");
    require(!dispatcher.knows("test.absent"), "an unregistered method is not");

    for (const auto& entry : corpus.at("dispatch_cases")) {
        const auto id = entry.at("id").get<std::string>();
        const auto& wire = entry.at("request");
        const protocol::Request request{.id = wire.at("id").get<std::int64_t>(),
                                        .method = wire.at("method").get<std::string>(),
                                        .params = wire.at("params")};
        const auto response = dispatcher.dispatch(request);
        require(response.id == request.id, id + ": the id is echoed");
        if (entry.contains("expect_result")) {
            require(response.result.has_value(), id + ": must succeed");
            require(*response.result == entry.at("expect_result"), id + ": result");
        } else {
            require(response.error.has_value(), id + ": must fail");
            require(response.error->code == entry.at("expect_error_code").get<std::string>(),
                    id + ": error code");
            if (entry.contains("expect_context")) {
                require(response.error->context == entry.at("expect_context"), id + ": context");
            }
        }
        ++checked;
    }

    require(checked == corpus.at("cases").size() + corpus.at("raw_path_cases").size() +
                           corpus.at("dispatch_cases").size(),
            "every corpus case must be checked");
    std::cout << "protocol v1 corpus: " << checked << " cases\n";
}

void malformed_encodings_are_refused() {
    namespace protocol = trackknife::protocol;
    require(!protocol::decode_raw_path("abc"), "a length that is not a multiple of four");
    require(!protocol::decode_raw_path("a!!="), "a non-base64 character");
    require(!protocol::decode_raw_path("=AAA"), "misplaced padding");
}

} // namespace

int main(int argc, char** argv) {
    require(argc > 1, "the corpus path must be given");
    run_corpus(std::filesystem::path{argv[1]});
    malformed_encodings_are_refused();
    return EXIT_SUCCESS;
}
