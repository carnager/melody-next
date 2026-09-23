// SPDX-License-Identifier: GPL-3.0-only

// ADR-0222: the catalogue methods end to end -- a framed request line in, a
// framed response line out, against a real database. This is the first test
// that exercises the protocol against the engine rather than against a stub,
// and it links no Qt, which is the property that lets the engine be a daemon.

#include "trackknife/engine/catalogue_methods.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/protocol/message.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace protocol = trackknife::protocol;
namespace engine = trackknife::engine;
namespace core = trackknife::core;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

// Round trip through the wire format rather than calling dispatch directly:
// encoding and parsing are part of what these methods have to survive.
[[nodiscard]] protocol::Response call(const protocol::Dispatcher& dispatcher, const std::int64_t id,
                                      const std::string& method, const protocol::Json& params) {
    const auto line =
        protocol::encode_message(protocol::Request{.id = id, .method = method, .params = params});
    auto request = protocol::parse_message(line);
    require(request.has_value(), "the encoded request must parse");
    const auto* typed = std::get_if<protocol::Request>(&*request);
    require(typed != nullptr, "it must parse back as a request");
    const auto response = dispatcher.dispatch(*typed);
    const auto response_line = protocol::encode_message(response);
    auto reparsed = protocol::parse_message(response_line);
    require(reparsed.has_value(), "the encoded response must parse");
    const auto* answer = std::get_if<protocol::Response>(&*reparsed);
    require(answer != nullptr, "it must parse back as a response");
    require(answer->id == id, "the id must survive the round trip");
    return *answer;
}

void catalogue_methods_answer_over_the_wire(const std::filesystem::path& database) {
    engine::LocalCatalogue catalogue{database};
    protocol::Dispatcher dispatcher;
    engine::register_catalogue_methods(dispatcher, catalogue);

    // An empty catalogue answers with an empty list, which is a success.
    const auto roots = call(dispatcher, 1, "catalogue.roots", protocol::Json::object());
    require(roots.result.has_value(), "roots must succeed on a fresh catalogue");
    require(roots.result->at("roots").is_array(), "roots must be an array");
    require(roots.result->at("roots").empty(), "a fresh catalogue has no roots");

    // A query that matches nothing is also a success, not a not_found.
    const auto empty = call(dispatcher, 2, "catalogue.filter_paths",
                            protocol::Json{{"query", "artist HAS nothing-matches-this"}});
    require(empty.result.has_value(), "a query matching nothing still succeeds");
    require(empty.result->at("paths").empty(), "and returns no paths");

    // A malformed query is the caller's mistake, reported as such.
    const auto broken =
        call(dispatcher, 3, "catalogue.filter_paths", protocol::Json{{"query", "artist HAS"}});
    require(broken.error.has_value(), "an unparseable query must fail");
    require(broken.error->code == "invalid_argument", "and be the caller's fault");

    // A missing parameter names itself, so a client need not parse prose.
    const auto missing = call(dispatcher, 4, "catalogue.filter_paths", protocol::Json::object());
    require(missing.error.has_value(), "a missing query must fail");
    require(missing.error->code == "invalid_argument", "as a bad argument");
    require(missing.error->context.at("param") == "query", "naming the parameter at fault");

    // Wrong types are refused rather than coerced.
    const auto wrong_type =
        call(dispatcher, 5, "catalogue.ratings", protocol::Json{{"hashes", "not-an-array"}});
    require(wrong_type.error.has_value(), "hashes must be an array");
    require(wrong_type.error->context.at("param") == "hashes", "naming the parameter");

    // Ratings round trip, and an unrated hash reads back as zero rather than
    // as an absence the caller has to interpret.
    // Rating identities are 64-character content hashes (ADR-0179); the
    // engine refuses anything else rather than storing a row nothing can find.
    const std::string rated(64U, 'a');
    const std::string unrated(64U, 'b');
    const auto malformed_hash =
        call(dispatcher, 6, "catalogue.set_rating",
             protocol::Json{{"hash", "abc123"}, {"album", false}, {"rating", 7}});
    require(malformed_hash.error.has_value(), "a short hash is refused");
    require(malformed_hash.error->code == "invalid_argument", "as a bad argument");

    const auto negative = call(dispatcher, 10, "catalogue.set_rating",
                               protocol::Json{{"hash", rated}, {"album", false}, {"rating", -1}});
    require(negative.error.has_value(), "a negative rating is refused");
    require(negative.error->context.at("param") == "rating", "naming the parameter");

    const auto stored = call(dispatcher, 11, "catalogue.set_rating",
                             protocol::Json{{"hash", rated}, {"album", false}, {"rating", 7}});
    require(stored.result.has_value(), "storing a rating must succeed");
    const auto read = call(dispatcher, 12, "catalogue.ratings",
                           protocol::Json{{"hashes", protocol::Json::array({rated, unrated})}});
    require(read.result.has_value(), "reading ratings must succeed");
    const auto& ratings = read.result->at("ratings");
    require(ratings.size() == 2U, "one rating per requested hash, in order");
    require(ratings[0] == 7U, "the stored rating comes back");
    require(ratings[1] == 0U, "an unrated hash is zero, not missing");

    // An album with no artwork is a success carrying null: the album may
    // exist, the cover does not.
    // Keys are bytes and travel encoded, like paths.
    const auto artwork =
        call(dispatcher, 8, "catalogue.artwork_source",
             protocol::Json{{"album_key", protocol::encode_raw_path("no-such-album")}});
    require(artwork.result.has_value(), "a missing cover is not an error");
    require(artwork.result->at("source").is_null(), "and reports null");
    const auto unencoded = call(dispatcher, 13, "catalogue.artwork_source",
                                protocol::Json{{"album_key", "not base64!"}});
    require(unencoded.error.has_value(), "an album key that is not encoded is refused");

    // An unknown method is answered, never dropped.
    const auto unknown = call(dispatcher, 9, "catalogue.invented", protocol::Json::object());
    require(unknown.error.has_value(), "an unknown method must answer");
    require(unknown.error->code == "unsupported", "as unsupported");

    std::cout << "catalogue methods: 12 calls\n";
}

void jobs_submit_and_cancel_over_the_wire(const std::filesystem::path& database) {
    engine::LocalCatalogue catalogue{database};

    std::mutex mutex;
    std::vector<protocol::Event> events;
    engine::JobRegistry registry{[&](const protocol::Event& event) {
        const std::lock_guard guard{mutex};
        events.push_back(event);
    }};
    engine::JobCatalog jobs;
    engine::register_catalogue_jobs(jobs, catalogue);

    protocol::Dispatcher dispatcher;
    engine::register_job_methods(dispatcher, registry, jobs);

    // A job nobody registered fails at submit, with the name it did not know.
    const auto unknown =
        call(dispatcher, 20, "job.submit", protocol::Json{{"job", "catalogue.invented"}});
    require(unknown.error.has_value(), "an unknown job must fail");
    require(unknown.error->code == "unsupported", "as unsupported");
    require(unknown.error->context.at("job") == "catalogue.invented", "naming the job");

    // Missing and malformed parameters are the caller's mistake, reported in
    // the response to their own submit rather than through an event.
    const auto nameless = call(dispatcher, 21, "job.submit", protocol::Json::object());
    require(nameless.error.has_value(), "a submit with no job name must fail");
    require(nameless.error->context.at("param") == "job", "naming the parameter");

    const auto bad_identity =
        call(dispatcher, 22, "job.cancel", protocol::Json{{"job_id", "not-an-identity"}});
    require(bad_identity.error.has_value(), "a malformed identity must fail");
    require(bad_identity.error->context.at("param") == "job_id", "naming the parameter");

    // A real job: scanning an empty catalogue finishes quickly, but still
    // answers immediately with an identity rather than when it is done.
    const auto submitted =
        call(dispatcher, 23, "job.submit", protocol::Json{{"job", "catalogue.scan"}});
    require(submitted.result.has_value(), "submitting a known job succeeds");
    const auto job_id = submitted.result->at("job_id").get<std::string>();
    require(!job_id.empty(), "and answers with an identity");

    registry.wait_all();

    bool finished = false;
    {
        const std::lock_guard guard{mutex};
        for (const auto& event : events) {
            require(event.data.at("job_id") == job_id, "every event names its job");
            if (event.name == "job.finished") {
                finished = true;
                require(event.data.at("job") == "catalogue.scan", "the finish names the job");
                require(event.data.at("outcome").contains("visited"),
                        "and carries the scan's counters");
            }
        }
    }
    require(finished, "a submitted job must report a finish");

    // Cancelling a job that has already finished is answered, not an error:
    // the client learns the identity is no longer running.
    const auto late = call(dispatcher, 24, "job.cancel", protocol::Json{{"job_id", job_id}});
    require(late.result.has_value(), "cancelling a finished job still answers");
    require(late.result->at("accepted") == false, "reporting that it was not running");

    std::cout << "job methods: 6 calls\n";
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-engine-methods-" + core::StableId::random().to_string());
    std::filesystem::create_directory(directory);
    const auto database = directory / "library.sqlite3";
    catalogue_methods_answer_over_the_wire(database);
    jobs_submit_and_cancel_over_the_wire(database);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    return EXIT_SUCCESS;
}
