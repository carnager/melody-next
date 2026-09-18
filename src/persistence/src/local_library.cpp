// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/local_library.hpp"

#include "trackknife/core/stable_id.hpp"
#include "trackknife/core/unicode.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/persistence/rating_identity.hpp"
#include "trackknife/persistence/tkq_row.hpp"

#include "library_query_internal.hpp"
#include "trackknife/titleformat/evaluator.hpp"

#include <sqlite3.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace trackknife::persistence {
namespace {

[[noreturn]] void fail(const std::string& message,
                       const core::ErrorCode code = core::ErrorCode::database) {
    throw core::Error{.code = code, .message = message, .context = {}};
}

template <typename F> auto checked(F&& operation) -> core::Result<std::invoke_result_t<F>> {
    try {
        return operation();
    } catch (const core::Error& error) {
        return std::unexpected(error);
    } catch (const std::exception& error) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::io, .message = error.what(), .context = {}});
    }
}

void execute(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        fail(sqlite3_errmsg(db));
    }
}

class Statement {
  public:
    Statement(sqlite3* db, const std::string& sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &value_, nullptr) != SQLITE_OK) {
            fail(sqlite3_errmsg(db));
        }
    }
    ~Statement() { sqlite3_finalize(value_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void text(int index, const std::string& value) {
        check(sqlite3_bind_text64(value_, index, value.data(), value.size(), SQLITE_TRANSIENT,
                                  SQLITE_UTF8));
    }
    void blob(int index, const std::string& value) {
        check(sqlite3_bind_blob64(value_, index, value.data(), value.size(), SQLITE_TRANSIENT));
    }
    void number(int index, sqlite3_int64 value) { check(sqlite3_bind_int64(value_, index, value)); }
    void reset() {
        check(sqlite3_reset(value_));
        check(sqlite3_clear_bindings(value_));
    }
    bool next() {
        const auto result = sqlite3_step(value_);
        if (result == SQLITE_ROW) {
            return true;
        }
        if (result != SQLITE_DONE) {
            fail(sqlite3_errmsg(db_));
        }
        return false;
    }
    std::string bytes(int column) const {
        const auto* data = static_cast<const char*>(sqlite3_column_blob(value_, column));
        return data == nullptr
                   ? std::string{}
                   : std::string{data,
                                 static_cast<std::size_t>(sqlite3_column_bytes(value_, column))};
    }
    sqlite3_int64 number(int column) const { return sqlite3_column_int64(value_, column); }

  private:
    void check(int result) {
        if (result != SQLITE_OK) {
            fail(sqlite3_errmsg(db_));
        }
    }
    sqlite3* db_;
    sqlite3_stmt* value_{nullptr};
};

class Transaction {
  public:
    explicit Transaction(sqlite3* db, bool read_only = false) : db_(db) {
        execute(db_, read_only ? "BEGIN" : "BEGIN IMMEDIATE");
    }
    ~Transaction() {
        if (!done_) {
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() {
        execute(db_, "COMMIT");
        done_ = true;
    }

  private:
    sqlite3* db_;
    bool done_{false};
};

class QueryCancellation {
  public:
    QueryCancellation(sqlite3* db, const core::CancellationToken& token) : db_(db) {
        if (token.is_cancellation_requested()) {
            fail("Library query cancelled", core::ErrorCode::cancelled);
        }
        sqlite3_progress_handler(
            db_, 1000,
            [](void* pointer) {
                return static_cast<const core::CancellationToken*>(pointer)
                               ->is_cancellation_requested()
                           ? 1
                           : 0;
            },
            const_cast<core::CancellationToken*>(&token));
    }
    ~QueryCancellation() { sqlite3_progress_handler(db_, 0, nullptr, nullptr); }
    QueryCancellation(const QueryCancellation&) = delete;
    QueryCancellation& operator=(const QueryCancellation&) = delete;

  private:
    sqlite3* db_;
};

std::string lower(const std::string& value) {
    const auto result = core::unicodeSimpleLower(value);
    return result ? *result : core::escape_raw_path(value);
}

class LibraryLabelContext final : public titleformat::EvaluationContext {
  public:
    explicit LibraryLabelContext(const LibraryEntry& entry) : entry_(entry) {}
    titleformat::FormatContextKind kind() const noexcept override {
        return titleformat::FormatContextKind::tree_level;
    }
    std::optional<std::string> resolveField(std::string_view name) const override {
        if (name == "artist" || name == "albumartist") {
            return entry_.artist;
        }
        if (name == "album") {
            return entry_.album;
        }
        if (name == "title") {
            return entry_.label;
        }
        if (name == "tracknumber" && entry_.track_number > 0) {
            return std::to_string(entry_.track_number);
        }
        return std::nullopt;
    }

  private:
    const LibraryEntry& entry_;
};

std::string format_label(const LibraryEntry& entry, bool search) {
    // The shipped tree uses the same versioned language as working-list views.
    const titleformat::CompileOptions options{
        .context = titleformat::FormatContextKind::tree_level, .dialect = {}, .parse_options = {}};
    static const auto artist = titleformat::compile("%albumartist%", options);
    static const auto album = titleformat::compile("%album%", options);
    static const auto track =
        titleformat::compile("$if(%tracknumber%,$num(%tracknumber%,2). ,)%title%", options);
    static const auto found_track = titleformat::compile(
        "%artist% — $if(%tracknumber%,$num(%tracknumber%,2). ,)%title%", options);
    const auto& compiled = entry.kind == LibraryEntryKind::artist  ? artist
                           : entry.kind == LibraryEntryKind::album ? album
                           : search                                ? found_track
                                                                   : track;
    if (!compiled.program) {
        fail("Invalid default library format", core::ErrorCode::invariant);
    }
    const LibraryLabelContext context{entry};
    auto rendered = titleformat::evaluate(*compiled.program, context);
    if (!rendered) {
        throw rendered.error();
    }
    return rendered->text;
}

bool contained(const std::string& path, const std::string& root) {
    return path == root || path.starts_with(root == "/" ? root : root + '/');
}

std::string revision_key(const core::LocalSourceRevision& revision) {
    // The "2:" prefix versions the indexed representation itself: rows
    // written before migration 30 (ADR-0150) mismatch once and reindex on
    // the next Refresh, backfilling field rows and technical columns.
    return "2:" + std::to_string(revision.device) + ':' + std::to_string(revision.inode) + ':' +
           std::to_string(revision.size) + ':' +
           std::to_string(revision.modification_time_seconds) + ':' +
           std::to_string(revision.modification_time_nanoseconds);
}

// A successful walk alone is insufficient: an unmounted volume can leave an
// accessible empty mountpoint. Require an absent path and a surviving directory
// on the file's previously observed device; uncertainty retains the cache.
bool confirmed_missing(const std::string& raw_path, const std::string& revision,
                       const std::string& root) {
    std::uint64_t device = 0;
    // Rows written since ADR-0150 carry a leading representation-version
    // component; the device follows it. Pre-migration rows start with the
    // device directly.
    std::string_view text{revision};
    if (text.starts_with("2:")) {
        text.remove_prefix(2U);
    }
    const auto separator = text.find(':');
    if (separator == std::string_view::npos) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + separator, device);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + separator) {
        return false;
    }
    struct stat state{};
    if (::lstat(raw_path.c_str(), &state) == 0 || errno != ENOENT) {
        return false;
    }
    auto parent = std::filesystem::path{raw_path}.parent_path();
    while (!parent.empty()) {
        if (::lstat(parent.c_str(), &state) == 0) {
            return S_ISDIR(state.st_mode) && static_cast<std::uint64_t>(state.st_dev) == device;
        }
        if (errno != ENOENT || parent == std::filesystem::path{root} ||
            parent == parent.parent_path()) {
            return false;
        }
        parent = parent.parent_path();
    }
    return false;
}

void prune_missing(sqlite3* db, const std::string& root, const std::string& generation,
                   const core::CancellationToken& cancellation) {
    std::string after;
    while (!cancellation.is_cancellation_requested()) {
        std::vector<std::pair<std::string, std::string>> candidates;
        {
            // Use the schema-28 raw-path primary-key index for keyset paging;
            // the root/seen index would repeatedly sort all missing entries.
            Statement page{db, "SELECT raw_path,revision FROM local_library_tracks "
                               "INDEXED BY sqlite_autoindex_local_library_tracks_1 WHERE "
                               "root=? AND seen<>? AND raw_path>? ORDER BY raw_path LIMIT 200"};
            page.blob(1, root);
            page.text(2, generation);
            page.blob(3, after);
            while (page.next()) {
                candidates.emplace_back(page.bytes(0), page.bytes(1));
            }
        }
        if (candidates.empty()) {
            return;
        }
        after = candidates.back().first;
        // Filesystem checks run outside the write transaction and in bounded pages.
        std::erase_if(candidates, [&](const auto& entry) {
            return cancellation.is_cancellation_requested() ||
                   !confirmed_missing(entry.first, entry.second, root);
        });
        if (cancellation.is_cancellation_requested()) {
            return;
        }
        Transaction transaction{db};
        for (const auto& [path, revision] : candidates) {
            Statement remove{db, "DELETE FROM local_library_tracks WHERE raw_path=? AND root=? "
                                 "AND revision=? AND seen<>? AND EXISTS(SELECT 1 FROM "
                                 "local_library_roots WHERE raw_path=root AND scan_token=?)"};
            remove.blob(1, path);
            remove.blob(2, root);
            remove.text(3, revision);
            remove.text(4, generation);
            remove.text(5, generation);
            remove.next();
        }
        transaction.commit();
    }
}

struct Tags {
    std::string title;
    std::string artist;
    std::string album;
    std::string release;
    std::string date;
    std::string search_track;
    int disc{0};
    int track{0};
};

Tags tags_from(const metadata::MetadataDocument& document, const std::string& path) {
    const auto value = [&](std::string_view name) {
        auto text = document.first_effective_value(name).value_or("");
        if (text.size() > 16'384U) {
            text.resize(16'384U);
        }
        return text;
    };
    const auto number = [&](std::string_view name) {
        const auto text = value(name);
        int result = 0;
        std::from_chars(text.data(), text.data() + text.size(), result);
        return result;
    };
    Tags tags;
    tags.title = value("title");
    if (tags.title.empty()) {
        tags.title = core::escape_raw_path(std::filesystem::path{path}.stem().native());
    }
    tags.artist = value("albumartist");
    if (tags.artist.empty()) {
        tags.artist = value("artist");
    }
    if (tags.artist.empty()) {
        tags.artist = "Unknown artist";
    }
    tags.album = value("album");
    if (tags.album.empty()) {
        tags.album = "Unknown album";
    }
    tags.release = value("musicbrainzalbumid");
    tags.date = value("date");
    tags.disc = number("discnumber");
    tags.track = number("tracknumber");
    tags.search_track = lower(tags.title + ' ' + tags.artist + ' ' + tags.album);
    for (const auto& artist : document.effective_values("artist")) {
        tags.search_track += ' ' + lower(artist);
    }
    return tags;
}

std::string album_key(const Tags& tags, const std::string& path) {
    if (!tags.release.empty()) {
        return "mbid:" + tags.release;
    }
    return std::to_string(tags.artist.size()) + ':' + tags.artist +
           std::to_string(tags.album.size()) + ':' + tags.album + ':' +
           core::escape_raw_path(std::filesystem::path{path}.parent_path().native());
}

// ADR-0150: technical properties retained from the probe the scan
// already runs, as typed queryable columns.
struct Technicals {
    std::string codec;
    int sample_rate{0};
    int bits{0};
    int channels{0};
    std::int64_t duration_ms{-1};
};

Technicals technicals_from(const formats::MediaProbe& probe) {
    Technicals result;
    result.duration_ms = probe.duration_ms.value_or(-1);
    if (!probe.best_audio_stream) {
        return result;
    }
    const auto found = std::ranges::find(probe.audio_streams, *probe.best_audio_stream,
                                         &formats::AudioStreamInfo::stream_index);
    if (found == probe.audio_streams.end()) {
        return result;
    }
    result.codec = found->codec_name;
    result.sample_rate = found->sample_rate;
    result.bits = formats::bits_per_sample_hint(found->sample_format);
    result.channels = found->channels;
    return result;
}

// ADR-0150: one bounded row per tag value, original bytes beside the
// normalized form, replaced wholesale inside the caller's transaction.
constexpr std::size_t maximum_field_names = 4'096U;
constexpr std::size_t maximum_field_values = 16'384U;
constexpr std::size_t maximum_field_text_bytes = 4U * 1024U * 1024U;

void write_field_rows(sqlite3* db, const std::string& raw_path,
                      const metadata::MetadataDocument& document) {
    {
        Statement remove{db, "DELETE FROM local_library_fields WHERE raw_path=?"};
        remove.blob(1, raw_path);
        remove.next();
    }
    Statement insert{db, "INSERT OR IGNORE INTO local_library_fields"
                         "(raw_path,canonical_name,position,value,value_lower) "
                         "VALUES(?,?,?,?,?)"};
    std::map<std::string, int> positions;
    std::size_t value_count = 0;
    std::size_t text_bytes = 0;
    for (const auto& field : document.fields) {
        if (field.canonical_name.empty()) {
            continue;
        }
        auto position = positions.find(field.canonical_name);
        if (position == positions.end()) {
            if (positions.size() >= maximum_field_names) {
                fail("Library metadata exceeds 4096 field names", core::ErrorCode::limit_exceeded);
            }
            if (field.canonical_name.size() > maximum_field_text_bytes - text_bytes) {
                fail("Library metadata exceeds 4 MiB of text", core::ErrorCode::limit_exceeded);
            }
            text_bytes += field.canonical_name.size();
            position = positions.emplace(field.canonical_name, 0).first;
        }
        for (const auto& value : field.values) {
            if (value.empty()) {
                continue;
            }
            if (value_count >= maximum_field_values ||
                value.size() > maximum_field_text_bytes - text_bytes) {
                fail("Library metadata exceeds 16384 values or 4 MiB of text",
                     core::ErrorCode::limit_exceeded);
            }
            ++value_count;
            text_bytes += value.size();
            insert.reset();
            insert.blob(1, raw_path);
            insert.text(2, field.canonical_name);
            insert.number(3, position->second);
            insert.blob(4, value);
            insert.blob(5, lower(value));
            insert.next();
            ++position->second;
        }
    }
    Statement complete{db,
                       "UPDATE local_library_tracks SET field_index_complete=1 WHERE raw_path=?"};
    complete.blob(1, raw_path);
    complete.next();
}

void require_complete_field_index(sqlite3* db) {
    Statement incomplete{
        db,
        "SELECT 1 FROM local_library_tracks WHERE available=1 AND field_index_complete=0 LIMIT 1"};
    if (incomplete.next()) {
        fail("The library field index is incomplete. Press Refresh in Library, wait for it to "
             "finish, then run this search again.",
             core::ErrorCode::conflict);
    }
}

void bind_tags(Statement& statement, int start, const Tags& tags, const std::string& path) {
    statement.text(start++, tags.title);
    statement.text(start++, tags.artist);
    statement.text(start++, tags.album);
    statement.text(start++, album_key(tags, path));
    statement.text(start++, tags.release);
    statement.text(start++, tags.date);
    statement.number(start++, tags.disc);
    statement.number(start++, tags.track);
    statement.text(start++, tags.search_track);
    statement.text(start, lower(tags.artist + ' ' + tags.album));
}

// --- ADR-0150: tkq filter planner and candidate evaluation ---

// One SQL parameter with its binding affinity: value_lower and raw_path
// are BLOB columns, canonical names are TEXT.
struct FilterBinding {
    enum class Kind : std::uint8_t { text, blob } kind{Kind::text};
    std::string value;
};

struct FilterClause {
    std::string sql;
    std::vector<FilterBinding> bindings;
};

// Translates one predicate to SQL against alias t, or nullopt when the
// predicate needs per-row evaluation (tkfmt expressions).
[[nodiscard]] std::optional<FilterClause>
translate_predicate(const query::TkqPredicate& predicate) {
    using query::TkqComparison;
    using query::TkqOperandKind;
    FilterClause clause;
    if (predicate.operand == TkqOperandKind::expression) {
        return std::nullopt;
    }
    if (predicate.operand == TkqOperandKind::any_field) {
        // Every word occurs in the denormalized search text or any field value.
        std::string sql;
        for (const auto& word : predicate.words) {
            if (!sql.empty()) {
                sql += " AND ";
            }
            sql += "(instr(t.search_track,?)>0 OR EXISTS(SELECT 1 FROM local_library_fields f "
                   "WHERE f.raw_path=t.raw_path AND instr(f.value_lower,?)>0))";
            clause.bindings.push_back({FilterBinding::Kind::text, word});
            clause.bindings.push_back({FilterBinding::Kind::blob, word});
        }
        clause.sql = "(" + sql + ")";
        return clause;
    }
    const auto canonical = internal::tkq_canonical_field(predicate.field);
    if (canonical == "rating" || canonical == "albumrating") {
        // ADR-0179: the stored rating joined by content identity; NULL means
        // unrated (a zero rating deletes the row, so no value is 0).
        const std::string value =
            canonical == "rating"
                ? "(SELECT r.rating FROM local_ratings r WHERE r.hash=t.rating_hash)"
                : "(SELECT r.rating FROM local_ratings r WHERE r.hash=t.album_rating_hash)";
        switch (predicate.comparison) {
        case TkqComparison::is:
            clause.sql = "(CAST(" + value + " AS TEXT)=?)";
            clause.bindings.push_back({FilterBinding::Kind::text, predicate.normalized});
            return clause;
        case TkqComparison::has: {
            std::string sql;
            for (const auto& word : predicate.words) {
                if (!sql.empty()) {
                    sql += " AND ";
                }
                sql += "instr(CAST(" + value + " AS TEXT),?)>0";
                clause.bindings.push_back({FilterBinding::Kind::text, word});
            }
            clause.sql = "(" + sql + ")";
            return clause;
        }
        case TkqComparison::greater:
        case TkqComparison::less:
        case TkqComparison::equal: {
            const auto* comparator = predicate.comparison == TkqComparison::greater ? ">"
                                     : predicate.comparison == TkqComparison::less  ? "<"
                                                                                    : "=";
            clause.sql = "(" + value + comparator + std::to_string(predicate.number) + ")";
            return clause;
        }
        case TkqComparison::present:
            clause.sql = "(" + value + " IS NOT NULL)";
            return clause;
        case TkqComparison::missing:
            clause.sql = "(" + value + " IS NULL)";
            return clause;
        }
        return std::nullopt;
    }
    if (const auto* column = internal::tkq_technical_column(canonical)) {
        const auto qualified = std::string{"t."} + column;
        switch (predicate.comparison) {
        case TkqComparison::is:
            clause.sql = "(" + qualified + "=?)";
            clause.bindings.push_back({FilterBinding::Kind::text, predicate.normalized});
            return clause;
        case TkqComparison::has: {
            std::string sql;
            for (const auto& word : predicate.words) {
                if (!sql.empty()) {
                    sql += " AND ";
                }
                sql += "instr(" + qualified + ",?)>0";
                clause.bindings.push_back({FilterBinding::Kind::text, word});
            }
            clause.sql = "(" + sql + ")";
            return clause;
        }
        case TkqComparison::greater:
        case TkqComparison::less:
        case TkqComparison::equal: {
            const auto* comparator = predicate.comparison == TkqComparison::greater ? ">"
                                     : predicate.comparison == TkqComparison::less  ? "<"
                                                                                    : "=";
            clause.sql = "(" + qualified + comparator + std::to_string(predicate.number) + ")";
            return clause;
        }
        case TkqComparison::present:
            clause.sql = canonical == "codec"      ? "(" + qualified + "<>'')"
                         : canonical == "lengthms" ? "(" + qualified + ">=0)"
                                                   : "(" + qualified + ">0)";
            return clause;
        case TkqComparison::missing:
            clause.sql = canonical == "codec"      ? "(" + qualified + "='')"
                         : canonical == "lengthms" ? "(" + qualified + "<0)"
                                                   : "(" + qualified + "<=0)";
            return clause;
        }
        return std::nullopt;
    }
    if (canonical == "date" && (predicate.comparison == TkqComparison::greater ||
                                predicate.comparison == TkqComparison::less ||
                                predicate.comparison == TkqComparison::equal)) {
        const auto* comparator = predicate.comparison == TkqComparison::greater ? ">"
                                 : predicate.comparison == TkqComparison::less  ? "<"
                                                                                : "=";
        clause.sql = "(t.date<>'' AND CAST(substr(t.date,1,4) AS INTEGER)" +
                     std::string{comparator} + std::to_string(predicate.number) + ")";
        return clause;
    }
    constexpr auto exists_head = "EXISTS(SELECT 1 FROM local_library_fields f WHERE "
                                 "f.raw_path=t.raw_path AND f.canonical_name=?";
    switch (predicate.comparison) {
    case TkqComparison::is:
        clause.sql = std::string{"("} + exists_head + " AND f.value_lower=?))";
        clause.bindings.push_back({FilterBinding::Kind::text, canonical});
        clause.bindings.push_back({FilterBinding::Kind::blob, predicate.normalized});
        break;
    case TkqComparison::has: {
        std::string sql;
        for (const auto& word : predicate.words) {
            if (!sql.empty()) {
                sql += " AND ";
            }
            sql += std::string{exists_head} + " AND instr(f.value_lower,?)>0)";
            clause.bindings.push_back({FilterBinding::Kind::text, canonical});
            clause.bindings.push_back({FilterBinding::Kind::blob, word});
        }
        clause.sql = "(" + sql + ")";
        break;
    }
    case TkqComparison::greater:
    case TkqComparison::less:
    case TkqComparison::equal: {
        const auto* comparator = predicate.comparison == TkqComparison::greater ? ">"
                                 : predicate.comparison == TkqComparison::less  ? "<"
                                                                                : "=";
        clause.sql = std::string{"("} + exists_head + " AND CAST(f.value_lower AS INTEGER)" +
                     comparator + std::to_string(predicate.number) + "))";
        clause.bindings.push_back({FilterBinding::Kind::text, canonical});
        break;
    }
    case TkqComparison::present:
        clause.sql = std::string{"("} + exists_head + "))";
        clause.bindings.push_back({FilterBinding::Kind::text, canonical});
        break;
    case TkqComparison::missing:
        clause.sql = std::string{"(NOT "} + exists_head + "))";
        clause.bindings.push_back({FilterBinding::Kind::text, canonical});
        break;
    }
    return clause;
}

[[nodiscard]] std::optional<FilterClause> translate_node(const query::CompiledTkq& compiled,
                                                         const std::size_t index) {
    const auto& node = compiled.nodes[index];
    switch (node.kind) {
    case query::TkqNodeKind::predicate:
        return translate_predicate(compiled.predicates[node.predicate_index]);
    case query::TkqNodeKind::not_node: {
        auto inner = translate_node(compiled, node.children.front());
        if (!inner) {
            return std::nullopt;
        }
        inner->sql = "(NOT " + inner->sql + ")";
        return inner;
    }
    case query::TkqNodeKind::and_node:
    case query::TkqNodeKind::or_node: {
        FilterClause clause;
        const auto* joiner = node.kind == query::TkqNodeKind::and_node ? " AND " : " OR ";
        std::string sql;
        for (const auto child : node.children) {
            auto translated = translate_node(compiled, child);
            if (!translated) {
                return std::nullopt;
            }
            if (!sql.empty()) {
                sql += joiner;
            }
            sql += translated->sql;
            clause.bindings.insert(clause.bindings.end(),
                                   std::make_move_iterator(translated->bindings.begin()),
                                   std::make_move_iterator(translated->bindings.end()));
        }
        clause.sql = "(" + sql + ")";
        return clause;
    }
    }
    return std::nullopt;
}

std::vector<LibraryRoot> read_roots(sqlite3* db) {
    Statement query{db,
                    "SELECT raw_path,available,error FROM local_library_roots ORDER BY raw_path"};
    std::vector<LibraryRoot> result;
    while (query.next()) {
        result.push_back({query.bytes(0), query.number(1) != 0, query.bytes(2)});
    }
    return result;
}

struct Filter {
    std::string sql{" WHERE 1=1"};
    std::vector<std::pair<std::string, bool>> values;
    explicit Filter(const LibraryQuery& query) {
        if (query.text.size() > 4096U) {
            fail("Search is too long", core::ErrorCode::limit_exceeded);
        }
        if (query.artist) {
            sql += " AND artist=?";
            values.emplace_back(*query.artist, false);
        }
        if (query.album_key) {
            sql += " AND album_key=?";
            values.emplace_back(*query.album_key, false);
        }
        if (query.raw_path) {
            sql += " AND raw_path=?";
            values.emplace_back(*query.raw_path, true);
        }
        std::istringstream words{lower(query.text)};
        std::string word;
        std::size_t count = 0;
        while (words >> word) {
            if (++count > 16U) {
                fail("Search supports up to 16 words", core::ErrorCode::limit_exceeded);
            }
            sql += query.kind == LibraryEntryKind::track ? " AND instr(search_track,?)>0"
                                                         : " AND instr(search_album,?)>0";
            values.emplace_back(word, false);
        }
    }
    void bind(Statement& statement) const {
        int index = 1;
        for (const auto& [value, blob] : values) {
            if (blob) {
                statement.blob(index++, value);
            } else {
                statement.text(index++, value);
            }
        }
    }
};

bool audio_path(const std::filesystem::path& path) {
    static constexpr std::array extensions{
        ".flac", ".mp3",  ".ogg", ".oga",  ".opus", ".m4a",  ".mp4", ".aac", ".wv",
        ".wav",  ".rf64", ".w64", ".aiff", ".aif",  ".aifc", ".ape", ".mpc", ".tta",
        ".spx",  ".mka",  ".wma", ".dsf",  ".dff",  ".mod",  ".xm",  ".s3m", ".it"};
    const auto extension = lower(path.extension().native());
    return std::ranges::find(extensions, extension) != extensions.end();
}

} // namespace

struct LocalLibrary::Impl {
    sqlite3* db{nullptr};
    ~Impl() {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }
};

LocalLibrary::LocalLibrary(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
LocalLibrary::LocalLibrary(LocalLibrary&&) noexcept = default;
LocalLibrary& LocalLibrary::operator=(LocalLibrary&&) noexcept = default;
LocalLibrary::~LocalLibrary() = default;

namespace {

// Migration 35 added the content-identity hash columns, but the scan only
// re-prepares changed files, so pre-existing rows would never join the
// rating store. Rows with complete field evidence rebuild their identity
// from the indexed values here; incomplete rows wait for their explicit
// Refresh, exactly like field search does. Idempotent and cheap once done.
void backfill_rating_identities(sqlite3* db) {
    {
        Statement pending{db, "SELECT 1 FROM local_library_tracks WHERE rating_hash='' AND "
                              "field_index_complete=1 LIMIT 1"};
        if (!pending.next()) {
            return;
        }
    }
    std::vector<std::string> paths;
    {
        Statement select{db, "SELECT raw_path FROM local_library_tracks WHERE rating_hash='' "
                             "AND field_index_complete=1"};
        while (select.next()) {
            paths.push_back(select.bytes(0));
        }
    }
    Transaction transaction{db};
    Statement fields{db, "SELECT canonical_name,value FROM local_library_fields WHERE raw_path=? "
                         "ORDER BY canonical_name,position"};
    Statement update{db, "UPDATE local_library_tracks SET rating_hash=?,album_rating_hash=? "
                         "WHERE raw_path=?"};
    for (const auto& path : paths) {
        metadata::MetadataDocument document;
        fields.reset();
        fields.blob(1, path);
        while (fields.next()) {
            auto name = fields.bytes(0);
            auto value = fields.bytes(1);
            if (!document.fields.empty() && document.fields.back().canonical_name == name) {
                document.fields.back().values.push_back(std::move(value));
            } else {
                document.fields.push_back(metadata::MetadataField{
                    .canonical_name = std::move(name),
                    .native_name = {},
                    .values = {std::move(value)},
                    .qualifier = {},
                    .provenance = metadata::FieldProvenance::cached_snapshot,
                });
            }
        }
        const auto identity = rating_identity(document, path);
        update.reset();
        update.text(1, identity.track_hash);
        update.text(2, identity.album_hash);
        update.blob(3, path);
        update.next();
    }
    transaction.commit();
}

} // namespace

core::Result<LocalLibrary> LocalLibrary::open(const std::filesystem::path& path) {
    return checked([&] {
        auto migrated = ListRepository::open(path);
        if (!migrated) {
            throw migrated.error();
        }
        auto impl = std::make_unique<Impl>();
        if (sqlite3_open_v2(path.c_str(), &impl->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX,
                            nullptr) != SQLITE_OK) {
            fail("Could not open local library");
        }
        sqlite3_busy_timeout(impl->db, 5000);
        execute(impl->db, "PRAGMA foreign_keys=ON");
        // ADR-0151: the index is a WAL-mode cache; NORMAL risks only the
        // final commit on power loss and removes the per-file fsync that
        // dominated large scans. The journals keep synchronous=FULL.
        execute(impl->db, "PRAGMA synchronous=NORMAL");
        backfill_rating_identities(impl->db);
        return LocalLibrary{std::move(impl)};
    });
}

core::Result<std::vector<LibraryRoot>> LocalLibrary::roots() const {
    return checked([&] { return read_roots(implementation_->db); });
}

core::Result<void> LocalLibrary::add_root(const std::string& raw_path) {
    const auto result = checked([&] {
        if (raw_path.empty() || raw_path.find('\0') != std::string::npos ||
            !std::filesystem::path{raw_path}.is_absolute()) {
            fail("Choose an absolute folder path", core::ErrorCode::invalid_argument);
        }
        std::error_code error;
        const auto canonical = std::filesystem::canonical(std::filesystem::path{raw_path}, error);
        if (error || !std::filesystem::is_directory(canonical, error) || error) {
            fail("The library folder is unavailable", core::ErrorCode::io);
        }
        const auto& path = canonical.native();
        auto* db = implementation_->db;
        Transaction transaction{db};
        const auto existing = read_roots(db);
        if (existing.size() >= 64U) {
            fail("At most 64 library folders can be configured", core::ErrorCode::limit_exceeded);
        }
        for (const auto& root : existing) {
            if (contained(path, root.raw_path) || contained(root.raw_path, path)) {
                fail("This folder overlaps an existing library folder", core::ErrorCode::conflict);
            }
        }
        Statement insert{db, "INSERT INTO local_library_roots(raw_path) VALUES(?)"};
        insert.blob(1, path);
        insert.next();
        transaction.commit();
        return true;
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

core::Result<void> LocalLibrary::remove_root(const std::string& path) {
    const auto result = checked([&] {
        Statement remove{implementation_->db, "DELETE FROM local_library_roots WHERE raw_path=?"};
        remove.blob(1, path);
        remove.next();
        return true;
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

core::Result<LibraryPage> LocalLibrary::query(const LibraryQuery& query,
                                              const core::CancellationToken& cancellation) const {
    return checked([&] {
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        Filter filter{query};
        std::string columns;
        std::string order;
        switch (query.kind) {
        case LibraryEntryKind::artist:
            columns = "artist,artist,artist,'',count(*),sum(available),0,"
                      "count(DISTINCT album_key),'',0";
            order = " GROUP BY artist ORDER BY artist COLLATE NOCASE";
            break;
        case LibraryEntryKind::album:
            // Every row of an album shares album_rating_hash, so the bare
            // column (an arbitrary group row) is deterministic here and an
            // aggregate would be rejected inside the correlated subquery.
            columns = "album_key,min(album),min(artist),min(album),count(*),sum(available),0,1,"
                      "album_rating_hash,coalesce((SELECT rating FROM local_ratings "
                      "WHERE hash=album_rating_hash),0)";
            order = " GROUP BY album_key ORDER BY min(artist) COLLATE NOCASE,min(date),min(album) "
                    "COLLATE NOCASE,album_key";
            break;
        case LibraryEntryKind::track:
            columns = "raw_path,title,artist,album,1,available,track,1,rating_hash,"
                      "coalesce((SELECT rating FROM local_ratings WHERE hash=rating_hash),0)";
            order = " ORDER BY artist COLLATE NOCASE,album_key,disc,track,title COLLATE "
                    "NOCASE,raw_path";
            break;
        }
        const auto limit = std::clamp<std::size_t>(query.limit, 1U, 200U);
        Statement statement{db,
                            "SELECT " + columns + " FROM local_library_tracks" + filter.sql +
                                order + " LIMIT " + std::to_string(limit + 1U) + " OFFSET " +
                                std::to_string(std::min<std::size_t>(query.offset, 1'000'000U))};
        filter.bind(statement);
        LibraryPage page;
        while (statement.next()) {
            if (page.entries.size() == limit) {
                page.more = true;
                break;
            }
            page.entries.push_back({query.kind, statement.bytes(0), statement.bytes(1),
                                    statement.bytes(2), statement.bytes(3),
                                    static_cast<std::size_t>(statement.number(4)),
                                    static_cast<std::size_t>(statement.number(5)),
                                    static_cast<int>(statement.number(6)),
                                    static_cast<std::size_t>(statement.number(7)),
                                    statement.bytes(8),
                                    static_cast<unsigned>(statement.number(9))});
            page.entries.back().label = format_label(page.entries.back(), !query.text.empty());
        }
        return page;
    });
}

core::Result<std::vector<std::string>>
LocalLibrary::paths(const LibraryQuery& query, const core::CancellationToken& cancellation) const {
    return checked([&] {
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        const Filter filter{query};
        Statement statement{db, "SELECT raw_path FROM local_library_tracks" + filter.sql +
                                    " AND available=1 ORDER BY artist COLLATE "
                                    "NOCASE,album_key,disc,track,raw_path LIMIT 100001"};
        filter.bind(statement);
        std::vector<std::string> result;
        while (statement.next()) {
            if (result.size() == 100'000U) {
                fail("Selection exceeds 100000 files; select an artist or album",
                     core::ErrorCode::limit_exceeded);
            }
            result.push_back(statement.bytes(0));
        }
        return result;
    });
}

namespace {

constexpr auto filter_columns =
    "t.raw_path,t.title,t.artist,t.album,t.album_key,t.date,t.search_track,t.disc,t.track,"
    "t.codec_name,t.sample_rate,t.bits,t.channels,t.duration_ms,"
    "coalesce((SELECT r.rating FROM local_ratings r WHERE r.hash=t.rating_hash),-1),"
    "coalesce((SELECT r.rating FROM local_ratings r WHERE r.hash=t.album_rating_hash),-1)";
constexpr auto filter_order = " ORDER BY t.artist COLLATE NOCASE,t.album_key,t.disc,t.track,"
                              "t.title COLLATE NOCASE,t.raw_path";
constexpr std::size_t filter_match_cap = 100'000U;

struct FilterPlan {
    std::optional<FilterClause> pushed;
    bool residual{false};
};

[[nodiscard]] FilterPlan plan_filter(const query::CompiledTkq& compiled) {
    FilterPlan plan;
    if (compiled.match_all) {
        return plan;
    }
    if (auto whole = translate_node(compiled, compiled.root)) {
        plan.pushed = std::move(whole);
        return plan;
    }
    plan.residual = true;
    // Pushable AND-conjuncts still pre-filter the candidate stream; the
    // full tree re-evaluates per row, so pushing is purely an optimization.
    const auto& root = compiled.nodes[compiled.root];
    if (root.kind == query::TkqNodeKind::and_node) {
        FilterClause partial;
        std::string sql;
        for (const auto child : root.children) {
            auto translated = translate_node(compiled, child);
            if (!translated) {
                continue;
            }
            if (!sql.empty()) {
                sql += " AND ";
            }
            sql += translated->sql;
            partial.bindings.insert(partial.bindings.end(),
                                    std::make_move_iterator(translated->bindings.begin()),
                                    std::make_move_iterator(translated->bindings.end()));
        }
        if (!sql.empty()) {
            partial.sql = "(" + sql + ")";
            plan.pushed = std::move(partial);
        }
    }
    return plan;
}

void bind_clause(Statement& statement, const FilterClause& clause) {
    int index = 1;
    for (const auto& binding : clause.bindings) {
        if (binding.kind == FilterBinding::Kind::blob) {
            statement.blob(index++, binding.value);
        } else {
            statement.text(index++, binding.value);
        }
    }
}

[[nodiscard]] std::string filter_where(const FilterPlan& plan) {
    std::string where = " WHERE t.available=1";
    if (plan.pushed) {
        where += " AND " + plan.pushed->sql;
    }
    return where;
}

// A candidate row: identity plus the shared evaluation facts.
struct FilterRow {
    std::string raw_path;
    std::string album_key;
    int disc{0};
    int track{0};
    TkqRowFacts facts;
};

void load_field_rows(Statement& statement, const std::string& raw_path, FilterRow& row) {
    statement.reset();
    statement.blob(1, raw_path);
    while (statement.next()) {
        row.facts.fields[statement.bytes(0)].emplace_back(statement.bytes(1), statement.bytes(2));
    }
}

// Materializes the bounded match set in the default library order,
// evaluating residual predicates per row; sorting happens afterwards.
[[nodiscard]] std::vector<FilterRow>
collect_filter_matches(sqlite3* db, const query::CompiledTkq& compiled, const FilterPlan& plan,
                       const core::CancellationToken& cancellation) {
    const auto need_rows = plan.residual || compiled.sort.has_value();
    Statement select{db, std::string{"SELECT "} + filter_columns + " FROM local_library_tracks t" +
                             filter_where(plan) + filter_order};
    if (plan.pushed) {
        bind_clause(select, *plan.pushed);
    }
    Statement fields{db, "SELECT canonical_name,value,value_lower FROM local_library_fields "
                         "WHERE raw_path=? ORDER BY canonical_name,position"};
    std::vector<FilterRow> matches;
    while (select.next()) {
        if (cancellation.is_cancellation_requested()) {
            fail("Library query cancelled", core::ErrorCode::cancelled);
        }
        FilterRow row;
        row.raw_path = select.bytes(0);
        row.facts.title = select.bytes(1);
        row.facts.artist = select.bytes(2);
        row.facts.album = select.bytes(3);
        row.album_key = select.bytes(4);
        row.facts.date = select.bytes(5);
        row.facts.search_text = select.bytes(6);
        row.disc = static_cast<int>(select.number(7));
        row.track = static_cast<int>(select.number(8));
        row.facts.codec = select.bytes(9);
        row.facts.sample_rate = select.number(10);
        row.facts.bits = select.number(11);
        row.facts.channels = select.number(12);
        row.facts.duration_ms = select.number(13);
        row.facts.rating = select.number(14);
        row.facts.album_rating = select.number(15);
        if (need_rows) {
            load_field_rows(fields, row.raw_path, row);
        }
        if (plan.residual && !tkq_matches(compiled, row.facts, cancellation)) {
            continue;
        }
        if (matches.size() == filter_match_cap) {
            fail("The query matches more than 100000 files; narrow it",
                 core::ErrorCode::limit_exceeded);
        }
        matches.push_back(std::move(row));
    }
    if (compiled.sort) {
        struct Keyed {
            std::string key;
            std::size_t position;
        };
        std::vector<Keyed> keyed;
        keyed.reserve(matches.size());
        for (std::size_t position = 0U; position < matches.size(); ++position) {
            auto key = tkq_sort_key(compiled, matches[position].facts, cancellation);
            if (!key) {
                throw key.error();
            }
            keyed.push_back({std::move(*key), position});
        }
        const auto descending = compiled.sort->direction == query::TkqSortDirection::descending;
        // Stable over the default library order, so equal keys keep the
        // deterministic raw_path-terminated ordering as their tiebreaker.
        std::ranges::stable_sort(keyed, [descending](const Keyed& left, const Keyed& right) {
            return descending ? right.key < left.key : left.key < right.key;
        });
        std::vector<FilterRow> sorted;
        sorted.reserve(matches.size());
        for (const auto& entry : keyed) {
            sorted.push_back(std::move(matches[entry.position]));
        }
        matches = std::move(sorted);
    }
    return matches;
}

[[nodiscard]] LibraryEntry filter_entry(const FilterRow& row) {
    LibraryEntry entry{LibraryEntryKind::track,
                       row.raw_path,
                       row.facts.title,
                       row.facts.artist,
                       row.facts.album,
                       1U,
                       1U,
                       row.track};
    entry.label = format_label(entry, true);
    return entry;
}

} // namespace

core::Result<LibraryPage> LocalLibrary::filter(const query::CompiledTkq& compiled,
                                               const std::size_t offset, const std::size_t limit,
                                               const core::CancellationToken& cancellation) const {
    return checked([&] {
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        require_complete_field_index(db);
        const auto plan = plan_filter(compiled);
        const auto page_limit = std::clamp<std::size_t>(limit, 1U, 200U);
        LibraryPage page;
        if (!plan.residual && !compiled.sort) {
            // Fully indexable and unsorted: page in SQL like ordinary queries.
            Statement statement{db, std::string{"SELECT "} + filter_columns +
                                        " FROM local_library_tracks t" + filter_where(plan) +
                                        filter_order + " LIMIT " + std::to_string(page_limit + 1U) +
                                        " OFFSET " +
                                        std::to_string(std::min<std::size_t>(offset, 1'000'000U))};
            if (plan.pushed) {
                bind_clause(statement, *plan.pushed);
            }
            while (statement.next()) {
                if (page.entries.size() == page_limit) {
                    page.more = true;
                    break;
                }
                FilterRow row;
                row.raw_path = statement.bytes(0);
                row.facts.title = statement.bytes(1);
                row.facts.artist = statement.bytes(2);
                row.facts.album = statement.bytes(3);
                row.track = static_cast<int>(statement.number(8));
                page.entries.push_back(filter_entry(row));
            }
            return page;
        }
        const auto matches = collect_filter_matches(db, compiled, plan, cancellation);
        for (auto position = offset; position < matches.size(); ++position) {
            if (page.entries.size() == page_limit) {
                page.more = true;
                break;
            }
            page.entries.push_back(filter_entry(matches[position]));
        }
        return page;
    });
}

core::Result<std::vector<std::string>>
LocalLibrary::filter_paths(const query::CompiledTkq& compiled,
                           const core::CancellationToken& cancellation) const {
    return checked([&] {
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        require_complete_field_index(db);
        const auto plan = plan_filter(compiled);
        if (!plan.residual && !compiled.sort) {
            Statement statement{db, std::string{"SELECT t.raw_path FROM local_library_tracks t"} +
                                        filter_where(plan) + filter_order + " LIMIT 100001"};
            if (plan.pushed) {
                bind_clause(statement, *plan.pushed);
            }
            std::vector<std::string> result;
            while (statement.next()) {
                if (result.size() == filter_match_cap) {
                    fail("The query matches more than 100000 files; narrow it",
                         core::ErrorCode::limit_exceeded);
                }
                result.push_back(statement.bytes(0));
            }
            return result;
        }
        const auto matches = collect_filter_matches(db, compiled, plan, cancellation);
        std::vector<std::string> result;
        result.reserve(matches.size());
        for (const auto& row : matches) {
            result.push_back(row.raw_path);
        }
        return result;
    });
}

core::Result<std::vector<LibraryTrackSnapshot>>
LocalLibrary::cached_tracks(const std::vector<std::string>& raw_paths,
                            const core::CancellationToken& cancellation) const {
    return checked([&] {
        if (raw_paths.size() > filter_match_cap) {
            fail("Selection exceeds 100000 files", core::ErrorCode::limit_exceeded);
        }
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        Transaction snapshot{db, true};
        Statement select{db, std::string{"SELECT "} + filter_columns +
                                 " FROM local_library_tracks t WHERE t.raw_path=?"};
        Statement fields{db, "SELECT canonical_name,value,value_lower FROM local_library_fields "
                             "WHERE raw_path=? ORDER BY canonical_name,position"};
        std::vector<LibraryTrackSnapshot> result;
        result.reserve(raw_paths.size());
        for (const auto& path : raw_paths) {
            if (cancellation.is_cancellation_requested()) {
                fail("Library query cancelled", core::ErrorCode::cancelled);
            }
            select.reset();
            select.blob(1, path);
            if (!select.next()) {
                fail("A search result is no longer indexed; run the search again",
                     core::ErrorCode::conflict);
            }
            FilterRow row;
            row.facts.title = select.bytes(1);
            row.facts.artist = select.bytes(2);
            row.facts.album = select.bytes(3);
            row.facts.date = select.bytes(5);
            row.facts.search_text = select.bytes(6);
            row.facts.codec = select.bytes(9);
            row.facts.sample_rate = select.number(10);
            row.facts.bits = select.number(11);
            row.facts.channels = select.number(12);
            row.facts.duration_ms = select.number(13);
            row.facts.rating = select.number(14);
            row.facts.album_rating = select.number(15);
            load_field_rows(fields, path, row);
            result.push_back({path, std::move(row.facts)});
        }
        snapshot.commit();
        return result;
    });
}

core::Result<void> LocalLibrary::set_rating(const std::string& hash, const bool album,
                                            const unsigned rating) {
    const auto result = checked([&] {
        if (hash.size() != 64U ||
            hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
            fail("Rating identity must be a 64-character content hash",
                 core::ErrorCode::invalid_argument);
        }
        if (rating > 10U) {
            fail("Track and album ratings must be between 0 and 10",
                 core::ErrorCode::invalid_argument);
        }
        auto* db = implementation_->db;
        if (rating == 0U) {
            Statement remove{db, "DELETE FROM local_ratings WHERE hash=?"};
            remove.text(1, hash);
            remove.next();
            return true;
        }
        Statement upsert{db, "INSERT INTO local_ratings(hash,type,rating,updated_at) "
                             "VALUES(?,?,?,datetime('now')) ON CONFLICT(hash) DO UPDATE SET "
                             "rating=excluded.rating,updated_at=excluded.updated_at"};
        upsert.text(1, hash);
        upsert.text(2, album ? "album" : "track");
        upsert.number(3, static_cast<int>(rating));
        upsert.next();
        return true;
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

core::Result<std::vector<unsigned>>
LocalLibrary::ratings(const std::vector<std::string>& hashes,
                      const core::CancellationToken& cancellation) const {
    return checked([&] {
        if (hashes.size() > filter_match_cap) {
            fail("Selection exceeds 100000 files", core::ErrorCode::limit_exceeded);
        }
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        Statement select{db, "SELECT rating FROM local_ratings WHERE hash=?"};
        std::vector<unsigned> result;
        result.reserve(hashes.size());
        for (const auto& hash : hashes) {
            if (cancellation.is_cancellation_requested()) {
                fail("Library query cancelled", core::ErrorCode::cancelled);
            }
            select.reset();
            select.text(1, hash);
            result.push_back(select.next() ? static_cast<unsigned>(select.number(0)) : 0U);
        }
        return result;
    });
}

core::Result<std::optional<std::string>>
LocalLibrary::artwork_source(const std::string& album_key,
                             const core::CancellationToken& cancellation) const {
    return checked([&]() -> std::optional<std::string> {
        auto* db = implementation_->db;
        QueryCancellation guard{db, cancellation};
        Statement source{db, "SELECT raw_path FROM local_library_tracks WHERE album_key=? "
                             "AND available=1 ORDER BY disc,track,raw_path LIMIT 1"};
        source.text(1, album_key);
        if (!source.next()) {
            return std::nullopt;
        }
        return source.bytes(0);
    });
}

namespace {

// ADR-0151: the expensive per-file preparation (probe, metadata read,
// tag/technical extraction) runs on a bounded worker pool; the walk and
// every database commit stay on the scan thread.
struct PreparedFile {
    std::string raw_path;
    std::string root;
    std::string revision;
    core::LocalSourceRevision before{};
    Tags tags;
    metadata::MetadataDocument document;
    Technicals technicals;
    bool failed{false};
};

struct ScanRequest {
    std::string raw_path;
    std::string root;
    std::string revision;
    core::LocalSourceRevision before{};
};

class PreparationPipeline {
  public:
    PreparationPipeline(const std::size_t worker_count, core::CancellationToken cancellation)
        : cancellation_(std::move(cancellation)), capacity_(worker_count * 2U) {
        workers_.reserve(worker_count);
        for (std::size_t index = 0U; index < worker_count; ++index) {
            workers_.emplace_back([this] { work(); });
        }
    }
    ~PreparationPipeline() {
        {
            const std::scoped_lock lock{mutex_};
            input_closed_ = true;
            abandoned_ = true;
        }
        request_ready_.notify_all();
        result_ready_.notify_all();
        for (auto& worker : workers_) {
            worker.join();
        }
    }
    PreparationPipeline(const PreparationPipeline&) = delete;
    PreparationPipeline& operator=(const PreparationPipeline&) = delete;

    // Blocks while both queues are full so in-flight memory stays bounded;
    // returns false once cancellation is requested.
    [[nodiscard]] bool submit(ScanRequest request) {
        std::unique_lock lock{mutex_};
        while (requests_.size() >= capacity_ && results_.size() >= capacity_) {
            if (cancelled()) {
                return false;
            }
            request_taken_.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (cancelled()) {
            return false;
        }
        requests_.push_back(std::move(request));
        ++in_flight_;
        lock.unlock();
        request_ready_.notify_one();
        return true;
    }

    void finish_input() {
        {
            const std::scoped_lock lock{mutex_};
            input_closed_ = true;
        }
        request_ready_.notify_all();
    }

    // Non-blocking drain while the walk continues.
    [[nodiscard]] std::optional<PreparedFile> try_next() {
        const std::scoped_lock lock{mutex_};
        if (results_.empty()) {
            return std::nullopt;
        }
        auto result = std::move(results_.front());
        results_.pop_front();
        --in_flight_;
        request_taken_.notify_all();
        return result;
    }

    // Blocking drain after finish_input(); nullopt when the pipeline is
    // empty or cancellation was requested.
    [[nodiscard]] std::optional<PreparedFile> next() {
        std::unique_lock lock{mutex_};
        while (results_.empty()) {
            if (cancelled() || (requests_.empty() && in_flight_ == 0U && input_closed_)) {
                return std::nullopt;
            }
            result_ready_.wait_for(lock, std::chrono::milliseconds(100));
        }
        auto result = std::move(results_.front());
        results_.pop_front();
        --in_flight_;
        request_taken_.notify_all();
        return result;
    }

  private:
    [[nodiscard]] bool cancelled() const {
        return abandoned_ || cancellation_.is_cancellation_requested();
    }

    void work() {
        while (true) {
            ScanRequest request;
            {
                std::unique_lock lock{mutex_};
                while (requests_.empty()) {
                    if (cancelled() || input_closed_) {
                        return;
                    }
                    request_ready_.wait_for(lock, std::chrono::milliseconds(100));
                }
                if (cancelled()) {
                    return;
                }
                request = std::move(requests_.front());
                requests_.pop_front();
            }
            PreparedFile prepared;
            prepared.raw_path = std::move(request.raw_path);
            prepared.root = std::move(request.root);
            prepared.revision = std::move(request.revision);
            prepared.before = request.before;
            auto probe = formats::probe_local_media(prepared.raw_path, cancellation_);
            if (!probe || !probe->best_audio_stream) {
                prepared.failed = true;
            } else {
                prepared.technicals = technicals_from(*probe);
                const auto read = metadata::read_local_metadata(prepared.raw_path, cancellation_);
                if (read) {
                    prepared.document = read->document;
                } else if (read.error().code != core::ErrorCode::unsupported) {
                    prepared.failed = true;
                }
                for (const auto& tag : probe->tags) {
                    const auto identity = metadata::resolve_text_property_identity(tag.name);
                    if (!prepared.document.first_effective_value(identity.canonical_name)) {
                        prepared.document.fields.push_back(
                            {.canonical_name = identity.canonical_name,
                             .native_name = tag.name,
                             .values = {tag.value},
                             .qualifier = {},
                             .provenance = metadata::FieldProvenance::stream});
                    }
                }
                prepared.tags = tags_from(prepared.document, prepared.raw_path);
            }
            {
                const std::scoped_lock lock{mutex_};
                results_.push_back(std::move(prepared));
            }
            result_ready_.notify_all();
        }
    }

    core::CancellationToken cancellation_;
    std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable request_ready_;
    std::condition_variable request_taken_;
    std::condition_variable result_ready_;
    std::deque<ScanRequest> requests_;
    std::deque<PreparedFile> results_;
    std::size_t in_flight_{0U};
    bool input_closed_{false};
    bool abandoned_{false};
    std::vector<std::thread> workers_;
};

[[nodiscard]] std::size_t scan_worker_count() {
    const auto hardware = std::thread::hardware_concurrency();
    return std::clamp<std::size_t>(hardware == 0U ? 2U : hardware / 2U, 2U, 8U);
}

} // namespace

core::Result<LibraryScanResult> LocalLibrary::scan(const core::CancellationToken& cancellation,
                                                   LibraryScanProgress& progress) {
    return checked([&] {
        auto* db = implementation_->db;
        const auto generation = core::StableId::random().to_string();
        LibraryScanResult result;
        for (const auto& root : read_roots(db)) {
            if (cancellation.is_cancellation_requested()) {
                result.cancelled = true;
                break;
            }
            std::error_code error;
            std::filesystem::recursive_directory_iterator iterator{
                std::filesystem::path{root.raw_path}, error};
            if (error) {
                Transaction transaction{db};
                Statement offline{db, "UPDATE local_library_roots SET "
                                      "available=0,error=?,scan_token=? WHERE raw_path=?"};
                offline.text(1, error.message());
                offline.text(2, generation);
                offline.blob(3, root.raw_path);
                offline.next();
                Statement missing{db, "UPDATE local_library_tracks SET available=0 WHERE root=?"};
                missing.blob(1, root.raw_path);
                missing.next();
                transaction.commit();
                continue;
            }
            {
                Statement begin{db, "UPDATE local_library_roots SET scan_token=? WHERE raw_path=?"};
                begin.text(1, generation);
                begin.blob(2, root.raw_path);
                begin.next();
            }
            bool complete = true;
            bool root_lost = false;
            PreparationPipeline pipeline{scan_worker_count(), cancellation};
            // Commits one prepared file in its own transaction with the
            // unchanged guards: fresh revision, current root scan token.
            const auto commit_prepared = [&](PreparedFile prepared) {
                if (root_lost) {
                    return;
                }
                if (prepared.failed) {
                    Statement incomplete{
                        db,
                        "UPDATE local_library_tracks SET field_index_complete=0 WHERE raw_path=?"};
                    incomplete.blob(1, prepared.raw_path);
                    incomplete.next();
                    ++progress.failed;
                    complete = false;
                    return;
                }
                Transaction transaction{db};
                // The commit lock serializes this fresh revision check against
                // metadata and relocation publication's dependent-state update.
                const auto after = core::observe_local_source_revision(prepared.raw_path);
                if (!after || prepared.before != *after) {
                    ++progress.failed;
                    complete = false;
                    return;
                }
                Statement exists{
                    db, "SELECT 1 FROM local_library_roots WHERE raw_path=? AND scan_token=?"};
                exists.blob(1, prepared.root);
                exists.text(2, generation);
                if (!exists.next()) {
                    root_lost = true;
                    complete = false;
                    return;
                }
                const auto identity = rating_identity(prepared.document, prepared.raw_path);
                Statement upsert{
                    db, "INSERT INTO "
                        "local_library_tracks(raw_path,root,revision,title,artist,album,album_key,"
                        "release_id,date,disc,track,search_track,search_album,available,seen,"
                        "codec_name,sample_rate,bits,channels,duration_ms,"
                        "rating_hash,album_rating_hash) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?,?,?,?,?) "
                        "ON CONFLICT(raw_path) DO UPDATE SET "
                        "root=excluded.root,revision=excluded.revision,title=excluded.title,artist="
                        "excluded.artist,album=excluded.album,album_key=excluded.album_key,"
                        "release_id=excluded.release_id,date=excluded.date,disc=excluded.disc,"
                        "track=excluded.track,search_track=excluded.search_track,search_album="
                        "excluded.search_album,available=1,seen=excluded.seen,"
                        "codec_name=excluded.codec_name,sample_rate=excluded.sample_rate,"
                        "bits=excluded.bits,channels=excluded.channels,"
                        "duration_ms=excluded.duration_ms,rating_hash=excluded.rating_hash,"
                        "album_rating_hash=excluded.album_rating_hash"};
                upsert.blob(1, prepared.raw_path);
                upsert.blob(2, prepared.root);
                upsert.text(3, prepared.revision);
                bind_tags(upsert, 4, prepared.tags, prepared.raw_path);
                upsert.text(14, generation);
                upsert.text(15, prepared.technicals.codec);
                upsert.number(16, prepared.technicals.sample_rate);
                upsert.number(17, prepared.technicals.bits);
                upsert.number(18, prepared.technicals.channels);
                upsert.number(19, prepared.technicals.duration_ms);
                upsert.text(20, identity.track_hash);
                upsert.text(21, identity.album_hash);
                upsert.next();
                write_field_rows(db, prepared.raw_path, prepared.document);
                ++progress.indexed;
                transaction.commit();
            };
            for (; iterator != std::filesystem::recursive_directory_iterator{};
                 iterator.increment(error)) {
                if (error) {
                    complete = false;
                    break;
                }
                if (cancellation.is_cancellation_requested()) {
                    result.cancelled = true;
                    complete = false;
                    break;
                }
                if (++progress.visited > 1'000'000U) {
                    complete = false;
                    break;
                }
                const auto path = iterator->path();
                const auto status = iterator->symlink_status(error);
                if (error) {
                    complete = false;
                    break;
                }
                if (!std::filesystem::is_regular_file(status) || !audio_path(path)) {
                    continue;
                }
                const auto& raw = path.native();
                auto before = core::observe_local_source_revision(raw);
                if (!before) {
                    ++progress.failed;
                    complete = false;
                    continue;
                }
                const auto revision = revision_key(*before);
                bool unchanged = false;
                {
                    // Rows still missing their migration-35 rating identity
                    // re-prepare even when the file itself is unchanged.
                    Statement previous{db, "SELECT revision,field_index_complete,rating_hash FROM "
                                           "local_library_tracks WHERE raw_path=?"};
                    previous.blob(1, raw);
                    unchanged = previous.next() && previous.bytes(0) == revision &&
                                previous.number(1) == 1 && !previous.bytes(2).empty();
                }
                if (unchanged) {
                    Transaction transaction{db};
                    const auto after = core::observe_local_source_revision(raw);
                    if (!after || *before != *after) {
                        ++progress.failed;
                        complete = false;
                        continue;
                    }
                    Statement exists{
                        db, "SELECT 1 FROM local_library_roots WHERE raw_path=? AND scan_token=?"};
                    exists.blob(1, root.raw_path);
                    exists.text(2, generation);
                    if (!exists.next()) {
                        complete = false;
                        break;
                    }
                    Statement touch{db, "UPDATE local_library_tracks SET available=1,seen=? WHERE "
                                        "raw_path=? AND revision=?"};
                    touch.text(1, generation);
                    touch.blob(2, raw);
                    touch.text(3, revision);
                    touch.next();
                    transaction.commit();
                } else if (!pipeline.submit({raw, root.raw_path, revision, *before})) {
                    result.cancelled = true;
                    complete = false;
                    break;
                }
                // Drain finished preparations without stalling the walk.
                while (auto prepared = pipeline.try_next()) {
                    commit_prepared(std::move(*prepared));
                }
                if (root_lost) {
                    break;
                }
            }
            pipeline.finish_input();
            while (auto prepared = pipeline.next()) {
                commit_prepared(std::move(*prepared));
            }
            if (cancellation.is_cancellation_requested()) {
                result.cancelled = true;
            }
            if (error || cancellation.is_cancellation_requested()) {
                complete = false;
            }
            result.incomplete = result.incomplete || !complete;
            Transaction transaction{db};
            Statement state{db, "UPDATE local_library_roots SET available=1,error=? WHERE "
                                "raw_path=? AND scan_token=?"};
            state.text(1, complete ? "" : "Scan incomplete; previous entries retained");
            state.blob(2, root.raw_path);
            state.text(3, generation);
            state.next();
            if (complete) {
                Statement missing{
                    db, "UPDATE local_library_tracks SET available=0 WHERE root=? AND seen<>? "
                        "AND EXISTS(SELECT 1 FROM local_library_roots WHERE raw_path=root AND "
                        "scan_token=?)"};
                missing.blob(1, root.raw_path);
                missing.text(2, generation);
                missing.text(3, generation);
                missing.next();
            }
            transaction.commit();
            if (complete) {
                prune_missing(db, root.raw_path, generation, cancellation);
            }
        }
        result.cancelled = result.cancelled || cancellation.is_cancellation_requested();
        return result;
    });
}

core::Result<void> refresh_library_source(sqlite3* db, const std::string& source,
                                          const std::string& target,
                                          const metadata::MetadataDocument* document) {
    const auto result = checked([&] {
        Tags tags;
        {
            Statement existing{db,
                               "SELECT title,artist,album,release_id,date,disc,track,search_track "
                               "FROM local_library_tracks WHERE raw_path=?"};
            existing.blob(1, source);
            if (!existing.next()) {
                return true;
            }
            tags = {existing.bytes(0),
                    existing.bytes(1),
                    existing.bytes(2),
                    existing.bytes(3),
                    existing.bytes(4),
                    existing.bytes(7),
                    static_cast<int>(existing.number(5)),
                    static_cast<int>(existing.number(6))};
        }
        std::string root;
        for (const auto& candidate : read_roots(db)) {
            if (contained(target, candidate.raw_path)) {
                root = candidate.raw_path;
                break;
            }
        }
        if (root.empty()) {
            Statement remove{db, "DELETE FROM local_library_tracks WHERE raw_path=?"};
            remove.blob(1, source);
            remove.next();
            return true;
        }
        if (document != nullptr) {
            tags = tags_from(*document, target);
        }
        if (source != target) {
            Statement remove{db, "DELETE FROM local_library_tracks WHERE raw_path=?"};
            remove.blob(1, target);
            remove.next();
        }
        // ADR-0150: field rows reference the track row, and the rename below
        // would strand them (enforced foreign keys reject an updated parent
        // key with surviving children). Carry a move's rows across in memory;
        // a tag commit rebuilds them from the fresh document afterwards.
        struct FieldRow {
            std::string canonical_name;
            int position;
            std::string value;
            std::string value_lower;
        };
        std::vector<FieldRow> retained;
        if (document == nullptr) {
            Statement select{db, "SELECT canonical_name,position,value,value_lower FROM "
                                 "local_library_fields WHERE raw_path=?"};
            select.blob(1, source);
            while (select.next()) {
                retained.push_back({select.bytes(0), static_cast<int>(select.number(1)),
                                    select.bytes(2), select.bytes(3)});
            }
        }
        {
            Statement remove{db, "DELETE FROM local_library_fields WHERE raw_path=?"};
            remove.blob(1, source);
            remove.next();
        }
        Statement update{
            db,
            "UPDATE local_library_tracks SET "
            "raw_path=?,root=?,revision='',title=?,artist=?,album=?,album_key=?,release_id=?,"
            "date=?,disc=?,track=?,search_track=?,search_album=?,available=1,"
            "seen=(SELECT scan_token FROM local_library_roots WHERE raw_path=?) WHERE raw_path=?"};
        update.blob(1, target);
        update.blob(2, root);
        bind_tags(update, 3, tags, target);
        update.blob(13, root);
        update.blob(14, source);
        update.next();
        if (document != nullptr) {
            // The rating identity follows tags; a pure move keeps it, while a
            // committed tag change recomputes it from the fresh document.
            const auto identity = rating_identity(*document, target);
            Statement hashes{db, "UPDATE local_library_tracks SET rating_hash=?,"
                                 "album_rating_hash=? WHERE raw_path=?"};
            hashes.text(1, identity.track_hash);
            hashes.text(2, identity.album_hash);
            hashes.blob(3, target);
            hashes.next();
            write_field_rows(db, target, *document);
        } else if (!retained.empty()) {
            Statement insert{db, "INSERT OR IGNORE INTO local_library_fields"
                                 "(raw_path,canonical_name,position,value,value_lower) "
                                 "VALUES(?,?,?,?,?)"};
            for (const auto& row : retained) {
                insert.reset();
                insert.blob(1, target);
                insert.text(2, row.canonical_name);
                insert.number(3, row.position);
                insert.blob(4, row.value);
                insert.blob(5, row.value_lower);
                insert.next();
            }
        }
        return true;
    });
    if (!result) {
        return std::unexpected(result.error());
    }
    return {};
}

} // namespace trackknife::persistence
